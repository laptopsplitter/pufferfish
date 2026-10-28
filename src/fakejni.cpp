#include "platform.hpp"
#include "fakejni.hpp"

#include <unistd.h>

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <fstream>
#include <functional>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "jni_names.inc"
#include "shims.hpp"

namespace tonic {

namespace {

using jint = int32_t;
using jsize = int32_t;
using jboolean = uint8_t;
using jchar = uint16_t;

constexpr jint kJniOk = 0;
constexpr jint kJniVersion16 = 0x00010006;

// ---------------------------------------------------------------------------
// Fake handles. Every jclass/jobject/jmethodID/jfieldID we hand out points at
// one of these, and we remember them, so a pointer we did not create is never
// dereferenced as one of ours.
// ---------------------------------------------------------------------------

struct Class {
  std::string name;
};
struct Object {
  Class* cls;
  std::string utf;    // contents, if this is a java.lang.String
  std::string label;  // name given in a script with new:LABEL, for logs and "on" rules
  // Arrays: a primitive array ("[B", "[I", ...) keeps its bytes in data, an
  // object array ("[L...;") its elements in elems.
  std::vector<uint8_t> data;
  std::vector<void*> elems;
  // Objects the library built with NewObject: the constructor's arguments
  // (raw bits, a float as a double's bits), e.g. NativeTextBoxInfo's fields.
  std::vector<uint64_t> ctor_args;
  // Lifetime, as Java's references would keep it: local and global
  // references the library holds. Freed when both reach 0, unless pinned
  // (objects tonic or a script keeps, e.g. new:LABEL callbacks).
  int locals = 1, globals = 0;
  bool pinned = false;
};
struct Member {
  Class* cls;
  std::string name, sig;
  bool is_static, is_field;
};
struct Native {
  std::string cls, name, sig;
  uint64_t fn;
};

std::recursive_mutex g_mu;
std::map<std::string, Class*> g_classes;
std::unordered_set<const void*> g_objects, g_members;
std::vector<Native> g_natives;
std::map<std::string, int> g_member_lookups;  // "cls.name sig" -> count

Class* class_named(const std::string& n) {
  auto& c = g_classes[n];
  if (!c) c = new Class{n};
  return c;
}
Class* as_class(const void* p) {
  for (auto& [n, c] : g_classes)
    if (c == p) return c;
  return nullptr;
}
Object* as_object(const void* p) { return g_objects.count(p) ? (Object*)p : nullptr; }
Member* as_member(const void* p) { return g_members.count(p) ? (Member*)p : nullptr; }

Object* new_object(Class* c, std::string utf = {}) {
  auto* o = new Object{c, std::move(utf), {}, {}, {}, {}};
  g_objects.insert(o);
  return o;
}

std::string class_of(const void* h) {
  if (Class* c = as_class(h)) return "class " + c->name;
  if (Object* o = as_object(h)) return o->label.empty() ? o->cls->name : o->cls->name + "#" + o->label;
  return h ? "?" : "null";
}

std::string describe(const void* mid) {
  Member* m = as_member(mid);
  if (!m) return "<unknown id>";
  return m->cls->name + "." + m->name + (m->is_field ? " : " : "") + m->sig;
}

// Every line carries the calling thread's id: JNI state is per thread.
// A line the same as the thread's previous one is counted, not printed: the
// library repeats some calls every frame, and writing each one to a terminal
// slows the game down to the terminal's speed.
__attribute__((format(printf, 1, 2))) void jni_log(const char* fmt, ...) {
  thread_local std::string last;
  thread_local long repeats = 0;
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  if (last == buf) {
    ++repeats;
    if (repeats % 100000 == 0) fprintf(stderr, "[jni t%d] (last line repeated %ld times so far)\n", platform_tid(), repeats);
    return;
  }
  if (repeats > 0) fprintf(stderr, "[jni t%d] (last line repeated %ld more times)\n", platform_tid(), repeats);
  repeats = 0;
  last = buf;
  fprintf(stderr, "[jni t%d] %s", platform_tid(), buf);
}
#define LOG(fmt, ...) jni_log(fmt, ##__VA_ARGS__)

struct Env;
struct Vm;

// ---------------------------------------------------------------------------
// JavaVM
// ---------------------------------------------------------------------------

struct Vm {
  const void* const* functions;
};
struct Env {
  const void* const* functions;
};

// JNIEnv is per thread, as the JNI spec requires. The library relies on this:
// it installs its own wrapper table over env->functions and remembers the
// original in a thread-local, so one env shared between threads makes the
// wrappers forward to each other forever.
std::vector<const void*> g_env_table, g_vm_table;
thread_local Env t_env{nullptr};
thread_local bool t_attached = false;
thread_local bool t_reported_wrap = false;
std::atomic<int> g_attached_threads{0};
Vm g_vm;

Env* attach_this_thread(const char* how) {
  if (!t_env.functions) t_env.functions = g_env_table.data();
  if (!t_attached) {
    t_attached = true;
    ++g_attached_threads;
    LOG("%s: thread attached, env %p\n", how, static_cast<void*>(&t_env));
  }
  return &t_env;
}

void note_wrapped() {
  if (t_env.functions && t_env.functions != g_env_table.data() && !t_reported_wrap) {
    t_reported_wrap = true;
    LOG("library has installed its own JNI function table on this thread's env (expected; it wraps ours)\n");
  }
}

jint vm_DestroyJavaVM(Vm*) { return kJniOk; }
jint vm_GetEnv(Vm*, void** env, jint version) {
  if (!t_attached) {
    LOG("GetEnv(version %#x) -> JNI_EDETACHED (thread not attached)\n", version);
    *env = nullptr;
    return -2;  // JNI_EDETACHED
  }
  note_wrapped();
  thread_local bool logged = false;  // asked for on every call; once per thread is enough
  if (!logged) LOG("GetEnv(version %#x) -> env %p (further GetEnv calls on this thread not logged)\n", version, static_cast<void*>(&t_env));
  logged = true;
  *env = &t_env;
  return kJniOk;
}
jint vm_AttachCurrentThread(Vm*, void** env, void*) {
  note_wrapped();
  *env = attach_this_thread("AttachCurrentThread");
  return kJniOk;
}
jint vm_DetachCurrentThread(Vm*) {
  if (t_attached) --g_attached_threads;
  t_attached = false;
  LOG("DetachCurrentThread\n");
  return kJniOk;
}

// ---------------------------------------------------------------------------
// JNIEnv: implemented functions
// ---------------------------------------------------------------------------

jint GetVersion(Env*) { return kJniVersion16; }

void* FindClass(Env*, const char* name) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  LOG("FindClass %s\n", name ? name : "(null)");
  return class_named(name ? name : "");
}

void* GetObjectClass(Env*, void* obj) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  if (Object* o = as_object(obj)) return o->cls;
  if (as_class(obj)) return class_named("java/lang/Class");  // a jclass is a java.lang.Class object
  if (!obj) LOG("GetObjectClass(null): Java would throw NullPointerException here; a value is missing (--values)\n");
  else LOG("GetObjectClass on an object we did not create (%p)\n", obj);
  return class_named("<unknown>");
}
void* GetSuperclass(Env*, void* cls) {
  LOG("GetSuperclass %s -> null\n", class_of(cls).c_str());
  return nullptr;
}
jboolean IsAssignableFrom(Env*, void* a, void* b) {
  LOG("IsAssignableFrom %s, %s -> false\n", class_of(a).c_str(), class_of(b).c_str());
  return 0;
}
jboolean IsInstanceOf(Env*, void* obj, void* cls) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Object* o = as_object(obj);
  const bool r = o && as_class(cls) == o->cls;
  LOG("IsInstanceOf %s, %s -> %s\n", class_of(obj).c_str(), class_of(cls).c_str(), r ? "true" : "false");
  return r;
}
jboolean IsSameObject(Env*, void* a, void* b) { return a == b; }

