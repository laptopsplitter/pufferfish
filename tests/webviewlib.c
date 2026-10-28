// Stands in for libroblox's side of the WebView (make test-webview): the
// natives the Android Java side calls for WebViewProtocol, the message bus and
// MemStorage. When tonic calls initializeAndroidWebViewProtocol(), a thread
// plays the engine: it opens tests/webview/bridge.html through the subscribed
// openWindow callback, waits for the page's bridge message, runs a script in
// the page through BrowserService.ExecuteJavaScript, waits for its answer, and
// exits 0 (all seen) or 1.
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef void* (*FindClassFn)(void**, const char*);
typedef void* (*GetObjectClassFn)(void**, void*);
typedef void* (*GetIdFn)(void**, void*, const char*, const char*);
typedef void (*CallVoidFn)(void**, void*, void*, ...);
typedef void* (*NewRefFn)(void**, void*);
typedef void* (*NewStringUTFFn)(void**, const char*);
typedef const char* (*GetStringUTFCharsFn)(void**, void*, unsigned char*);
typedef int (*GetEnvFn)(void**, void***, int);
typedef int (*AttachFn)(void**, void***, void*);

static void** g_vm;
static void* g_open_cb;    // RawCallback for the openWindow message
static void* g_exec_cb;    // MemStorage callback for BrowserService.ExecuteJavaScript
static void* g_launch_cb;  // RawCallback for Game.launch (Play)
static void* g_haptic_cb[4];       // HapticProtocol requests tonic answers (rumble)
static const char* g_haptic_names[4] = {"SupportsHaptics", "UpdateSingletonVibration", "GetMotors", "SetMotorState"};
static volatile int g_haptics_state = -1;
static char g_haptic_reply[256];
static volatile long long g_started_place = -1;
static volatile int g_started_type = -1;
static char g_signals[2][256];
static int g_nsignals;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

int JNI_OnLoad(void** vm, void* reserved) {
  (void)reserved;
  g_vm = vm;
  return 0x10006;
}

static void** env(void) {
  void** e = NULL;
  ((AttachFn)((void**)*g_vm)[4])(g_vm, &e, NULL);  // AttachCurrentThread
  return e;
}
static void* jstr(void** e, const char* s) { return ((NewStringUTFFn)((void**)*e)[167])(e, s); }
static const char* cstr(void** e, void* s) { return ((GetStringUTFCharsFn)((void**)*e)[169])(e, s, NULL); }

static void call_string_method(void** e, void* obj, const char* name, const char* arg) {
  void** t = (void**)*e;
  void* cls = ((GetObjectClassFn)t[31])(e, obj);
  void* m = ((GetIdFn)t[33])(e, cls, name, "(Ljava/lang/String;)V");
  ((CallVoidFn)t[61])(e, obj, m, jstr(e, arg));
}

// WebViewProtocol: names and keys, as libroblox defines them.
#define NAME(fn, text) \
  void* Java_com_roblox_protocols_webview_WebViewProtocol_##fn(void** e, void* c) { (void)c; return jstr(e, text); }
NAME(getProtocolName, "WebView")
NAME(getOpenWindowId, "openWindow")
NAME(getMutateWindowId, "mutateWindow")
NAME(getCloseWindowId, "closeWindow")
NAME(getIsAvailableId, "isAvailable")
NAME(getHandleWindowCloseId, "handleWindowClose")
NAME(getUrlKey, "url")
NAME(getTitleKey, "title")
NAME(getIsVisibleKey, "isVisible")
NAME(getAvailableKey, "isAvailable")

void Java_com_roblox_protocols_webview_WebViewProtocol_signalJavascriptCallback(void** e, void* c, void* s) {
  (void)c;
  pthread_mutex_lock(&g_mu);
  if (g_nsignals < 2) snprintf(g_signals[g_nsignals++], sizeof g_signals[0], "%s", cstr(e, s));
  pthread_mutex_unlock(&g_mu);
}

