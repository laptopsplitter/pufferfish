#include "script.hpp"

#include <cerrno>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

#include "fakejni.hpp"
#include "shims.hpp"

namespace tonic {

namespace {

using u64 = uint64_t;

// Splits on whitespace; "quoted text" is one token, with \" \\ \n \t escapes.
bool tokenize(const std::string& s, std::vector<NativeCall::Arg>& out, std::string& err) {
  size_t i = 0;
  while (i < s.size()) {
    if (isspace(static_cast<unsigned char>(s[i]))) {
      ++i;
      continue;
    }
    if (s[i] == '#') break;
    NativeCall::Arg a{"", s[i] == '"'};
    if (a.quoted) {
      ++i;
      while (i < s.size() && s[i] != '"') {
        if (s[i] == '\\' && i + 1 < s.size()) {
          const char c = s[++i];
          a.text += c == 'n' ? '\n' : c == 't' ? '\t' : c;
        } else {
          a.text += s[i];
        }
        ++i;
      }
      if (i >= s.size()) {
        err = "unterminated string";
        return false;
      }
      ++i;
    } else {
      while (i < s.size() && !isspace(static_cast<unsigned char>(s[i]))) a.text += s[i++];
    }
    out.push_back(a);
  }
  return true;
}

// Splits a method descriptor into parameter descriptors and the return type.
bool split_desc(const std::string& d, std::vector<std::string>& params, std::string& ret) {
  if (d.empty() || d[0] != '(') return false;
  size_t i = 1;
  while (i < d.size() && d[i] != ')') {
    size_t j = i;
    while (j < d.size() && d[j] == '[') ++j;
    if (j >= d.size()) return false;
    if (d[j] == 'L') {
      j = d.find(';', j);
      if (j == std::string::npos) return false;
    } else if (!strchr("ZBSCIJFD", d[j])) {
      return false;
    }
    params.push_back(d.substr(i, j + 1 - i));
    i = j + 1;
  }
  if (i >= d.size()) return false;
  ret = d.substr(i + 1);
  return !ret.empty();
}

void mangle_into(std::string& out, const std::string& s) {
  for (size_t i = 0; i < s.size(); ++i) {
    const unsigned char c = s[i];
    if (isalnum(c)) out += static_cast<char>(c);
    else if (c == '/') out += '_';
    else if (c == '_') out += "_1";
    else if (c == ';') out += "_2";
    else if (c == '[') out += "_3";
    else {
      // UTF-8 to one UTF-16 unit (BMP only; enough for class and method names).
      unsigned cp = c;
      if ((c & 0xE0) == 0xC0 && i + 1 < s.size()) cp = ((c & 0x1F) << 6) | (s[++i] & 0x3F);
      else if ((c & 0xF0) == 0xE0 && i + 2 < s.size()) {
        cp = ((c & 0x0F) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F);
        i += 2;
      }
      char buf[8];
      snprintf(buf, sizeof buf, "_0%04x", cp & 0xFFFF);
      out += buf;
    }
  }
}

bool is_fp(const std::string& t) { return t == "F" || t == "D"; }

// One argument as the 64-bit register/stack value the callee will read.
// ${NAME} in quoted strings and file: paths (set_script_var): the data
// folder, the APK's assets, its version.
std::map<std::string, std::string>& script_vars() {
  static std::map<std::string, std::string> v;
  return v;
}
std::string expand(const std::string& s) {
  if (s.find("${") == std::string::npos) return s;
  std::string out;
  for (size_t i = 0; i < s.size();) {
    const size_t a = s.find("${", i);
    if (a == std::string::npos) {
      out += s.substr(i);
      break;
    }
    const size_t b = s.find('}', a);
    if (b == std::string::npos) {
      out += s.substr(i);
      break;
    }
    out += s.substr(i, a - i);
    const auto it = script_vars().find(s.substr(a + 2, b - a - 2));
    out += it != script_vars().end() ? it->second : s.substr(a, b - a + 1);
    i = b + 1;
  }
  return out;
}

bool convert(const std::string& type, const NativeCall::Arg* a, u64& v, std::string& shown, std::string& err) {
  v = 0;
  if (!a) {
    shown = is_fp(type) ? "0.0" : type.size() == 1 ? "0" : "null";
    return true;
  }
  const char* s = a->text.c_str();
  char* end = nullptr;
  errno = 0;
  if (type.size() > 1) {  // object or array
    if (a->quoted) {
      const std::string t = expand(a->text);
      v = reinterpret_cast<u64>(jni_new_string(t));
      shown = "\"" + t + "\"";
      if (type != "Ljava/lang/String;" && type != "Ljava/lang/CharSequence;" && type != "Ljava/lang/Object;")
        fprintf(stderr, "[native]   note: passing a String where %s is expected\n", type.c_str());
      return true;
    }
    if (a->text == "null") {
      shown = "null";
      return true;
    }
    if (a->text.rfind("file:", 0) == 0) {  // a String holding a file's contents
      const std::string path = expand(a->text.substr(5));
      std::ifstream f(path, std::ios::binary);
      if (!f) {
        err = "cannot read " + path;
        return false;
      }
      const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
      v = reinterpret_cast<u64>(jni_new_string(text));
      shown = "contents of " + path + " (" + std::to_string(text.size()) + " bytes)";
      return true;
    }
    if (a->text == "new" || a->text.rfind("new:", 0) == 0) {
      if (type[0] == '[') {
        err = "'new' cannot make arrays yet; pass null";
        return false;
      }
      const std::string cn = type.substr(1, type.size() - 2);
      const std::string label = a->text.size() > 4 ? a->text.substr(4) : "";
      v = reinterpret_cast<u64>(jni_new_object(cn, label));
      shown = "new " + cn + (label.empty() ? "" : "#" + label);
      return true;
    }
    err = "argument '" + a->text + "' for " + type + ": use \"text\", null or new";
    return false;
  }
  if (a->quoted) {
    err = "a string was given for a " + type + " parameter";
    return false;
  }
  switch (type[0]) {
    case 'Z':
      if (a->text == "true" || a->text == "1") v = 1;
      else if (a->text != "false" && a->text != "0") {
        err = "boolean must be true or false, got " + a->text;
        return false;
      }
      shown = v ? "true" : "false";
      return true;
    case 'F': {
      const float f = strtof(s, &end);
      uint32_t bits;
      memcpy(&bits, &f, 4);
      v = bits;
      break;
    }
    case 'D': {
      const double d = strtod(s, &end);
      memcpy(&v, &d, 8);
      break;
    }
    default:  // B S C I J
      v = static_cast<u64>(strtoll(s, &end, 0));
      if (type != "J") v = static_cast<u64>(static_cast<int64_t>(static_cast<int32_t>(v)));
      break;
  }
  if (end == s || *end || errno) {
    err = "cannot read '" + a->text + "' as " + type;
    return false;
  }
  shown = a->text;
  return true;
}

// Every argument the SysV x86_64 ABI can put in a register, plus 16 stack
// slots. Unused ones are ignored by the callee. Integer-class arguments
// (including env and the receiver) fill rdi..r9, float/double fill xmm0..7,
// and whatever does not fit goes on the stack, 8 bytes each, in order.
constexpr int kStack = 16;
using FnInt = u64 (*)(u64, u64, u64, u64, u64, u64, double, double, double, double, double, double, double,
                      double, u64, u64, u64, u64, u64, u64, u64, u64, u64, u64, u64, u64, u64, u64, u64, u64);
using FnFp = double (*)(u64, u64, u64, u64, u64, u64, double, double, double, double, double, double, double,
                        double, u64, u64, u64, u64, u64, u64, u64, u64, u64, u64, u64, u64, u64, u64, u64, u64);

double as_double(u64 v) {
  double d;
  memcpy(&d, &v, 8);
  return d;
}

std::string format_result(const std::string& ret, u64 rax, double xmm0) {
  char buf[64];
  switch (ret[0]) {
    case 'V':
      return "(void)";
    case 'Z':
      return (rax & 0xFF) ? "true" : "false";
    case 'B':
      snprintf(buf, sizeof buf, "%d", static_cast<int8_t>(rax));
      return buf;
    case 'S':
      snprintf(buf, sizeof buf, "%d", static_cast<int16_t>(rax));
      return buf;
    case 'C':
      snprintf(buf, sizeof buf, "%u", static_cast<uint16_t>(rax));
      return buf;
    case 'I':
      snprintf(buf, sizeof buf, "%d", static_cast<int32_t>(rax));
      return buf;
    case 'J':
      snprintf(buf, sizeof buf, "%" PRId64, static_cast<int64_t>(rax));
      return buf;
    case 'F': {
      float f;
      memcpy(&f, &xmm0, 4);  // low 32 bits of xmm0
      snprintf(buf, sizeof buf, "%g", f);
      return buf;
    }
    case 'D':
      snprintf(buf, sizeof buf, "%g", xmm0);
      return buf;
    default:
      return jni_describe(reinterpret_cast<void*>(rax));
  }
}

}  // namespace

void set_script_var(const std::string& name, const std::string& value) { script_vars()[name] = value; }

std::string jni_short_name(const std::string& cls, const std::string& name) {
  std::string s = "Java_";
  mangle_into(s, cls);
  s += '_';
  mangle_into(s, name);
  return s;
}

void* find_native(const ElfImage& img, const std::string& cls, const std::string& name, const std::string& desc) {
  if (u64 fn = jni_registered_native(cls, name, desc)) return reinterpret_cast<void*>(fn);
  if (void* p = img.find_symbol(jni_short_name(cls, name).c_str())) return p;
  return img.find_symbol(jni_long_name(cls, name, desc).c_str());
}

std::string jni_long_name(const std::string& cls, const std::string& name, const std::string& desc) {
  std::string s = jni_short_name(cls, name) + "__";
  mangle_into(s, desc.substr(1, desc.find(')') - 1));
  return s;
}

namespace {
using Builtin = std::function<void(const ElfImage&, const std::vector<std::string>&)>;
std::map<std::string, Builtin>& builtins() {
  static auto* m = new std::map<std::string, Builtin>;
  return *m;
}
}  // namespace

void register_script_builtin(const std::string& name, Builtin fn) {
  builtins()[name] = std::move(fn);
}

bool parse_native_call(const std::string& line, const std::string& where, NativeCall& out, std::string& err) {
  std::vector<NativeCall::Arg> tok;
  if (!tokenize(line, tok, err)) return false;
  out = NativeCall{};
  out.where = where;
  if (!tok.empty() && !tok[0].quoted && tok[0].text == "tonic") {
    if (tok.size() < 2) {
      err = "expected: tonic NAME [ARG ...]";
      return false;
    }
    out.builtin = tok[1].text;
    for (size_t k = 2; k < tok.size(); ++k) out.builtin_args.push_back(tok[k].text);
    return true;
  }
  size_t i = 0;
  if (tok.size() >= 2 && !tok[0].quoted && tok[0].text == "on") {
    out.on = tok[1].text;
    i = 2;
  }
  if (i < tok.size() && !tok[i].quoted && tok[i].text == "static") out.is_static = true, ++i;
  if (i >= tok.size()) {
    err = "missing CLASS.METHOD(DESCRIPTOR)";
    return false;
  }
  const std::string& m = tok[i++].text;
  const size_t paren = m.find('(');
  const size_t dot = paren == std::string::npos ? std::string::npos : m.rfind('.', paren);
  if (dot == std::string::npos || dot == 0) {
    err = "expected CLASS.METHOD(DESCRIPTOR)RET, got " + m;
    return false;
  }
  out.cls = m.substr(0, dot);
  for (char& c : out.cls)
    if (c == '.') c = '/';
  out.name = m.substr(dot + 1, paren - dot - 1);
  out.desc = m.substr(paren);
  std::vector<std::string> params;
  std::string ret;
  if (!split_desc(out.desc, params, ret)) {
    err = "bad descriptor " + out.desc;
    return false;
  }
  for (; i < tok.size(); ++i) {
    if (!tok[i].quoted && tok[i].text == "=>") {
      if (i + 2 != tok.size()) {
        err = "'=>' must be followed by exactly one value";
        return false;
      }
      out.has_expect = true;
      out.expect = tok[i + 1].quoted ? "\"" + tok[i + 1].text + "\"" : tok[i + 1].text;
      break;
    }
    out.args.push_back(tok[i]);
  }
  if (out.args.size() > params.size()) {
    err = "too many arguments for " + out.desc;
    return false;
  }
  return true;
}

bool load_script(const std::string& path, std::vector<NativeCall>& out, std::string& err) {
  std::ifstream f(path);
  if (!f) {
    err = "cannot open " + path;
    return false;
  }
  std::string line;
  for (int n = 1; std::getline(f, line); ++n) {
    std::vector<NativeCall::Arg> tok;
    std::string e;
    if (!tokenize(line, tok, e)) {
      err = path + ":" + std::to_string(n) + ": " + e;
      return false;
    }
    if (tok.empty()) continue;
    NativeCall c;
    if (!parse_native_call(line, path + ":" + std::to_string(n), c, e)) {
      err = path + ":" + std::to_string(n) + ": " + e;
      return false;
    }
    out.push_back(std::move(c));
  }
  return true;
}

int run_native_calls(const ElfImage& img, const std::vector<NativeCall>& calls) {
  int failed = 0;
  void* env = jni_env();
  for (const NativeCall& c : calls) {
    if (!c.on.empty()) continue;  // a rule: runs from the call hook
    if (!c.builtin.empty()) {
      printf("\n[native] tonic %s  (%s)\n", c.builtin.c_str(), c.where.c_str());
      auto it = builtins().find(c.builtin);
      if (it == builtins().end()) {
        printf("[native]   UNKNOWN tonic step \"%s\"\n", c.builtin.c_str());
        ++failed;
      } else {
        fflush(stdout);
        it->second(img, c.builtin_args);
      }
      continue;
    }
    const std::string title = std::string(c.is_static ? "static " : "") + c.cls + "." + c.name + c.desc;
    printf("\n[native] %s  (%s)\n", title.c_str(), c.where.c_str());

    // Registered natives first: on Android a RegisterNatives binding is used
    // even when a Java_ symbol with the same name is exported.
    u64 fn = jni_registered_native(c.cls, c.name, c.desc);
    const char* how = "RegisterNatives";
    std::string sym;
    if (!fn) {
      sym = jni_short_name(c.cls, c.name);
      fn = reinterpret_cast<u64>(img.find_symbol(sym.c_str()));
      how = "symbol";
    }
    if (!fn) {
      sym = jni_long_name(c.cls, c.name, c.desc);
      fn = reinterpret_cast<u64>(img.find_symbol(sym.c_str()));
    }
    if (!fn) {
      printf("[native]   NOT FOUND: not registered, and no %s or %s exported\n",
             jni_short_name(c.cls, c.name).c_str(), sym.c_str());
      ++failed;
      continue;
    }
    char where[300];
    describe_addr(fn, where, sizeof where);
    printf("[native]   -> %s (%s%s%s)\n", where, how, sym.empty() ? "" : " ", sym.c_str());

    std::vector<std::string> params;
    std::string ret;
    split_desc(c.desc, params, ret);
    u64 ints[6] = {reinterpret_cast<u64>(env),
                   reinterpret_cast<u64>(c.is_static ? jni_class(c.cls) : jni_new_object(c.cls)), 0, 0, 0, 0};
    double fps[8] = {};
    u64 stack[kStack] = {};
    int ni = 2, nf = 0, ns = 0;
    std::string shown_all, err;
    bool ok = true;
    for (size_t k = 0; k < params.size() && ok; ++k) {
      u64 v;
      std::string shown;
      if (!convert(params[k], k < c.args.size() ? &c.args[k] : nullptr, v, shown, err)) {
        ok = false;
        break;
      }
      shown_all += (k ? ", " : "") + shown;
      if (is_fp(params[k]) && nf < 8) fps[nf++] = as_double(v);
      else if (!is_fp(params[k]) && ni < 6) ints[ni++] = v;
      else if (ns < kStack) stack[ns++] = v;
      else {
        err = "too many arguments for the call thunk";
        ok = false;
      }
    }
    if (!ok) {
      printf("[native]   BAD ARGUMENTS: %s\n", err.c_str());
      ++failed;
      continue;
    }
    printf("[native]   %s(%s)\n", c.name.c_str(), shown_all.c_str());
    fflush(stdout);
    fflush(stderr);

    u64 rax = 0;
    double xmm0 = 0;
#define TONIC_ARGS                                                                                             \
  ints[0], ints[1], ints[2], ints[3], ints[4], ints[5], fps[0], fps[1], fps[2], fps[3], fps[4], fps[5], fps[6], \
      fps[7], stack[0], stack[1], stack[2], stack[3], stack[4], stack[5], stack[6], stack[7], stack[8], stack[9], \
      stack[10], stack[11], stack[12], stack[13], stack[14], stack[15]
    if (is_fp(ret)) xmm0 = reinterpret_cast<FnFp>(fn)(TONIC_ARGS);
    else rax = reinterpret_cast<FnInt>(fn)(TONIC_ARGS);
#undef TONIC_ARGS
    fflush(stderr);
    const std::string result = format_result(ret, rax, xmm0);
    if (c.has_expect && result != c.expect) {
      printf("[native]   returned %s, EXPECTED %s\n", result.c_str(), c.expect.c_str());
      ++failed;
    } else {
      printf("[native]   returned %s%s\n", result.c_str(), c.has_expect ? " (as expected)" : "");
    }
    fflush(stdout);
  }
  return failed;
}

// Rules do not run inside the library's callback. Answering from there (on
// the thread that is delivering the request, maybe holding its locks) can
// deadlock: a reply published from inside the Credentials callback never
// returned. Java answers from its own threads, so rules are queued and run in
// order on one worker thread of ours, attached to the fake JVM like any Java
// thread.
void install_rules(const ElfImage& img, const std::vector<NativeCall>& calls) {
  auto rules = std::make_shared<std::vector<NativeCall>>();
  for (const NativeCall& c : calls)
    if (!c.on.empty()) rules->push_back(c);
  if (rules->empty()) return;
  printf("%zu \"on\" rule(s) installed\n", rules->size());

  struct Queue {
    std::mutex mu;
    std::condition_variable cv;
    std::deque<std::pair<NativeCall, std::string>> items;  // call, why
  };
  // Never destroyed: the worker waits on it until the process exits, and
  // destroying a condition variable that has a waiter blocks at exit.
  static Queue& q = *new Queue;
  const ElfImage* image = &img;
  std::thread([image] {
    for (;;) {
      std::unique_lock<std::mutex> l(q.mu);
      q.cv.wait(l, [] { return !q.items.empty(); });
      auto item = std::move(q.items.front());
      q.items.pop_front();
      l.unlock();
      printf("\n%s\n", item.second.c_str());
      run_native_calls(*image, {item.first});
      fflush(stdout);
    }
  }).detach();

  set_jni_call_hook([rules](const std::string& label, const std::string& method, const std::string& args,
                            const std::vector<std::string>& strs) {
    for (const NativeCall& r : *rules) {
      // "LABEL" or "LABEL~TEXT": TEXT must appear in the logged arguments.
      const size_t tilde = r.on.find('~');
      if (r.on.substr(0, tilde) != label) continue;
      if (tilde != std::string::npos && args.find(r.on.substr(tilde + 1)) == std::string::npos) continue;
      NativeCall c = r;
      c.on.clear();
      // $arg1, $arg2, ...: the String arguments the library passed, in order
      // ($arg is $arg1). Higher numbers first, so $arg1 does not eat $arg10.
      for (auto& a : c.args) {
        for (size_t n = strs.size(); n >= 1; --n) {
          const std::string key = "$arg" + std::to_string(n);
          size_t p;
          while ((p = a.text.find(key)) != std::string::npos) a.text.replace(p, key.size(), strs[n - 1]);
        }
        size_t p;
        while ((p = a.text.find("$arg")) != std::string::npos) a.text.replace(p, 4, strs.empty() ? "" : strs[0]);
      }
      std::lock_guard<std::mutex> l(q.mu);
      q.items.emplace_back(std::move(c), "[on " + label + "] " + method + args + " called; running rule (" + r.where + ")");
      q.cv.notify_one();
    }
  });
}

}  // namespace tonic