void* member_id(void* cls, const char* name, const char* sig, bool is_static, bool is_field) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Class* c = as_class(cls);
  if (!c) c = class_named("<unknown>");
  auto* m = new Member{c, name ? name : "", sig ? sig : "", is_static, is_field};
  g_members.insert(m);
  ++g_member_lookups[std::string(is_static ? "static " : "") + (is_field ? "field  " : "method ") + c->name + "." +
                     m->name + " " + m->sig];
  LOG("Get%s%sID %s.%s %s\n", is_static ? "Static" : "", is_field ? "Field" : "Method", c->name.c_str(),
      m->name.c_str(), m->sig.c_str());
  return m;
}
void* GetMethodID(Env*, void* c, const char* n, const char* s) { return member_id(c, n, s, false, false); }
void* GetStaticMethodID(Env*, void* c, const char* n, const char* s) { return member_id(c, n, s, true, false); }
void* GetFieldID(Env*, void* c, const char* n, const char* s) { return member_id(c, n, s, false, true); }
void* GetStaticFieldID(Env*, void* c, const char* n, const char* s) { return member_id(c, n, s, true, true); }

struct NativeMethod {
  const char* name;
  const char* signature;
  void* fn;
};
jint RegisterNatives(Env*, void* cls, const NativeMethod* methods, jint n) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  const std::string cn = class_of(cls);
  LOG("RegisterNatives %s (%d methods)\n", cn.c_str(), n);
  for (jint i = 0; i < n; ++i) {
    char where[300];
    describe_addr(reinterpret_cast<uint64_t>(methods[i].fn), where, sizeof where);
    LOG("    %s %s -> %s\n", methods[i].name, methods[i].signature, where);
    g_natives.push_back({cn, methods[i].name, methods[i].signature, reinterpret_cast<uint64_t>(methods[i].fn)});
  }
  return kJniOk;
}
jint UnregisterNatives(Env*, void* cls) {
  LOG("UnregisterNatives %s\n", class_of(cls).c_str());
  return kJniOk;
}

// References: one kind of handle for everything (the object itself), counted
// so objects the library is done with are freed. The library creates objects
// every frame (strings, arrays); kept forever they add up to millions and
// slow everything that looks objects up.
void free_if_unused(Object* o) {
  if (o->pinned || o->locals > 0 || o->globals > 0) return;
  g_objects.erase(o);
  delete o;
}
void* NewGlobalRef(Env*, void* p) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  if (Object* o = as_object(p)) ++o->globals;
  return p;
}
void* NewLocalRef(Env*, void* p) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  if (Object* o = as_object(p)) ++o->locals;
  return p;
}
void DeleteGlobalRef(Env*, void* p) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  if (Object* o = as_object(p)) {
    if (o->globals > 0) --o->globals;
    free_if_unused(o);
  }
}
void DeleteLocalRef(Env*, void* p) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  if (Object* o = as_object(p)) {
    if (o->locals > 0) --o->locals;
    free_if_unused(o);
  }
}
jint PushLocalFrame(Env*, jint) { return kJniOk; }
void* PopLocalFrame(Env*, void* result) { return result; }
jint EnsureLocalCapacity(Env*, jint) { return kJniOk; }
jint GetObjectRefType(Env*, void* o) { return o ? 1 : 0; }  // JNILocalRefType

// Exceptions: there is never a pending exception, because nothing can throw.
jboolean ExceptionCheck(Env*) { return 0; }
void* ExceptionOccurred(Env*) { return nullptr; }
void ExceptionVoid(Env*) {}
jint Throw(Env*, void* t) {
  LOG("Throw %s\n", class_of(t).c_str());
  return kJniOk;
}
jint ThrowNew(Env*, void* cls, const char* msg) {
  LOG("ThrowNew %s: %s\n", class_of(cls).c_str(), msg ? msg : "");
  return kJniOk;
}
[[noreturn]] void FatalError(Env*, const char* msg) {
  LOG("FatalError: %s\n", msg ? msg : "");
  abort();
}

jint MonitorEnterExit(Env*, void*) { return kJniOk; }
jint GetJavaVM(Env*, void** vm) {
  *vm = &g_vm;
  return kJniOk;
}

void* AllocObject(Env*, void* cls) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  LOG("AllocObject %s\n", class_of(cls).c_str());
  return new_object(as_class(cls) ? as_class(cls) : class_named("<unknown>"));
}
// NewObject, NewObjectV and NewObjectA: defined after the argument readers.
void* NewObject(Env*, void* cls, void* ctor, ...);
void* NewObjectV(Env*, void* cls, void* ctor, va_list ap);
void* NewObjectA(Env*, void* cls, void* ctor, const uint64_t* args);