void* Java_com_roblox_universalapp_messagebus_MessageBus_getMessageId(void** e, void* c, void* a, void* b) {
  (void)c;
  char buf[256];
  snprintf(buf, sizeof buf, "%s.%s", cstr(e, a), cstr(e, b));
  return jstr(e, buf);
}
void* Java_com_roblox_universalapp_messagebus_MessageBus_doSubscribeRaw(void** e, void* self, void* name, void* cb,
                                                                         unsigned char flag) {
  (void)self, (void)flag;
  if (!strcmp(cstr(e, name), "WebView.openWindow")) g_open_cb = ((NewRefFn)((void**)*e)[21])(e, cb);
  if (!strcmp(cstr(e, name), "Game.launch")) g_launch_cb = ((NewRefFn)((void**)*e)[21])(e, cb);
  return NULL;
}
void* Java_com_roblox_universalapp_messagebus_MessageBus_doSubscribeProtocolMethodRequestRaw(void** e, void* self, void* proto,
                                                                                         void* method, void* cb, unsigned char f) {
  (void)self, (void)f;
  if (strcmp(cstr(e, proto), "HapticProtocol")) return NULL;
  for (int i = 0; i < 4; ++i)
    if (!strcmp(cstr(e, method), g_haptic_names[i])) g_haptic_cb[i] = ((NewRefFn)((void**)*e)[21])(e, cb);
  return NULL;
}
void Java_com_roblox_universalapp_messagebus_MessageBus_publishProtocolMethodResponseRaw(void** e, void* self, void* proto,
                                                                                      void* method, void* json, int st, void* x) {
  (void)self, (void)st, (void)x;
  pthread_mutex_lock(&g_mu);
  snprintf(g_haptic_reply, sizeof g_haptic_reply, "%s.%s %s", cstr(e, proto), cstr(e, method), cstr(e, json));
  pthread_mutex_unlock(&g_mu);
}
void Java_com_roblox_engine_jni_NativeGLInterface_nativeUpdateMobileHapticsState(void** e, void* c, unsigned char on, void* m) {
  (void)e, (void)c, (void)m;
  g_haptics_state = on;
}
void Java_com_roblox_universalapp_messagebus_MessageBus_setRequestHandlerRaw(void** e, void* s, void* p, void* m, void* h) {
  (void)e, (void)s, (void)p, (void)m, (void)h;
}
void Java_com_roblox_universalapp_messagebus_MessageBus_publishRaw(void** e, void* s, void* n, void* j) {
  (void)e, (void)s, (void)n, (void)j;
}
void* Java_com_roblox_engine_jni_memstorage_MemStorage_bind(void** e, void* c, void* key, void* cb) {
  (void)c;
  if (!strcmp(cstr(e, key), "BrowserService.ExecuteJavaScript")) g_exec_cb = ((NewRefFn)((void**)*e)[21])(e, cb);
  return NULL;
}
void Java_com_roblox_engine_jni_memstorage_MemStorage_fire(void** e, void* c, void* k, void* v) {
  (void)e, (void)c, (void)k, (void)v;
}

static int wait_signals(int n, int seconds) {
  for (int i = 0; i < seconds * 10; ++i) {
    pthread_mutex_lock(&g_mu);
    const int have = g_nsignals;
    pthread_mutex_unlock(&g_mu);
    if (have >= n) return 1;
    usleep(100000);
  }
  return 0;
}

