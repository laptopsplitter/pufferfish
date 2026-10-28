#include "platform.hpp"
#include "webview.hpp"

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <chrono>
#include <unistd.h>

#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "fakejni.hpp"
#include "mainloop.hpp"
#include "script.hpp"

extern char** environ;

namespace tonic {

namespace {

// Timestamped in UTC like the library's own log lines, so delays show.
std::string now_utc() {
  timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  tm t;
  gmtime_r(&ts.tv_sec, &t);
  char buf[32];
  snprintf(buf, sizeof buf, "%02d:%02d:%02d.%03ld", t.tm_hour, t.tm_min, t.tm_sec, ts.tv_nsec / 1000000);
  return buf;
}
#define LOG(fmt, ...) fprintf(stderr, "[webview] %s " fmt, now_utc().c_str(), ##__VA_ARGS__)

constexpr const char* kProto = "com/roblox/protocols/webview/WebViewProtocol";
constexpr const char* kBus = "com/roblox/universalapp/messagebus/MessageBus";
constexpr const char* kMem = "com/roblox/engine/jni/memstorage/MemStorage";
constexpr const char* kRawCallback = "com/roblox/universalapp/messagebus/RawCallback";
constexpr const char* kHandler = "com/roblox/universalapp/messagebus/RequestHandlerRaw";
constexpr const char* kMemCallback = "com/roblox/engine/jni/memstorage/Callback";

const ElfImage* g_img = nullptr;

// --- calling the library's natives ------------------------------------------

void* lookup(const char* cls, const char* name, const char* desc) {
  if (uint64_t fn = jni_registered_native(cls, name, desc)) return reinterpret_cast<void*>(fn);
  if (void* p = g_img->find_symbol(jni_short_name(cls, name).c_str())) return p;
  void* p = g_img->find_symbol(jni_long_name(cls, name, desc).c_str());
  if (!p) LOG("%s.%s%s not found\n", cls, name, desc);
  return p;
}

void* env() { return jni_env(); }
void* str(const std::string& s) { return jni_new_string(s); }

std::string static_string(const char* name) {  // WebViewProtocol.getXxx()Ljava/lang/String;
  auto fn = reinterpret_cast<void* (*)(void*, void*)>(lookup(kProto, name, "()Ljava/lang/String;"));
  return fn ? jni_text(fn(env(), jni_class(kProto))) : std::string();
}

struct Names {
  std::string proto, open, mutate, close, available, closed_msg;
  std::string url_key, title_key, visible_key, available_key;
} g_names;

void* g_bus = nullptr;  // the MessageBus object instance natives are called on

std::string message_id(const std::string& proto, const std::string& id) {
  auto fn = reinterpret_cast<void* (*)(void*, void*, void*, void*)>(
      lookup(kBus, "getMessageId", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;"));
  return fn ? jni_text(fn(env(), jni_class(kBus), str(proto), str(id))) : proto + "." + id;
}

void bus_subscribe(const std::string& msg, const char* label) {
  auto fn = reinterpret_cast<void* (*)(void*, void*, void*, void*, uint8_t)>(
      lookup(kBus, "doSubscribeRaw",
             "(Ljava/lang/String;Lcom/roblox/universalapp/messagebus/RawCallback;Z)"
             "Lcom/roblox/universalapp/messagebus/Connection;"));
  if (fn) fn(env(), g_bus, str(msg), jni_new_object(kRawCallback, label), 0);
}

void bus_publish(const std::string& msg, const std::string& json) {
  auto fn = reinterpret_cast<void (*)(void*, void*, void*, void*)>(
      lookup(kBus, "publishRaw", "(Ljava/lang/String;Ljava/lang/String;)V"));
  if (fn) fn(env(), g_bus, str(msg), str(json));
}

void mem_bind(const char* key, const char* label) {
  auto fn = reinterpret_cast<void* (*)(void*, void*, void*, void*)>(
      lookup(kMem, "bind",
             "(Ljava/lang/String;Lcom/roblox/engine/jni/memstorage/Callback;)"
             "Lcom/roblox/engine/jni/memstorage/Connection;"));
  if (fn) fn(env(), jni_class(kMem), str(key), jni_new_object(kMemCallback, label));
}

void mem_fire(const char* key, const std::string& value) {
  auto fn = reinterpret_cast<void (*)(void*, void*, void*, void*)>(
      lookup(kMem, "fire", "(Ljava/lang/String;Ljava/lang/String;)V"));
  if (fn) fn(env(), jni_class(kMem), str(key), str(value));
}

// --- a little JSON reading (flat objects with string/bool values) -----------

bool json_find(const std::string& j, const std::string& key, size_t& at) {
  const std::string k = "\"" + key + "\"";
  size_t p = j.find(k);
  if (key.empty() || p == std::string::npos) return false;
  p = j.find(':', p + k.size());
  if (p == std::string::npos) return false;
  at = j.find_first_not_of(" \t\r\n", p + 1);
  return at != std::string::npos;
}
bool json_string(const std::string& j, const std::string& key, std::string& out) {
  size_t p;
  if (!json_find(j, key, p) || j[p] != '"') return false;
  out.clear();
  for (++p; p < j.size() && j[p] != '"'; ++p) {
    if (j[p] == '\\' && p + 1 < j.size()) {
      const char c = j[++p];
      if (c == 'n') out += '\n';
      else if (c == 't') out += '\t';
      else if (c == 'u' && p + 4 < j.size()) {  // \uXXXX, ASCII only (URLs and titles)
        const int cp = static_cast<int>(strtol(j.substr(p + 1, 4).c_str(), nullptr, 16));
        out += cp < 128 ? static_cast<char>(cp) : '?';
        p += 4;
      } else out += c;
    } else {
      out += j[p];
    }
  }
  return true;
}
int json_bool(const std::string& j, const std::string& key) {  // -1 absent
  size_t p;
  if (!json_find(j, key, p)) return -1;
  return j.compare(p, 4, "true") == 0 ? 1 : j.compare(p, 5, "false") == 0 ? 0 : -1;
}

// URL without its query and fragment, for logs.
std::string short_url(const std::string& u) { return u.substr(0, u.find_first_of("?#")); }

// --- the helper process -----------------------------------------------------

std::string helper_path() {
  if (const char* e = getenv("TONIC_WEBVIEW")) return e;
  const std::string d = self_exe();
  if (d.empty()) return "pufferfish-webview";
  return d.substr(0, d.rfind('/') + 1) + "pufferfish-webview";
}

std::string escape(const std::string& s) {
  std::string o;
  for (char c : s) {
    if (c == '\\') o += "\\\\";
    else if (c == '\n') o += "\\n";
    else if (c == '\r') o += "\\r";
    else o += c;
  }
  return o;
}
std::string unescape(const std::string& s) {
  std::string o;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\\' && i + 1 < s.size()) {
      const char c = s[++i];
      o += c == 'n' ? '\n' : c == 'r' ? '\r' : c;
    } else {
      o += s[i];
    }
  }
  return o;
}

// Everything that calls into the library runs on the app's main thread
// (mainloop.hpp), in order: as Java delivers WebView callbacks on its UI
// thread, and never inside the library's own callback (that can deadlock).
void post(std::function<void()> f) { main_post(std::move(f)); }

// The page's calls into the app go in on a thread of their own, in order, as
// Android's WebView delivers @JavascriptInterface calls on its "JavaBridge"
// thread, not the main thread. Made on the main thread, the call waited on
// the main thread itself (about a minute, until the app gave up on the page).
void bridge_post(std::function<void()> f) {
  static std::mutex mu;
  static std::condition_variable cv;
  static std::deque<std::function<void()>> q;
  static bool started = false;
  std::lock_guard<std::mutex> l(mu);
  q.push_back(std::move(f));
  cv.notify_one();
  if (started) return;
  started = true;
  std::thread([] {
    jni_attach_current_thread();
    for (;;) {
      std::function<void()> job;
      {
        std::unique_lock<std::mutex> l2(mu);
        cv.wait(l2, [] { return !q.empty(); });
        job = std::move(q.front());
        q.pop_front();
      }
      job();
    }
  }).detach();
}

struct Window {
  bool shown = false;
  pid_t pid = 0;
  int to = -1;  // helper's stdin
  bool unified = false;
  int generation = 0;
};
Window g_win;  // worker thread only
int g_generation = 0;

void send(const std::string& line) {
  if (g_win.to < 0) return;
  const std::string l = line + "\n";
  if (write(g_win.to, l.data(), l.size()) < 0) LOG("helper is gone\n");
}

void close_window(bool tell_helper) {
  if (!g_win.pid) return;
  if (tell_helper) send("close");
  close(g_win.to);
  g_win = Window{};
}

void on_helper_line(int generation, const std::string& line);

void open_window(const std::string& url, const std::string& title, bool visible, bool unified) {
  close_window(true);
  const std::string helper = helper_path();
  if (access(helper.c_str(), X_OK) != 0) {
    LOG("cannot open %s: %s not found (make webview; or set TONIC_WEBVIEW)\n", short_url(url).c_str(),
        helper.c_str());
    return;
  }
  int in[2], out[2];
  if (pipe2(in, O_CLOEXEC) || pipe2(out, O_CLOEXEC)) return;
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_adddup2(&fa, in[0], 0);
  posix_spawn_file_actions_adddup2(&fa, out[1], 1);
  const std::string ua = java_value_string("com/roblox/engine/jni/autovalue/InitParams.userAgent()Ljava/lang/String;");
  std::vector<std::string> args = {helper, "--url", url, "--title", title.empty() ? "Roblox" : title};
  if (!ua.empty()) args.insert(args.end(), {"--ua", ua});
  // TONIC_WEBVIEW_SHOW=1: show windows the app opens hidden (a hidden
  // WebKitGTK view may not run animation frames and timers like a visible one).
  const char* force = getenv("TONIC_WEBVIEW_SHOW");
  if (!visible && !(force && *force == '1')) args.push_back("--hidden");
  std::vector<char*> argv;
  for (auto& a : args) argv.push_back(a.data());
  argv.push_back(nullptr);
  pid_t pid = 0;
  const int e = posix_spawn(&pid, helper.c_str(), &fa, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&fa);
  close(in[0]);
  close(out[1]);
  if (e != 0) {
    LOG("starting %s failed: %s\n", helper.c_str(), strerror(e));
    close(in[1]);
    close(out[0]);
    return;
  }
  g_win.pid = pid;
  g_win.to = in[1];
  g_win.unified = unified;
  g_win.generation = ++g_generation;
  g_win.shown = visible;
  LOG("opened %s (%s, %s path)\n", short_url(url).c_str(), visible ? "visible" : "hidden", unified ? "WebViewProtocol" : "BrowserService");
  const int gen = g_win.generation;
  const int from = out[0];
  std::thread([gen, from, pid] {
    std::string buf;
    char tmp[4096];
    for (;;) {
      const ssize_t n = read(from, tmp, sizeof tmp);
      if (n <= 0) break;
      buf.append(tmp, n);
      size_t nl;
      while ((nl = buf.find('\n')) != std::string::npos) {
        const std::string line = buf.substr(0, nl);
        buf.erase(0, nl + 1);
        post([gen, line] { on_helper_line(gen, line); });
      }
    }
    close(from);
    int status = 0;
    waitpid(pid, &status, 0);
    // How the window process ended: a crash shows here (the window just vanishes).
    if (WIFSIGNALED(status)) LOG("window process crashed (signal %d)\n", WTERMSIG(status));
    else if (WIFEXITED(status) && WEXITSTATUS(status) != 0) LOG("window process exited with status %d\n", WEXITSTATUS(status));
    else LOG("window process ended\n");
  }).detach();
}

void on_helper_line(int generation, const std::string& line) {
  if (generation != g_win.generation || !g_win.pid) return;  // an old window
  const size_t sp = line.find(' ');
  const std::string kind = line.substr(0, sp);
  const std::string text = sp == std::string::npos ? "" : unescape(line.substr(sp + 1));
  if (kind == "js") {
    // Which module/function/feature only: the rest can carry tokens.
    std::string module, function, feature;
    json_string(text, "moduleID", module);
    json_string(text, "functionName", function);
    json_string(text, "feature", feature);
    LOG("page -> app: %s.%s%s%s (%zu bytes)\n", module.empty() ? "?" : module.c_str(),
        function.empty() ? "?" : function.c_str(), feature.empty() ? "" : " feature ", feature.c_str(), text.size());
    // The page says it is showing something to the user (a challenge): the
    // window must be on screen for that, whether or not the app asks for it
    // (on Android the WebView sits over the game; here it is its own window).
    if (!g_win.shown && feature.size() > 9 && feature.compare(feature.size() - 9, 9, "Displayed") == 0) {
      LOG("the page shows something (%s): showing the window\n", feature.c_str());
      send("show");
      g_win.shown = true;
    }
    if (g_win.unified) {
      auto fn = reinterpret_cast<void (*)(void*, void*, void*)>(
          lookup(kProto, "signalJavascriptCallback", "(Ljava/lang/String;)V"));
      if (fn)
        bridge_post([fn, text] {
          const auto t0 = std::chrono::steady_clock::now();
          fn(env(), jni_class(kProto), str(text));
          const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
          if (ms > 500) LOG("page -> app call took %.0f ms\n", ms);
        });
    } else {
      bridge_post([text] { mem_fire("BrowserService.JavaScriptCallback", text); });
    }
  } else if (kind == "log") {
    LOG("helper: %s\n", text.c_str());
  } else if (kind == "console") {
    LOG("page console: %s\n", text.c_str());
  } else if (kind == "loaded") {
    LOG("loaded %s\n", short_url(text).c_str());
  } else if (kind == "closed") {
    LOG("closed by the user\n");
    const bool unified = g_win.unified;
    close_window(false);
    if (unified) bus_publish(message_id(g_names.proto, g_names.closed_msg), "{}");
    else mem_fire("BrowserService.BrowserWindowClosed", "");
  }
}

// --- requests from the library ----------------------------------------------

void handle(const std::string& label, const std::string& arg) {
  if (label == "wvOpen" || label == "wvMutate") {  // WebViewProtocol (unified)
    std::string url, title;
    json_string(arg, g_names.url_key, url);
    json_string(arg, g_names.title_key, title);
    const int vis = json_bool(arg, g_names.visible_key);
    if (label == "wvOpen") {
      if (url.empty()) LOG("open without a URL\n");
      else open_window(url, title, vis != 0, true);
    } else {
      LOG("app: window %s%s\n", vis > 0 ? "shown" : vis == 0 ? "hidden" : "changed", title.empty() ? "" : ", new title");
      if (vis >= 0) send(vis ? "show" : "hide"), g_win.shown = vis != 0;
      if (!title.empty()) send("title " + escape(title));
    }
  } else if (label == "wvClose" || label == "bsClose") {
    LOG("closed by the app\n");
    close_window(true);
  } else if (label == "bsCmd") {  // BrowserService.SendCommand {"command": "open" | "config", ...}
    std::string cmd, url, title;
    json_string(arg, "command", cmd);
    json_string(arg, "url", url);
    json_string(arg, "title", title);
    const int vis = json_bool(arg, "visible");
    if (cmd == "open") open_window(url, title, vis != 0, false);
    else if (cmd == "config") {
      if (vis >= 0) send(vis ? "show" : "hide");
      if (!title.empty()) send("title " + escape(title));
    } else LOG("SendCommand \"%s\" not supported\n", cmd.c_str());
  } else if (label == "bsOpen") {  // BrowserService.OpenBrowserWindow: a URL
    std::string url = arg;
    if (!arg.empty() && arg[0] == '{') json_string(arg, "url", url);
    open_window(url, "", true, false);
  } else if (label == "bsExec") {  // BrowserService.ExecuteJavaScript
    LOG("app -> page: script (%zu bytes)%s\n", arg.size(), g_win.pid ? "" : ", but no window is open");
    send("eval " + escape(arg));
  }
}

}  // namespace

void webview_init() {
  add_jni_call_listener([](const std::string& label, const std::string& method, const std::vector<uint64_t>& raw) {
    const bool ours = label == "wvOpen" || label == "wvMutate" || label == "wvClose" || label == "bsOpen" ||
                      label == "bsClose" || label == "bsCmd" || label == "bsExec";
    if (!ours || (method != "run" && method != "onItemSet")) return;
    const std::string arg = raw.empty() ? "" : jni_text(reinterpret_cast<void*>(raw[0]));
    post([label, arg] { handle(label, arg); });
  });
}

void webview_enable(const ElfImage& img) {
  static bool done = false;  // from "tonic webview" in the script, or after the calls
  if (done) return;
  done = true;
  g_img = &img;
  g_bus = jni_new_object(kBus, "webviewBus");
  g_names.proto = static_string("getProtocolName");
  g_names.open = static_string("getOpenWindowId");
  g_names.mutate = static_string("getMutateWindowId");
  g_names.close = static_string("getCloseWindowId");
  g_names.available = static_string("getIsAvailableId");
  g_names.closed_msg = static_string("getHandleWindowCloseId");
  g_names.url_key = static_string("getUrlKey");
  g_names.title_key = static_string("getTitleKey");
  g_names.visible_key = static_string("getIsVisibleKey");
  g_names.available_key = static_string("getAvailableKey");
  if (g_names.proto.empty()) {
    LOG("WebViewProtocol natives not found; only the BrowserService path is wired\n");
  } else {
    LOG("protocol %s: open %s, mutate %s, close %s, closed %s\n", g_names.proto.c_str(), g_names.open.c_str(),
        g_names.mutate.c_str(), g_names.close.c_str(), g_names.closed_msg.c_str());
    // isAvailable: a synchronous request handler; its answer comes from the
    // values table (RequestHandlerRaw#LABEL.run), as for scripts/roblox.values.
    set_java_value_string(std::string(kHandler) + "#wvAvail.run(Ljava/lang/String;)Ljava/lang/String;",
                          "{\"" + g_names.available_key + "\":true}");
    auto set_handler = reinterpret_cast<void (*)(void*, void*, void*, void*, void*)>(
        lookup(kBus, "setRequestHandlerRaw",
               "(Ljava/lang/String;Ljava/lang/String;Lcom/roblox/universalapp/messagebus/RequestHandlerRaw;)V"));
    if (set_handler)
      set_handler(env(), g_bus, str(g_names.proto), str(g_names.available), jni_new_object(kHandler, "wvAvail"));
    bus_subscribe(message_id(g_names.proto, g_names.open), "wvOpen");
    bus_subscribe(message_id(g_names.proto, g_names.mutate), "wvMutate");
    bus_subscribe(message_id(g_names.proto, g_names.close), "wvClose");
    if (auto init = reinterpret_cast<void (*)(void*, void*)>(lookup(kProto, "initializeAndroidWebViewProtocol", "()V")))
      init(env(), jni_class(kProto));
  }
  mem_bind("BrowserService.OpenBrowserWindow", "bsOpen");
  mem_bind("BrowserService.CloseBrowserWindow", "bsClose");
  mem_bind("BrowserService.SendCommand", "bsCmd");
  mem_bind("BrowserService.ExecuteJavaScript", "bsExec");
  const std::string helper = helper_path();
  LOG("ready (helper %s%s)\n", helper.c_str(), access(helper.c_str(), X_OK) == 0 ? "" : ": NOT FOUND, run make webview");
}

}  // namespace tonic