// Strings (modified UTF-8 is treated as plain UTF-8 for now).
void* NewStringUTF(Env*, const char* s) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  return new_object(class_named("java/lang/String"), s ? s : "");
}
const char* GetStringUTFChars(Env*, void* str, jboolean* is_copy) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  if (is_copy) *is_copy = 0;
  if (Object* o = as_object(str)) return o->utf.c_str();
  LOG("GetStringUTFChars on a string we did not create (%p) -> \"\"\n", str);
  return "";
}
void ReleaseStringUTFChars(Env*, void*, const char*) {}
jsize GetStringUTFLength(Env*, void* str) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Object* o = as_object(str);
  return o ? static_cast<jsize>(o->utf.size()) : 0;
}
jsize GetStringLength(Env* e, void* str) { return GetStringUTFLength(e, str); }  // ASCII-only approximation
void GetStringUTFRegion(Env*, void* str, jsize start, jsize len, char* buf) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Object* o = as_object(str);
  const std::string s = o ? o->utf : "";
  const size_t st = std::min<size_t>(start, s.size());
  const size_t n = std::min<size_t>(len, s.size() - st);
  memcpy(buf, s.data() + st, n);
  buf[n] = '\0';
}

// Arrays. Element sizes by JNI type letter.
size_t elem_size(const std::string& cls) {
  switch (cls.size() == 2 ? cls[1] : 0) {
    case 'Z': case 'B': return 1;
    case 'C': case 'S': return 2;
    case 'I': case 'F': return 4;
    case 'J': case 'D': return 8;
    default: return 0;
  }
}
template <char T>
void* NewPrimArray(Env*, jsize n) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Object* o = new_object(class_named(std::string("[") + T));
  o->data.assign(static_cast<size_t>(n < 0 ? 0 : n) * elem_size(o->cls->name), 0);
  return o;
}
void* NewObjectArray(Env*, jsize n, void* cls, void* init) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Class* c = as_class(cls);
  Object* o = new_object(class_named("[L" + (c ? c->name : std::string("java/lang/Object")) + ";"));
  o->elems.assign(n < 0 ? 0 : n, init);
  return o;
}
jsize GetArrayLength(Env*, void* arr) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Object* o = as_object(arr);
  if (!o) {
    LOG("GetArrayLength on %s -> 0\n", arr ? "an array we did not create" : "null");
    return 0;
  }
  const size_t es = elem_size(o->cls->name);
  return static_cast<jsize>(es ? o->data.size() / es : o->elems.size());
}
void* GetObjectArrayElement(Env*, void* arr, jsize i) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Object* o = as_object(arr);
  return o && i >= 0 && static_cast<size_t>(i) < o->elems.size() ? o->elems[i] : nullptr;
}
void SetObjectArrayElement(Env*, void* arr, jsize i, void* v) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Object* o = as_object(arr);
  if (o && i >= 0 && static_cast<size_t>(i) < o->elems.size()) o->elems[i] = v;
}
// Get<T>ArrayElements and GetPrimitiveArrayCritical hand out the array's own
// storage (never a copy), so Release has nothing to write back.
void* GetArrayElements(Env*, void* arr, jboolean* is_copy) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  if (is_copy) *is_copy = 0;
  Object* o = as_object(arr);
  if (!o) return nullptr;
  if (o->data.empty()) o->data.reserve(1);
  return o->data.data();
}
void ReleaseArrayElements(Env*, void*, void*, jint) {}
void ArrayRegion(void* arr, jsize start, jsize len, void* buf, bool get) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Object* o = as_object(arr);
  if (!o || start < 0 || len < 0) return;
  const size_t es = elem_size(o->cls->name);
  const size_t off = static_cast<size_t>(start) * es, n = static_cast<size_t>(len) * es;
  if (off + n > o->data.size()) {
    LOG("%sArrayRegion out of bounds (%d+%d of %zu)\n", get ? "Get" : "Set", start, len, es ? o->data.size() / es : 0);
    return;
  }
  if (get) memcpy(buf, o->data.data() + off, n);
  else memcpy(o->data.data() + off, buf, n);
}
void GetArrayRegion(Env*, void* arr, jsize start, jsize len, void* buf) { ArrayRegion(arr, start, len, buf, true); }
void SetArrayRegion(Env*, void* arr, jsize start, jsize len, const void* buf) {
  ArrayRegion(arr, start, len, const_cast<void*>(buf), false);
}

// ---------------------------------------------------------------------------
// Values "Java" returns, from --values FILE (see fakejni.hpp). Keyed like
//   com/x/Y.method(I)Ljava/lang/String;     a method (static or not)
//   com/x/Y.field:F                          a field
// ---------------------------------------------------------------------------

struct Value {
  char kind;  // 's' string, 'n' new object, 'z' null, 'i' integer/boolean, 'd' floating
  std::string s;
  int64_t i = 0;
  double d = 0;
};
std::map<std::string, Value> g_values;
std::unordered_set<std::string> g_values_used;

std::string member_key(const Member* m) {
  return m->cls->name + "." + m->name + (m->is_field ? ":" : "") + m->sig;
}

// The Java type a member produces: a method's return type, a field's type.
std::string result_type(const Member* m) {
  return m->is_field ? m->sig : m->sig.substr(m->sig.find(')') + 1);
}

// Returns the configured value as R, or R{} (0/null) when there is none, and
// logs which it was.
// Every argument's raw bits of the call being handled (a float/double as a
// double's bits), filled by args_from_va / args_from_array.
thread_local std::vector<uint64_t> t_raw_args;

// Java methods implemented in C++ (register_java_method), by member key.
// Registered before the library runs, read without a lock afterwards.
struct JavaImpl {
  std::function<uint64_t(void*, const std::vector<uint64_t>&)> fn;
  std::atomic<long> calls{0};
};
std::map<std::string, JavaImpl*>& java_impls() {
  static auto* m = new std::map<std::string, JavaImpl*>;
  return *m;
}