static void* engine(void* arg) {
  (void)arg;
  void** e = env();
  char json[1024];
  snprintf(json, sizeof json, "{\"url\":\"file://%s/tests/webview/bridge.html\",\"title\":\"test\",\"isVisible\":false}",
           getenv("PWD") ? getenv("PWD") : ".");
  for (int i = 0; i < 50 && !g_exec_cb; ++i) usleep(100000);  // tonic binds MemStorage after init
  if (!g_open_cb || !g_exec_cb) {
    printf("[webview-test] FAIL: tonic did not subscribe (open %p, exec %p)\n", g_open_cb, g_exec_cb);
    fflush(stdout);
    _exit(1);
  }
  call_string_method(e, g_open_cb, "run", json);
  if (!wait_signals(1, 20)) {
    printf("[webview-test] FAIL: no message from the page\n");
    fflush(stdout);
    _exit(1);
  }
  call_string_method(e, g_exec_cb, "onItemSet",
                     "window.__globalRobloxAndroidBridge__.executeRoblox('answer from ' + document.title)");
  const int ok = wait_signals(2, 20);
  printf("[webview-test] page said: %s\n[webview-test] script said: %s\n[webview-test] %s\n", g_signals[0],
         ok ? g_signals[1] : "(nothing)", ok && strstr(g_signals[1], "answer from bridge test") ? "ok" : "FAIL");
  fflush(stdout);
  if (!(ok && strstr(g_signals[1], "answer from bridge test"))) _exit(1);

  // Play: the app publishes Game.launch; tonic should start the game.
  for (int i = 0; i < 50 && !g_launch_cb; ++i) usleep(100000);
  if (!g_launch_cb) {
    printf("[launch-test] FAIL: tonic did not subscribe to Game.launch\n");
    fflush(stdout);
    _exit(1);
  }
  call_string_method(e, g_launch_cb, "run", "{\"placeId\":1818,\"joinAttemptOrigin\":\"test\"}");
  for (int i = 0; i < 50 && g_started_place < 0; ++i) usleep(100000);
  const int lok = g_started_place == 1818 && g_started_type == 0;
  printf("[launch-test] StartGameWithParam got place %lld, join type %d: %s\n", g_started_place, g_started_type,
         lok ? "ok" : "FAIL");
  fflush(stdout);
  if (!lok) _exit(1);

  // Rumble: tonic says the "phone" can vibrate, answers SupportsHaptics, takes a vibration.
  int subs = 0;
  for (int i = 0; i < 4; ++i) subs += g_haptic_cb[i] != NULL;
  if (subs != 4 || g_haptics_state != 1) {
    printf("[haptics-test] FAIL: %d of 4 requests subscribed, haptics state %d\n", subs, g_haptics_state);
    fflush(stdout);
    _exit(1);
  }
  call_string_method(e, g_haptic_cb[0], "run", "{}");
  call_string_method(e, g_haptic_cb[1], "run", "{\"intensity\":\"0.5\"}");
  int got = 0;
  for (int i = 0; i < 50 && !got; ++i) {
    pthread_mutex_lock(&g_mu);
    got = strstr(g_haptic_reply, "supportsHaptics\":true") != NULL;
    pthread_mutex_unlock(&g_mu);
    if (!got) usleep(100000);
  }
  usleep(300000);  // (the vibration is logged by tonic's rumble thread)
  printf("[haptics-test] subscribed, state reported, SupportsHaptics answered: %s\n", got ? "ok" : "FAIL");
  fflush(stdout);
  _exit(got ? 0 : 1);
  return NULL;
}

typedef long long (*CallLongFn)(void**, void*, void*, ...);
typedef int (*CallIntFn)(void**, void*, void*, ...);
int Java_com_roblox_engine_jni_NativeGLInterface_nativeAppBridgeV2StartGameWithParam(void** e, void* c, void* params) {
  (void)c;
  void** t = (void**)*e;
  void* cls = ((GetObjectClassFn)t[31])(e, params);
  g_started_type = ((CallIntFn)t[49])(e, params, ((GetIdFn)t[33])(e, cls, "joinRequestType", "()I"));
  g_started_place = ((CallLongFn)t[52])(e, params, ((GetIdFn)t[33])(e, cls, "placeId", "()J"));
  return 0;
}

void Java_com_roblox_protocols_webview_WebViewProtocol_initializeAndroidWebViewProtocol(void** e, void* c) {
  (void)e, (void)c;
  pthread_t th;
  pthread_create(&th, NULL, engine, NULL);
  pthread_detach(th);
}