template <typename R>
R produce(const char* what, void* mid, void* obj, const std::string& args = {}) {
  if constexpr (!std::is_floating_point_v<R>) {
    if (!java_impls().empty()) {
      std::string key;
      {
        std::lock_guard<std::recursive_mutex> l(g_mu);
        if (Member* m = as_member(mid)) key = member_key(m);
      }
      auto it = java_impls().find(key);
      if (it != java_impls().end()) {
        // Run outside the lock: an implementation may block (AudioTrack.write).
        const long n = ++it->second->calls;
        const uint64_t r = it->second->fn(obj, t_raw_args);
        if (n <= 2) LOG("%s %s%s -> %llu (implemented by tonic%s)\n", what, key.c_str(),
                        args.empty() ? "" : (" args " + args).c_str(), static_cast<unsigned long long>(r),
                        n == 2 ? "; not logging further calls" : "");
        return static_cast<R>(r);
      }
    }
  }
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Member* m = as_member(mid);
  const std::string on = (args.empty() ? "" : " args " + args) + (obj ? " on " + class_of(obj) : "");
  // A value for this labelled object (CLASS#LABEL.member) wins over one for
  // the class: that is how one handler object answers differently from another.
  auto it = g_values.end();
  if (Object* o = as_object(obj); m && o && !o->label.empty()) {
    const std::string k = member_key(m);
    it = g_values.find(m->cls->name + "#" + o->label + k.substr(m->cls->name.size()));
  }
  if (it == g_values.end() && m) it = g_values.find(member_key(m));
  // Java library methods the library relies on, answered from the object
  // itself. String.getBytes(charset) / getBytes(): how it reads a String's
  // text (TextBox input arrives this way). The text is never logged.
  if constexpr (!std::is_floating_point_v<R>) {
    Object* self = as_object(obj);
    if (it == g_values.end() && m && self && self->cls->name == "java/lang/String" && m->name == "getBytes" &&
        (m->sig == "(Ljava/lang/String;)[B" || m->sig == "()[B" || m->sig == "(Ljava/nio/charset/Charset;)[B")) {
      Object* arr = new_object(class_named("[B"));
      arr->data.assign(self->utf.begin(), self->utf.end());
      LOG("%s %s%s -> byte[%zu] (the String's UTF-8)\n", what, describe(mid).c_str(),
          args.empty() ? "" : (" args " + args).c_str(), arr->data.size());
      return static_cast<R>(reinterpret_cast<uint64_t>(arr));
    }
  }
  if (it == g_values.end()) {
    LOG("%s %s%s -> 0\n", what, describe(mid).c_str(), on.c_str());
    return R{};
  }
  g_values_used.insert(it->first);
  const Value& v = it->second;
  if constexpr (std::is_floating_point_v<R>) {
    LOG("%s %s%s -> %g (values)\n", what, describe(mid).c_str(), on.c_str(), v.kind == 'd' ? v.d : (double)v.i);
    return static_cast<R>(v.kind == 'd' ? v.d : static_cast<double>(v.i));
  } else {
    uint64_t r = 0;
    std::string shown;
    switch (v.kind) {
      case 's': r = reinterpret_cast<uint64_t>(new_object(class_named("java/lang/String"), v.s)); shown = "\"" + v.s + "\""; break;
      case 'n': {
        const std::string t = result_type(m);
        const std::string cn = t.size() > 2 && t[0] == 'L' ? t.substr(1, t.size() - 2) : t;
        r = reinterpret_cast<uint64_t>(new_object(class_named(cn)));
        shown = "new " + cn;
        break;
      }
      case 'z': shown = "null"; break;
      case 'd': r = static_cast<uint64_t>(static_cast<int64_t>(v.d)); shown = std::to_string(v.d); break;
      default: r = static_cast<uint64_t>(v.i); shown = std::to_string(v.i); break;
    }
    LOG("%s %s%s -> %s (values)\n", what, describe(mid).c_str(), on.c_str(), shown.c_str());
    return static_cast<R>(r);
  }
}

// ---------------------------------------------------------------------------
// Families: Call*Method, CallNonvirtual*Method, CallStatic*Method (and their V
// and A forms), Get/Set(Static)*Field. They log what was asked and return the
// configured value or 0. The plain, V and A forms differ only after the method
// ID, so one function serves all three. Float and double results come back in
// xmm0, so those need their own return types.
// ---------------------------------------------------------------------------

// Arguments of a Call*Method, for the log: Strings show their text, other
// objects their class, numbers their value. The plain forms are variadic, the
// V forms take a va_list and the A forms a jvalue array; all three are read by
// the method's signature. Floats arrive as doubles in the variadic forms.
std::vector<std::string> param_types(const Member* m) {
  std::vector<std::string> out;
  if (!m || m->is_field) return out;
  const std::string& d = m->sig;
  size_t i = 1;
  while (i < d.size() && d[i] != ')') {
    size_t j = i;
    while (j < d.size() && d[j] == '[') ++j;
    if (j < d.size() && d[j] == 'L') j = d.find(';', j);
    if (j == std::string::npos || j >= d.size()) break;
    out.push_back(d.substr(i, j + 1 - i));
    i = j + 1;
  }
  return out;
}

// The text of each String argument of the call being logged, for rules ($arg1..),
// and every argument's raw bits (a float/double as a double's bits), for observers.
thread_local std::vector<std::string> t_string_args;

// Calls that carry what the user typed into a TextBox: their String arguments
// are shown as "<redacted>" (rules still see them).
thread_local bool t_redact = false;
bool carries_typed_text(void* mid) {
  Member* m = as_member(mid);
  if (!m) return false;
  // onItemSet: MemStorage callbacks, which carry scripts the app runs in web
  // pages (challenge answers, tokens).
  for (const char* k : {"TextBox", "Keyboard", "Password", "password", "Credential", "onItemSet", "Cookie", "cookie"})
    if (m->name.find(k) != std::string::npos) return true;
  return false;
}
std::string show_value(const std::string& t, uint64_t bits, double fp) {
  char buf[64];
  if (t.size() > 1) {  // object or array
    std::lock_guard<std::recursive_mutex> l(g_mu);
    void* h = reinterpret_cast<void*>(bits);
    if (!h) return "null";
    if (Object* o = as_object(h)) {
      // Contents never logged: libroblox passes TextBox text (passwords
      // included) to Java as a byte[].
      if (o->cls->name == "[B") return "byte[" + std::to_string(o->data.size()) + "]";
      if (o->cls->name != "java/lang/String") return class_of(o);
      t_string_args.push_back(o->utf);
      if (t_redact) return "\"<redacted>\"";
      std::string s = o->utf.size() > 400 ? o->utf.substr(0, 400) + "...(" + std::to_string(o->utf.size()) + " bytes)" : o->utf;
      return "\"" + s + "\"";
    }
    return class_of(h);
  }
  if (t == "F" || t == "D") snprintf(buf, sizeof buf, "%g", fp);
  else if (t == "J") snprintf(buf, sizeof buf, "%lld", static_cast<long long>(bits));
  else if (t == "Z") return bits & 0xff ? "true" : "false";
  else snprintf(buf, sizeof buf, "%d", static_cast<int>(bits));
  return buf;
}

std::string args_from_va(void* mid, va_list ap) {
  t_string_args.clear();
  t_raw_args.clear();
  t_redact = carries_typed_text(mid);
  const auto types = param_types(as_member(mid));
  if (types.empty()) return {};
  std::string out = "(";
  for (size_t i = 0; i < types.size(); ++i) {
    const bool fp = types[i] == "F" || types[i] == "D";
    uint64_t bits = 0;
    double d = 0;
    if (fp) d = va_arg(ap, double), memcpy(&bits, &d, 8), t_raw_args.push_back(bits), bits = 0;
    else bits = va_arg(ap, uint64_t), t_raw_args.push_back(bits);
    out += (i ? ", " : "") + show_value(types[i], bits, d);
  }
  return out + ")";
}

std::string args_from_array(void* mid, const uint64_t* a) {
  t_string_args.clear();
  t_raw_args.clear();
  t_redact = carries_typed_text(mid);
  const auto types = param_types(as_member(mid));
  if (types.empty() || !a) return {};
  std::string out = "(";
  for (size_t i = 0; i < types.size(); ++i) {
    double d = 0;
    if (types[i] == "F") {
      float f;
      memcpy(&f, &a[i], 4);
      d = f;
    } else if (types[i] == "D") {
      memcpy(&d, &a[i], 8);
    }
    {
      uint64_t raw = a[i];
      if (types[i] == "F") memcpy(&raw, &d, 8);
      t_raw_args.push_back(raw);
    }
    out += (i ? ", " : "") + show_value(types[i], a[i], d);
  }
  return out + ")";
}

void* new_with_args(void* cls, void* ctor) {  // t_raw_args holds the constructor's arguments
  std::lock_guard<std::recursive_mutex> l(g_mu);
  t_redact = false;
  LOG("NewObject %s\n", describe(ctor).c_str());
  Object* o = new_object(as_class(cls) ? as_class(cls) : class_named("<unknown>"));
  o->ctor_args = t_raw_args;
  return o;
}
void* NewObjectV(Env*, void* cls, void* ctor, va_list in) {
  va_list ap;
  va_copy(ap, in);
  args_from_va(ctor, ap);
  va_end(ap);
  return new_with_args(cls, ctor);
}
void* NewObject(Env* e, void* cls, void* ctor, ...) {
  va_list ap;
  va_start(ap, ctor);
  void* r = NewObjectV(e, cls, ctor, ap);
  va_end(ap);
  return r;
}
void* NewObjectA(Env*, void* cls, void* ctor, const uint64_t* args) {
  args_from_array(ctor, args);
  return new_with_args(cls, ctor);
}

// Calls on an object created with new:LABEL are reported to the script runner,
// which runs the "on LABEL ..." rules for it (how the probe answers callbacks).
std::function<void(const std::string&, const std::string&, const std::string&, const std::vector<std::string>&)> g_call_hook;
// Every call into "Java" (static or not, labelled or not): method name and raw
// arguments. The input code uses it to learn which TextBox has focus.
std::vector<std::function<void(const std::string&, const std::vector<uint64_t>&)>> g_observers;
// Calls on labelled objects, for C++ code that hands the library callback
// objects of its own (webview.cpp): (label, method, raw arguments).
std::vector<std::function<void(const std::string&, const std::string&, const std::vector<uint64_t>&)>> g_listeners;

void observe(void* mid) {
  if (g_observers.empty()) return;
  std::string method;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Member* m = as_member(mid);
    if (!m) return;
    method = m->name;
  }
  for (auto& f : g_observers) f(method, t_raw_args);
}

// Static calls into "Java" (notifications such as onDataModelNotificationCallback)
// are reported too, under the label "static:<method name>".
template <typename R>
R notify_static(R r, void* mid, const std::string& args) {
  observe(mid);
  std::string method;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Member* m = as_member(mid);
    if (!m || !g_call_hook) return r;
    method = m->name;
  }
  const std::vector<std::string> strs = t_string_args;
  g_call_hook("static:" + method, method, args, strs);
  return r;
}

template <typename R>
R notify(R r, void* obj, void* mid, const std::string& args) {
  observe(mid);
  std::string label, method;
  {
    std::lock_guard<std::recursive_mutex> l(g_mu);
    Object* o = as_object(obj);
    if (!o || o->label.empty()) return r;
    label = o->label;
    Member* m = as_member(mid);
    method = m ? m->name : "?";
  }
  for (auto& f : g_listeners) f(label, method, t_raw_args);
  if (!g_call_hook) return r;
  const std::vector<std::string> strs = t_string_args;
  g_call_hook(label, method, args, strs);
  return r;
}

#define TONIC_VA_ARGS(mid, last)  \
  va_list ap;                         \
  va_start(ap, last);                 \
  const std::string a = args_from_va(mid, ap); \
  va_end(ap)

template <typename R>
R call_inst(Env*, void* obj, void* mid, ...) {
  TONIC_VA_ARGS(mid, mid);
  return notify(produce<R>("Call", mid, obj, a), obj, mid, a);
}
template <typename R>
R call_inst_v(Env*, void* obj, void* mid, va_list in) {
  va_list ap;
  va_copy(ap, in);
  const std::string a = args_from_va(mid, ap);
  va_end(ap);
  return notify(produce<R>("Call", mid, obj, a), obj, mid, a);
}
template <typename R>
R call_inst_a(Env*, void* obj, void* mid, const uint64_t* v) {
  const std::string a = args_from_array(mid, v);
  return notify(produce<R>("Call", mid, obj, a), obj, mid, a);
}

template <typename R>
R call_nonvirt(Env*, void* obj, void*, void* mid, ...) {
  TONIC_VA_ARGS(mid, mid);
  return notify(produce<R>("CallNonvirtual", mid, obj, a), obj, mid, a);
}
template <typename R>
R call_nonvirt_v(Env*, void* obj, void*, void* mid, va_list in) {
  va_list ap;
  va_copy(ap, in);
  const std::string a = args_from_va(mid, ap);
  va_end(ap);
  return notify(produce<R>("CallNonvirtual", mid, obj, a), obj, mid, a);
}
template <typename R>
R call_nonvirt_a(Env*, void* obj, void*, void* mid, const uint64_t* v) {
  const std::string a = args_from_array(mid, v);
  return notify(produce<R>("CallNonvirtual", mid, obj, a), obj, mid, a);
}

template <typename R>
R call_static(Env*, void*, void* mid, ...) {
  TONIC_VA_ARGS(mid, mid);
  return notify_static(produce<R>("CallStatic", mid, nullptr, a), mid, a);
}
template <typename R>
R call_static_v(Env*, void*, void* mid, va_list in) {
  va_list ap;
  va_copy(ap, in);
  const std::string a = args_from_va(mid, ap);
  va_end(ap);
  return notify_static(produce<R>("CallStatic", mid, nullptr, a), mid, a);
}
template <typename R>
R call_static_a(Env*, void*, void* mid, const uint64_t* v) {
  const std::string a = args_from_array(mid, v);
  return notify_static(produce<R>("CallStatic", mid, nullptr, a), mid, a);
}
#undef TONIC_VA_ARGS
template <typename R>
R get_field(Env*, void* obj, void* fid) { return produce<R>("Get", fid, obj); }
template <typename R>
R get_static(Env*, void*, void* fid) { return produce<R>("GetStatic", fid, nullptr); }
void set_field(Env*, void* obj, void* fid) { LOG("Set %s on %s (ignored)\n", describe(fid).c_str(), class_of(obj).c_str()); }
void set_static(Env*, void*, void* fid) { LOG("SetStatic %s (ignored)\n", describe(fid).c_str()); }

bool starts(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }
bool ends(const std::string& s, const char* p) {
  const size_t n = strlen(p);
  return s.size() >= n && s.compare(s.size() - n, n, p) == 0;
}

template <template <typename> class F>
void* by_type(const std::string& name) {
  if (name.find("Float") != std::string::npos) return reinterpret_cast<void*>(F<float>::fn);
  if (name.find("Double") != std::string::npos) return reinterpret_cast<void*>(F<double>::fn);
  return reinterpret_cast<void*>(F<uint64_t>::fn);
}
template <typename R> struct CallInst { static constexpr auto fn = &call_inst<R>; };
template <typename R> struct CallInstV { static constexpr auto fn = &call_inst_v<R>; };
template <typename R> struct CallInstA { static constexpr auto fn = &call_inst_a<R>; };
template <typename R> struct CallNonvirt { static constexpr auto fn = &call_nonvirt<R>; };
template <typename R> struct CallNonvirtV { static constexpr auto fn = &call_nonvirt_v<R>; };
template <typename R> struct CallNonvirtA { static constexpr auto fn = &call_nonvirt_a<R>; };
template <typename R> struct CallStatic { static constexpr auto fn = &call_static<R>; };
template <typename R> struct CallStaticV { static constexpr auto fn = &call_static_v<R>; };
template <typename R> struct CallStaticA { static constexpr auto fn = &call_static_a<R>; };
template <typename R> struct GetField { static constexpr auto fn = &get_field<R>; };
template <typename R> struct GetStatic { static constexpr auto fn = &get_static<R>; };

void* family(const std::string& n) {
  const bool v = ends(n, "MethodV"), a = ends(n, "MethodA"), method = ends(n, "Method") || v || a;
  if (method && starts(n, "CallStatic"))
    return v ? by_type<CallStaticV>(n) : a ? by_type<CallStaticA>(n) : by_type<CallStatic>(n);
  if (method && starts(n, "CallNonvirtual"))
    return v ? by_type<CallNonvirtV>(n) : a ? by_type<CallNonvirtA>(n) : by_type<CallNonvirt>(n);
  if (method && starts(n, "Call")) return v ? by_type<CallInstV>(n) : a ? by_type<CallInstA>(n) : by_type<CallInst>(n);
  if (ends(n, "Field") && starts(n, "GetStatic")) return by_type<GetStatic>(n);
  if (ends(n, "Field") && starts(n, "Get")) return by_type<GetField>(n);
  if (ends(n, "Field") && starts(n, "SetStatic")) return reinterpret_cast<void*>(&set_static);
  if (ends(n, "Field") && starts(n, "Set")) return reinterpret_cast<void*>(&set_field);
  return nullptr;
}

template <typename T>
void put(std::unordered_map<std::string, void*>& m, const char* n, T* f) {
  m[n] = reinterpret_cast<void*>(f);
}

void build() {
  std::unordered_map<std::string, void*> impl;
  put(impl, "GetVersion", &GetVersion);
  put(impl, "FindClass", &FindClass);
  put(impl, "GetObjectClass", &GetObjectClass);
  put(impl, "GetSuperclass", &GetSuperclass);
  put(impl, "IsAssignableFrom", &IsAssignableFrom);
  put(impl, "IsInstanceOf", &IsInstanceOf);
  put(impl, "IsSameObject", &IsSameObject);
  put(impl, "GetMethodID", &GetMethodID);
  put(impl, "GetStaticMethodID", &GetStaticMethodID);
  put(impl, "GetFieldID", &GetFieldID);
  put(impl, "GetStaticFieldID", &GetStaticFieldID);
  put(impl, "RegisterNatives", &RegisterNatives);
  put(impl, "UnregisterNatives", &UnregisterNatives);
  put(impl, "NewGlobalRef", &NewGlobalRef);
  put(impl, "NewWeakGlobalRef", &NewGlobalRef);
  put(impl, "NewLocalRef", &NewLocalRef);
  put(impl, "DeleteGlobalRef", &DeleteGlobalRef);
  put(impl, "DeleteWeakGlobalRef", &DeleteGlobalRef);
  put(impl, "DeleteLocalRef", &DeleteLocalRef);
  put(impl, "PushLocalFrame", &PushLocalFrame);
  put(impl, "PopLocalFrame", &PopLocalFrame);
  put(impl, "EnsureLocalCapacity", &EnsureLocalCapacity);
  put(impl, "GetObjectRefType", &GetObjectRefType);
  put(impl, "ExceptionCheck", &ExceptionCheck);
  put(impl, "ExceptionOccurred", &ExceptionOccurred);
  put(impl, "ExceptionClear", &ExceptionVoid);
  put(impl, "ExceptionDescribe", &ExceptionVoid);
  put(impl, "Throw", &Throw);
  put(impl, "ThrowNew", &ThrowNew);
  put(impl, "FatalError", &FatalError);
  put(impl, "MonitorEnter", &MonitorEnterExit);
  put(impl, "MonitorExit", &MonitorEnterExit);
  put(impl, "GetJavaVM", &GetJavaVM);
  put(impl, "AllocObject", &AllocObject);
  put(impl, "NewObject", &NewObject);
  put(impl, "NewObjectV", &NewObjectV);
  put(impl, "NewObjectA", &NewObjectA);
  put(impl, "NewStringUTF", &NewStringUTF);
  put(impl, "GetStringUTFChars", &GetStringUTFChars);
  put(impl, "ReleaseStringUTFChars", &ReleaseStringUTFChars);
  put(impl, "GetStringUTFLength", &GetStringUTFLength);
  put(impl, "GetStringLength", &GetStringLength);
  put(impl, "GetStringUTFRegion", &GetStringUTFRegion);
  put(impl, "NewBooleanArray", &NewPrimArray<'Z'>);
  put(impl, "NewByteArray", &NewPrimArray<'B'>);
  put(impl, "NewCharArray", &NewPrimArray<'C'>);
  put(impl, "NewShortArray", &NewPrimArray<'S'>);
  put(impl, "NewIntArray", &NewPrimArray<'I'>);
  put(impl, "NewLongArray", &NewPrimArray<'J'>);
  put(impl, "NewFloatArray", &NewPrimArray<'F'>);
  put(impl, "NewDoubleArray", &NewPrimArray<'D'>);
  put(impl, "NewObjectArray", &NewObjectArray);
  put(impl, "GetArrayLength", &GetArrayLength);
  put(impl, "GetObjectArrayElement", &GetObjectArrayElement);
  put(impl, "SetObjectArrayElement", &SetObjectArrayElement);
  put(impl, "GetPrimitiveArrayCritical", &GetArrayElements);
  put(impl, "ReleasePrimitiveArrayCritical", &ReleaseArrayElements);
  for (const char* t : {"Boolean", "Byte", "Char", "Short", "Int", "Long", "Float", "Double"}) {
    const std::string n = t;
    impl["Get" + n + "ArrayElements"] = reinterpret_cast<void*>(&GetArrayElements);
    impl["Release" + n + "ArrayElements"] = reinterpret_cast<void*>(&ReleaseArrayElements);
    impl["Get" + n + "ArrayRegion"] = reinterpret_cast<void*>(&GetArrayRegion);
    impl["Set" + n + "ArrayRegion"] = reinterpret_cast<void*>(&SetArrayRegion);
  }

  for (const char* n : jni_names::kEnv) {
    const std::string name = n;
    if (starts(name, "reserved")) {
      g_env_table.push_back(nullptr);
    } else if (auto it = impl.find(name); it != impl.end()) {
      g_env_table.push_back(it->second);
    } else if (void* f = family(name)) {
      g_env_table.push_back(f);
    } else {
      g_env_table.push_back(reinterpret_cast<void*>(make_logging_stub("JNI " + name)));
    }
  }
  for (const char* n : jni_names::kVm) {
    const std::string name = n;
    if (starts(name, "reserved")) g_vm_table.push_back(nullptr);
    else if (name == "DestroyJavaVM") g_vm_table.push_back(reinterpret_cast<void*>(&vm_DestroyJavaVM));
    else if (name == "GetEnv") g_vm_table.push_back(reinterpret_cast<void*>(&vm_GetEnv));
    else if (name == "DetachCurrentThread") g_vm_table.push_back(reinterpret_cast<void*>(&vm_DetachCurrentThread));
    else g_vm_table.push_back(reinterpret_cast<void*>(&vm_AttachCurrentThread));  // both Attach forms
  }
  g_vm.functions = g_vm_table.data();
}

}  // namespace

void* fake_java_vm() {
  static std::once_flag once;
  std::call_once(once, build);
  return &g_vm;
}

void jni_attach_current_thread() {
  fake_java_vm();
  attach_this_thread("loader");
}

void* jni_env() { return attach_this_thread("jni_env"); }

bool load_java_values(const std::string& path, std::string& err) {
  std::ifstream f(path);
  if (!f) {
    err = "cannot open " + path;
    return false;
  }
  std::string line;
  for (int n = 1; std::getline(f, line); ++n) {
    const auto where = path + ":" + std::to_string(n) + ": ";
    size_t a = line.find_first_not_of(" \t");
    if (a == std::string::npos || line[a] == '#') continue;
    const size_t eq = line.find(" = ");
    if (eq == std::string::npos) {
      err = where + "expected KEY = VALUE";
      return false;
    }
    std::string key = line.substr(a, eq - a);
    while (!key.empty() && isspace(static_cast<unsigned char>(key.back()))) key.pop_back();
    std::string val = line.substr(eq + 3);
    while (!val.empty() && isspace(static_cast<unsigned char>(val.back()))) val.pop_back();
    Value v{};
    if (val.size() >= 2 && val.front() == '"' && val.back() == '"') {
      v.kind = 's';
      for (size_t i = 1; i + 1 < val.size(); ++i) {
        if (val[i] == '\\' && i + 2 < val.size()) {
          const char c = val[++i];
          v.s += c == 'n' ? '\n' : c == 't' ? '\t' : c;
        } else {
          v.s += val[i];
        }
      }
    } else if (val == "new") {
      v.kind = 'n';
    } else if (val == "null") {
      v.kind = 'z';
    } else if (val == "true" || val == "false") {
      v.kind = 'i';
      v.i = val == "true";
    } else {
      char* end = nullptr;
      if (val.find_first_of(".eE") != std::string::npos && val.rfind("0x", 0) != 0) {
        v.kind = 'd';
        v.d = strtod(val.c_str(), &end);
      } else {
        v.kind = 'i';
        v.i = strtoll(val.c_str(), &end, 0);
      }
      if (val.empty() || *end) {
        err = where + "cannot read value '" + val + "' (use \"text\", a number, true/false, null or new)";
        return false;
      }
    }
    std::lock_guard<std::recursive_mutex> l(g_mu);
    g_values[key] = v;
  }
  return true;
}

void set_java_value_string(const std::string& key, const std::string& text) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Value val{};
  val.kind = 's';
  val.s = text;
  g_values[key] = val;
  g_values_used.insert(key);
}

void register_java_method(const std::string& key,
                          std::function<uint64_t(void*, const std::vector<uint64_t>&)> fn) {
  auto* impl = new JavaImpl;
  impl->fn = std::move(fn);
  java_impls()[key] = impl;
}

size_t jni_object_count() {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  return g_objects.size();
}

std::vector<uint64_t> jni_ctor_args(void* h) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Object* o = as_object(h);
  return o ? o->ctor_args : std::vector<uint64_t>{};
}

std::vector<std::string> jni_string_array(void* h) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  std::vector<std::string> out;
  Object* o = as_object(h);
  if (!o) return out;
  for (void* e : o->elems)
    if (Object* s = as_object(e)) out.push_back(s->utf);
  return out;
}

void* jni_new_float_array(const std::vector<float>& values) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Object* o = new_object(class_named("[F"));
  o->data.resize(values.size() * sizeof(float));
  if (!values.empty()) memcpy(o->data.data(), values.data(), o->data.size());
  return o;
}

void set_java_value_int(const std::string& key, int64_t v) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Value val{};
  val.kind = 'i';
  val.i = v;
  g_values[key] = val;
  g_values_used.insert(key);
}

void set_java_value_new(const std::string& key) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Value val{};
  val.kind = 'n';
  g_values[key] = val;
  g_values_used.insert(key);
}

std::string java_value_string(const std::string& key) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  auto it = g_values.find(key);
  return it != g_values.end() && it->second.kind == 's' ? it->second.s : std::string();
}

void add_jni_call_listener(
    std::function<void(const std::string&, const std::string&, const std::vector<uint64_t>&)> fn) {
  g_listeners.push_back(std::move(fn));
}

void set_java_value_default(const std::string& key, double v) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  if (g_values.count(key)) return;  // --values wins
  Value val{};
  const bool integral = key.size() > 2 && (key.compare(key.size() - 2, 2, ":I") == 0 || key.back() == 'I' || key.back() == 'J');
  if (integral) val.kind = 'i', val.i = static_cast<int64_t>(v);
  else val.kind = 'd', val.d = v;
  g_values[key] = val;
  g_values_used.insert(key);  // not the user's, so not reported as unused
}

void set_java_value_double(const std::string& key, double v) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Value val{};
  val.kind = 'd', val.d = v;
  g_values[key] = val;
  g_values_used.insert(key);
}

void print_unused_java_values() {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  bool header = false;
  for (const auto& [k, v] : g_values) {
    if (g_values_used.count(k)) continue;
    if (!header) printf("  values never asked for (check the spelling/signature, or not reached yet):\n");
    header = true;
    printf("    %s\n", k.c_str());
  }
}

void* jni_class(const std::string& name) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  return class_named(name);
}

void* jni_new_object(const std::string& class_name, const std::string& label) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Object* o = new_object(class_named(class_name));
  o->label = label;
  o->pinned = true;  // tonic keeps these (callbacks, handlers)
  return o;
}

void set_jni_call_hook(std::function<void(const std::string&, const std::string&, const std::string&, const std::vector<std::string>&)> hook) {
  g_call_hook = std::move(hook);
}

void set_jni_observer(std::function<void(const std::string&, const std::vector<uint64_t>&)> fn) {
  g_observers.push_back(std::move(fn));
}

std::string jni_text(void* h) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  Object* o = as_object(h);
  if (!o) return {};
  if (o->cls->name == "[B") return std::string(o->data.begin(), o->data.end());
  return o->utf;
}

void* jni_new_string(const std::string& utf) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  return new_object(class_named("java/lang/String"), utf);
}

std::string jni_describe(void* h) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  if (!h) return "null";
  if (Object* o = as_object(h))
    return o->cls->name == "java/lang/String" ? "\"" + o->utf + "\"" : "object " + class_of(o);
  if (Class* c = as_class(h)) return "class " + c->name;
  char buf[48];
  snprintf(buf, sizeof buf, "%p (not one of our handles)", h);
  return buf;
}

uint64_t jni_registered_native(const std::string& cls, const std::string& name, const std::string& desc) {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  for (auto it = g_natives.rbegin(); it != g_natives.rend(); ++it)  // the latest registration wins
    if (it->name == name && it->sig == desc && it->cls == "class " + cls) return it->fn;
  return 0;
}

void print_jni_summary() {
  std::lock_guard<std::recursive_mutex> l(g_mu);
  printf("\nJNI summary\n  threads attached now: %d\n  classes looked up (%zu):\n", g_attached_threads.load(),
         g_classes.size());
  for (auto& [n, c] : g_classes) printf("    %s\n", n.c_str());
  printf("  members looked up (%zu):\n", g_member_lookups.size());
  for (auto& [k, cnt] : g_member_lookups) printf("    %s\n", k.c_str());
  print_unused_java_values();
  printf("  natives registered (%zu):\n", g_natives.size());
  for (const auto& n : g_natives) {
    char where[300];
    describe_addr(n.fn, where, sizeof where);
    printf("    %s.%s %s -> %s\n", n.cls.c_str(), n.name.c_str(), n.sig.c_str(), where);
  }
}

}  // namespace tonic
