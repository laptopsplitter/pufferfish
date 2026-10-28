// Synthetic test library for tonic. Exercises: constructors, RELATIVE
// relocations (pointer table), GLOB_DAT/JUMP_SLOT to glibc, a bionic-only
// symbol (__errno), a weak missing symbol, an unimplemented one (stub), our
// dl_iterate_phdr shim (the library must find itself), and the abort reporter.
#define _GNU_SOURCE
#include <link.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// What a bionic-built caller passes and allocates. Declared by hand so this
// test (built against glibc headers) exercises the bionic ABI on purpose.
extern long sysconf(int);
typedef unsigned long bsigset_t;
struct bsigaction {
  int sa_flags;
  void (*handler)(int);
  bsigset_t sa_mask;
  void (*sa_restorer)(void);
};
extern int sigaction(int, const void*, void*);
extern int sigfillset(void*);
extern int setjmp(void*);
extern void longjmp(void*, int) __attribute__((noreturn));

static void on_usr1(int s) { (void)s; }

static int bionic_abi_ok(void) {
  int ok = 1;
  if (sysconf(0x27) != 4096 && sysconf(0x27) != 16384) {  // bionic _SC_PAGESIZE
    printf("testlib: sysconf(bionic _SC_PAGESIZE) = %ld\n", sysconf(0x27));
    ok = 0;
  }
  struct { bsigset_t set; unsigned char guard[64]; } g1;
  memset(g1.guard, 0xAB, sizeof g1.guard);
  sigfillset(&g1.set);
  struct { struct bsigaction act, old; unsigned char guard[64]; } g2;
  memset(&g2, 0, sizeof g2);
  memset(g2.guard, 0xAB, sizeof g2.guard);
  g2.act.handler = on_usr1;
  sigaction(10 /* SIGUSR1 */, &g2.act, NULL);
  sigaction(10, NULL, &g2.old);
  if (g2.old.handler != on_usr1) {
    printf("testlib: sigaction round trip failed\n");
    ok = 0;
  }
  struct { long jb[11]; unsigned char guard[64]; } g3;  // bionic x86_64 jmp_buf
  memset(g3.guard, 0xAB, sizeof g3.guard);
  volatile int jumped = 0;
  if (setjmp(g3.jb) == 0) longjmp(g3.jb, 1);
  jumped = 1;
  for (int i = 0; i < 64; ++i)
    if (g1.guard[i] != 0xAB || g2.guard[i] != 0xAB || g3.guard[i] != 0xAB) {
      printf("testlib: a shim wrote past a bionic-sized buffer\n");
      return 0;
    }
  return ok && jumped;
}

extern int* __errno(void);
extern void totally_missing_function(void);
extern void weak_missing(void) __attribute__((weak));

static const char* table[] = {"alpha", "beta", "gamma"};
static int ctor_ran = 0;

__attribute__((constructor)) static void ctor(void) { ctor_ran = 1; }

static int count_objects(struct dl_phdr_info* info, size_t size, void* data) {
  (void)info;
  (void)size;
  ++*(int*)data;
  return 0;
}

static int found_self(struct dl_phdr_info* info, size_t size, void* data) {
  (void)size;
  const ElfW(Addr) want = (ElfW(Addr))data;
  for (int i = 0; i < info->dlpi_phnum; ++i) {
    const ElfW(Phdr)* p = &info->dlpi_phdr[i];
    if (p->p_type == PT_LOAD && want >= info->dlpi_addr + p->p_vaddr &&
        want < info->dlpi_addr + p->p_vaddr + p->p_memsz)
      return 1;
  }
  return 0;
}

// Called with --call test_abort: should produce "[abort] ... libtest.so+0x..."
// and a stack listing that points back into this function.
__attribute__((noinline)) int test_abort(void* a, void* b) {
  (void)a;
  (void)b;
  abort();
}

// JNI by table index, so this builds without a JDK. Indexes are the JNI spec's.
typedef void* (*FindClassFn)(void**, const char*);
typedef void* (*GetIdFn)(void**, void*, const char*, const char*);
typedef void* (*NewStringUTFFn)(void**, const char*);
typedef const char* (*GetStringUTFCharsFn)(void**, void*, unsigned char*);
typedef int (*CallStaticIntFn)(void**, void*, void*, ...);
typedef struct { const char* name; const char* sig; void* fn; } NativeMethod;
typedef int (*RegisterNativesFn)(void**, void*, const NativeMethod*, int);
typedef int (*GetEnvFn)(void**, void**, int);

// Mimics what libroblox.so does to every env it is given: replace
// env->functions with a per-thread wrapper table, remember the original in a
// thread-local (a pthread key, as emutls uses), and forward through it. If two
// threads ever share one env, the second thread's "original" is the first
// thread's wrapper and ExceptionCheck forwards to itself forever.
typedef int (*ExceptionCheckFn)(void**);
static pthread_key_t orig_key, mine_key;
static pthread_once_t keys_once = PTHREAD_ONCE_INIT;
static void make_keys(void) {
  pthread_key_create(&orig_key, NULL);
  pthread_key_create(&mine_key, NULL);
}
static int wrapped_exception_check(void** env) {
  void** orig = pthread_getspecific(orig_key);
  return ((ExceptionCheckFn)orig[228])(env);
}
static void wrap_env(void** env) {
  pthread_once(&keys_once, make_keys);
  void** mine = pthread_getspecific(mine_key);
  if (!mine) {
    mine = calloc(233, sizeof(void*));
    pthread_setspecific(mine_key, mine);
  }
  void** cur = (void**)*env;
  if (cur == mine) return;
  pthread_setspecific(orig_key, cur);
  memcpy(mine, cur, 233 * sizeof(void*));
  mine[228] = (void*)wrapped_exception_check;
  *env = mine;
}
static int exception_check(void** env) { return ((ExceptionCheckFn)((void**)*env)[228])(env); }

typedef int (*AttachFn)(void**, void***, void*);
typedef int (*DetachFn)(void**);
struct ThreadResult {
  void** vm;
  void** main_env;
  int getenv_before_attach;
  int ok;
};
static void* other_thread(void* p) {
  struct ThreadResult* r = p;
  void** vmt = (void**)*r->vm;
  void** env = NULL;
  r->getenv_before_attach = ((GetEnvFn)vmt[6])(r->vm, (void**)&env, 0x10006);
  if (((AttachFn)vmt[4])(r->vm, &env, NULL) != 0 || !env) return NULL;
  wrap_env(env);
  r->ok = env != r->main_env && exception_check(env) == 0;
  ((DetachFn)vmt[5])(r->vm);
  return NULL;
}

static int per_thread_envs_ok(void** vm, void** env) {
  wrap_env(env);
  if (exception_check(env) != 0) return 0;
  struct ThreadResult r = {vm, env, 0, 0};
  pthread_t th;
  if (pthread_create(&th, NULL, other_thread, &r) != 0) return 0;
  pthread_join(th, NULL);
  if (r.getenv_before_attach != -2) {
    printf("testlib: GetEnv on an unattached thread returned %d, expected -2 (JNI_EDETACHED)\n",
           r.getenv_before_attach);
    return 0;
  }
  if (!r.ok) {
    printf("testlib: second thread did not get its own working env\n");
    return 0;
  }
  return exception_check(env) == 0;
}

static int native_add(void* env, void* cls, int a, int b) { (void)env; (void)cls; return a + b; }

static int use_jni(void** vm) {
  void** env = NULL;
  if (((GetEnvFn)((void**)*vm)[6])(vm, (void**)&env, 0x10006) != 0 || !env) return 0;
  void** t = (void**)*env;
  void* cls = ((FindClassFn)t[6])(env, "com/example/Test");
  void* mid = ((GetIdFn)t[113])(env, cls, "answer", "()I");
  int r = ((CallStaticIntFn)t[129])(env, cls, mid);
  void* str = ((NewStringUTFFn)t[167])(env, "hello");
  const char* back = ((GetStringUTFCharsFn)t[169])(env, str, NULL);
  NativeMethod m = {"add", "(II)I", (void*)native_add};
  int reg = ((RegisterNativesFn)t[215])(env, cls, &m, 1);
  if (!(cls && mid && r == 0 && strcmp(back, "hello") == 0 && reg == 0)) return 0;
  if (!per_thread_envs_ok((void**)vm, env)) return 0;
  printf("testlib: per-thread JNIEnv with library-style wrapping OK\n");
  return 1;
}

// Called with --call test_overflow: endless recursion, to check the stack
// overflow report.
__attribute__((noinline)) int test_overflow(void* a, void* b) {
  volatile char pad[256];
  pad[0] = (char)(long)a;
  return test_overflow(b, a) + pad[0];
}

// JNI-named natives for tests/natives.script (tonic --script).
static const char* jstr(void** env, void* s) { return s ? ((GetStringUTFCharsFn)((void**)*env)[169])(env, s, NULL) : NULL; }

// 9 integer-class arguments (3 go on the stack) and 3 floating-point ones.
long long Java_com_example_Test_mixed(void** env, void* cls, void* s1, int i, unsigned char z, float f, double d,
                                      long long j, void* s2, int i2, int i3, float f2) {
  (void)cls;
  const char* a = jstr(env, s1);
  const char* b = jstr(env, s2);
  if (!a || strcmp(a, "hi") || i != -7 || z != 1 || f != 1.5f || d != 2.25 || j != (1LL << 40) || !b ||
      strcmp(b, "there") || i2 != 100 || i3 != 200 || f2 != -0.5f) {
    printf("testlib: mixed() got the wrong arguments\n");
    return -1;
  }
  return j + i2 + i3;
}
int Java_com_example_Test_with_1underscore(void* env, void* cls) { (void)env; (void)cls; return 42; }
int Java_com_example_Test_over__I(void* env, void* cls, int x) { (void)env; (void)cls; return 1000 + x; }
float Java_com_example_Test_half(void* env, void* cls, float x) { (void)env; (void)cls; return x / 2; }
void* Java_com_example_Test_echo(void* env, void* obj, void* s) { (void)env; (void)obj; return s; }

// Asks "Java" for values the way libroblox does, for --values (tests/test.values):
// a static String method, a static float method, a static int field, and an
// object from an instance method whose class and float field are then read.
typedef float (*CallStaticFloatFn)(void**, void*, void*, ...);
typedef void* (*CallObjectFn)(void**, void*, void*, ...);
typedef void* (*CallStaticObjectFn)(void**, void*, void*, ...);
typedef int (*GetStaticIntFieldFn)(void**, void*, void*);
typedef float (*GetFloatFieldFn)(void**, void*, void*);
typedef void* (*GetObjectClassFn)(void**, void*);
void* Java_com_example_Test_askJava(void** env, void* self) {
  void** t = (void**)*env;
  char buf[256];
  void* c = ((FindClassFn)t[6])(env, "com/example/Test");
  void* s = ((CallStaticObjectFn)t[114])(env, c, ((GetIdFn)t[113])(env, c, "greeting", "()Ljava/lang/String;"));
  float f = ((CallStaticFloatFn)t[135])(env, c, ((GetIdFn)t[113])(env, c, "scale", "()F"));
  int n = ((GetStaticIntFieldFn)t[150])(env, c, ((GetIdFn)t[144])(env, c, "COUNT", "I"));
  void* p = ((CallObjectFn)t[34])(env, self, ((GetIdFn)t[33])(env, ((GetObjectClassFn)t[31])(env, self), "params", "()Lcom/example/Params;"));
  float dpi = -1;
  if (p) {
    void* pc = ((GetObjectClassFn)t[31])(env, p);
    dpi = ((GetFloatFieldFn)t[102])(env, p, ((GetIdFn)t[94])(env, pc, "dpi", "F"));
  }
  // Arguments are logged by the probe: plain (variadic) and A (jvalue array) forms.
  typedef void (*CallStaticVoidFn)(void**, void*, void*, ...);
  typedef void (*CallStaticVoidAFn)(void**, void*, void*, const unsigned long long*);
  void* note = ((GetIdFn)t[113])(env, c, "note", "(Ljava/lang/String;IFZ)V");
  ((CallStaticVoidFn)t[141])(env, c, note, ((NewStringUTFFn)t[167])(env, "plain"), 5, 2.5, 1);
  unsigned long long av[4] = {(unsigned long long)((NewStringUTFFn)t[167])(env, "array"), 6, 0, 0};
  float half = 0.5f;
  memcpy(&av[2], &half, 4);
  av[3] = 0;
  ((CallStaticVoidAFn)t[143])(env, c, note, av);
  snprintf(buf, sizeof buf, "%s|%g|%d|%g", s ? jstr(env, s) : "null", f, n, dpi);
  return ((NewStringUTFFn)t[167])(env, buf);
}

// Draws one frame the way an Android app does: ANativeWindow from a Surface,
// EGL on the default display with an Android-only config attribute, a GLES2
// context, a clear, a read-back and a swap. For --window (tests/window.script).
extern void* ANativeWindow_fromSurface(void* env, void* surface);
extern int ANativeWindow_getWidth(void* w);
extern void* eglGetDisplay(void* native);
extern unsigned eglInitialize(void* d, int* major, int* minor);
extern unsigned eglChooseConfig(void* d, const int* attribs, void** configs, int size, int* n);
extern void* eglCreateWindowSurface(void* d, void* c, void* w, const int* attribs);
extern void* eglCreateContext(void* d, void* c, void* share, const int* attribs);
extern unsigned eglMakeCurrent(void* d, void* draw, void* read, void* ctx);
extern unsigned eglSwapBuffers(void* d, void* s);
extern int eglGetError(void);
extern const unsigned char* glGetString(unsigned name);
extern void glClearColor(float r, float g, float b, float a);
extern void glClear(unsigned mask);
extern void glReadPixels(int x, int y, int w, int h, unsigned fmt, unsigned type, void* px);
static void *g_egl_d, *g_egl_s;
void* Java_com_example_Test_drawFrame(void** env, void* cls, void* surface) {
  (void)cls;
  void** t = (void**)*env;
  char buf[256];
  void* win = ANativeWindow_fromSurface(env, surface);
  void* d = eglGetDisplay(0);
  int major = 0, minor = 0;
  if (!win || !d || !eglInitialize(d, &major, &minor)) {
    snprintf(buf, sizeof buf, "no window/display (egl error %#x)", eglGetError());
    return ((NewStringUTFFn)t[167])(env, buf);
  }
  const int cfg_attribs[] = {0x3033, 4, 0x3040, 4, 0x3024, 8, 0x3023, 8, 0x3022, 8, 0x3142, 1, 0x3038};
  void* cfg = NULL;
  int n = 0;
  const int ctx_attribs[] = {0x3098, 2, 0x3038};
  void* s = NULL;
  void* ctx = NULL;
  if (!eglChooseConfig(d, cfg_attribs, &cfg, 1, &n) || n < 1 ||
      !(s = eglCreateWindowSurface(d, cfg, win, NULL)) || !(ctx = eglCreateContext(d, cfg, NULL, ctx_attribs)) ||
      !eglMakeCurrent(d, s, s, ctx)) {
    snprintf(buf, sizeof buf, "egl setup failed (error %#x)", eglGetError());
    return ((NewStringUTFFn)t[167])(env, buf);
  }
  printf("testlib: EGL %d.%d, GL_RENDERER %s, window width %d\n", major, minor, glGetString(0x1F01),
         ANativeWindow_getWidth(win));
  glClearColor(1, 0, 1, 1);
  glClear(0x4000);
  unsigned char px[4] = {0};
  glReadPixels(0, 0, 1, 1, 0x1908, 0x1401, px);
  eglSwapBuffers(d, s);
  g_egl_d = d, g_egl_s = s;
  snprintf(buf, sizeof buf, "ok:%d,%d,%d", px[0], px[1], px[2]);
  return ((NewStringUTFFn)t[167])(env, buf);
}

// Reads tests/assets/hello.txt through AAssetManager, as libroblox reads its
// shader packs: by buffer, and by file descriptor.
extern void* AAssetManager_fromJava(void* env, void* mgr);
extern void* AAssetManager_open(void* mgr, const char* name, int mode);
extern const void* AAsset_getBuffer(void* a);
extern long AAsset_getLength(void* a);
extern int AAsset_openFileDescriptor(void* a, long* start, long* len);
extern void AAsset_close(void* a);
extern long read(int fd, void* buf, unsigned long n);
extern int close(int fd);
void* Java_com_example_Test_readAsset(void** env, void* cls, void* mgr_obj, void* name) {
  (void)cls;
  void** t = (void**)*env;
  char buf[128] = "missing";
  void* mgr = AAssetManager_fromJava(env, mgr_obj);
  void* a = AAssetManager_open(mgr, jstr(env, name), 3 /* AASSET_MODE_BUFFER */);
  if (a) {
    const long n = AAsset_getLength(a);
    const char* p = AAsset_getBuffer(a);
    long start = -1, len = -1;
    char viafd[64] = {0};
    const int fd = AAsset_openFileDescriptor(a, &start, &len);
    if (fd >= 0) {
      read(fd, viafd, len < 63 ? len : 63);
      close(fd);
    }
    snprintf(buf, sizeof buf, "%ld:%.*s:%s", n, (int)n, p ? p : "", strcmp(viafd, p ? p : "") ? "fd-differs" : "fd-same");
    AAsset_close(a);
  }
  return ((NewStringUTFFn)t[167])(env, buf);
}

// getaddrinfo with bionic's struct addrinfo (canonname before addr) and
// bionic's AI_* values, as libroblox calls it. Resolves NAME numerically.
struct baddrinfo {
  int ai_flags, ai_family, ai_socktype, ai_protocol;
  unsigned ai_addrlen;
  char* ai_canonname;
  void* ai_addr;
  struct baddrinfo* ai_next;
};
extern int getaddrinfo(const char*, const char*, const void*, void*);
extern void freeaddrinfo(void*);
void* Java_com_example_Test_resolve(void** env, void* cls, void* name) {
  (void)cls;
  void** t = (void**)*env;
  char buf[128];
  struct baddrinfo hints = {0x4 /* AI_NUMERICHOST */ | 0x400 /* AI_ADDRCONFIG */, 0, 1 /* SOCK_STREAM */, 0, 0, 0, 0, 0};
  struct baddrinfo* res = 0;
  const int r = getaddrinfo(jstr(env, name), "443", &hints, &res);
  if (r != 0) {
    snprintf(buf, sizeof buf, "error %d", r);
  } else {
    const unsigned char* sa = res->ai_addr;  // sockaddr_in: family(2) port(2) addr(4)
    snprintf(buf, sizeof buf, "family %d port %d addr %d.%d.%d.%d", sa[0] | sa[1] << 8, sa[2] << 8 | sa[3], sa[4],
             sa[5], sa[6], sa[7]);
    freeaddrinfo(res);
  }
  return ((NewStringUTFFn)t[167])(env, buf);
}

// A message bus in miniature, for "on" rules: subscribe() keeps a callback,
// fire() calls callback.run(message) the way libroblox delivers a request, and
// publish() records the answer a rule sends back.
static void* g_callback;
static char g_published[256];
typedef void (*CallVoidFn)(void**, void*, void*, ...);
typedef void* (*NewGlobalRefFn)(void**, void*);
void Java_com_example_Test_subscribe(void** env, void* cls, void* name, void* callback) {
  (void)cls;
  (void)name;
  g_callback = ((NewGlobalRefFn)((void**)*env)[21])(env, callback);
}
void Java_com_example_Test_fire(void** env, void* cls, void* msg) {
  (void)cls;
  void** t = (void**)*env;
  void* run = ((GetIdFn)t[33])(env, ((GetObjectClassFn)t[31])(env, g_callback), "run", "(Ljava/lang/String;)V");
  ((CallVoidFn)t[61])(env, g_callback, run, msg);
}
void Java_com_example_Test_publish(void** env, void* cls, void* s) {
  (void)cls;
  char tmp[256];
  snprintf(tmp, sizeof tmp, "%s", jstr(env, s));
  memcpy(g_published + 1, tmp + 1, sizeof tmp - 1);
  __atomic_store_n(&g_published[0], tmp[0], __ATOMIC_RELEASE);
}
// Rules answer from another thread, so wait (up to 2 s) for the answer.
extern int usleep(unsigned usec);
void* Java_com_example_Test_waitPublished(void** env, void* cls) {
  (void)cls;
  for (int i = 0; i < 200 && !__atomic_load_n(&g_published[0], __ATOMIC_ACQUIRE); ++i) usleep(10000);
  void* r = ((NewStringUTFFn)((void**)*env)[167])(env, g_published);
  __atomic_store_n(&g_published[0], 0, __ATOMIC_RELEASE);  // the next wait waits for a new answer
  return r;
}

// Sends a static notification to "Java", like onDataModelNotificationCallback.
void Java_com_example_Test_notifyJava(void** env, void* cls, void* what) {
  void** t = (void**)*env;
  void* c = ((FindClassFn)t[6])(env, "com/example/Test");
  (void)cls;
  void* ev = ((GetIdFn)t[113])(env, c, "event", "(Ljava/lang/String;)V");
  ((CallVoidFn)t[141])(env, c, ev, what);
}

// A message-bus style request handler: the library calls handler.run(params)
// and uses the String it returns. Values for labelled objects (--values
// CLASS#LABEL...) let two handlers of one class answer differently.
typedef void* (*CallObjectMethodFn)(void**, void*, void*, ...);
void* Java_com_example_Test_askHandler(void** env, void* cls, void* handler, void* params) {
  (void)cls;
  void** t = (void**)*env;
  void* run = ((GetIdFn)t[33])(env, ((GetObjectClassFn)t[31])(env, handler), "run", "(Ljava/lang/String;)Ljava/lang/String;");
  void* r = ((CallObjectMethodFn)t[34])(env, handler, run, params);
  return r ? r : ((NewStringUTFFn)t[167])(env, "null");
}
// Calls cb.run(params, requestId), as the library calls an async handler.
void Java_com_example_Test_fireAsync(void** env, void* cls, void* params, void* id) {
  (void)cls;
  void** t = (void**)*env;
  void* run = ((GetIdFn)t[33])(env, ((GetObjectClassFn)t[31])(env, g_callback), "run", "(Ljava/lang/String;Ljava/lang/String;)V");
  ((CallVoidFn)t[61])(env, g_callback, run, params, id);
}

int JNI_OnLoad(void* vm, void* reserved) {
  (void)reserved;
  int ok = 1;
  ok &= ctor_ran;
  ok &= strcmp(table[1], "beta") == 0;
  *__errno() = 42;
  ok &= *__errno() == 42;
  ok &= weak_missing == NULL;
  ok &= bionic_abi_ok();
  if (vm && !use_jni((void**)vm)) {
    printf("testlib: fake JNI round trip failed\n");
    ok = 0;
  }
  if (!dl_iterate_phdr(found_self, (void*)&table)) {
    printf("testlib: dl_iterate_phdr did not report this library\n");
    ok = 0;
  }
  // Only our images: host objects (with g++ unwind info) must stay invisible
  // to the library's own unwinder.
  int objects = 0;
  dl_iterate_phdr(count_objects, &objects);
  if (objects != 1) {
    printf("testlib: dl_iterate_phdr reported %d objects, expected only this library\n", objects);
    ok = 0;
  }
  if (!ok) return -1;
  printf("testlib: constructor, relocations, shims, bionic ABI and dl_iterate_phdr OK\n");
  totally_missing_function();  // should hit the logging stub and return 0
  return 0x10006;              // JNI_VERSION_1_6
}

// Byte arrays, as libroblox passes a TextBox's text to Java's showKeyboard:
// make one, fill it, read it back two ways, and return what was read.
typedef void* (*NewByteArrayFn)(void**, int);
typedef int (*GetArrayLengthFn)(void**, void*);
typedef void (*ByteRegionFn)(void**, void*, int, int, void*);
typedef signed char* (*GetByteElementsFn)(void**, void*, unsigned char*);
void* Java_com_example_Test_bytes(void** env, void* cls, void* s) {
  (void)cls;
  void** t = (void**)*env;
  const char* in = ((GetStringUTFCharsFn)t[169])(env, s, NULL);
  const int n = (int)strlen(in);
  void* arr = ((NewByteArrayFn)t[176])(env, n);
  ((ByteRegionFn)t[208])(env, arr, 0, n, (void*)in);
  char buf[128] = {0};
  const int len = ((GetArrayLengthFn)t[171])(env, arr);
  if (len != n || len >= (int)sizeof buf - 16) return ((NewStringUTFFn)t[167])(env, "bad length");
  ((ByteRegionFn)t[200])(env, arr, 0, len, buf);
  signed char* direct = ((GetByteElementsFn)t[184])(env, arr, NULL);
  if (memcmp(direct, buf, len) != 0) return ((NewStringUTFFn)t[167])(env, "elements differ");
  snprintf(buf + len, 16, ":%d", len);
  return ((NewStringUTFFn)t[167])(env, buf);
}

// TextBox text must never reach the log: calls the "Java" callback libroblox
// uses for TextBox changes with a String and a byte[] of secret text.
typedef void (*CallStaticVoidFn2)(void**, void*, void*, ...);
void Java_com_example_Test_typed(void** env, void* cls) {
  (void)cls;
  void** t = (void**)*env;
  void* c = ((FindClassFn)t[6])(env, "com/roblox/engine/jni/NativeGLJavaInterface");
  void* changed = ((GetIdFn)t[113])(env, c, "onLuaTextBoxChangedCallback", "(Ljava/lang/String;)V");
  ((CallStaticVoidFn2)t[141])(env, c, changed, ((NewStringUTFFn)t[167])(env, "hunter2-secret"));
  void* show = ((GetIdFn)t[113])(env, c, "showKeyboard", "(JZ[BLcom/roblox/engine/jni/model/NativeTextBoxInfo;)V");
  void* arr = ((NewByteArrayFn)t[176])(env, 14);
  ((ByteRegionFn)t[208])(env, arr, 0, 14, (void*)"hunter2-secret");
  ((CallStaticVoidFn2)t[141])(env, c, show, (long long)7, 1, arr, (void*)0);
}

// Reads a String's text the way libroblox does for TextBox input:
// String.getBytes("UTF-8"), then the byte[]'s length and contents.
void* Java_com_example_Test_viaBytes(void** env, void* cls, void* s) {
  (void)cls;
  void** t = (void**)*env;
  void* sc = ((FindClassFn)t[6])(env, "java/lang/String");
  void* gb = ((GetIdFn)t[33])(env, sc, "getBytes", "(Ljava/lang/String;)[B");
  void* arr = ((CallObjectFn)t[34])(env, s, gb, ((NewStringUTFFn)t[167])(env, "UTF-8"));
  if (!arr) return ((NewStringUTFFn)t[167])(env, "null");
  char buf[64] = {0};
  const int n = ((GetArrayLengthFn)t[171])(env, arr);
  if (n < 0 || n >= (int)sizeof buf) return ((NewStringUTFFn)t[167])(env, "too long");
  ((ByteRegionFn)t[200])(env, arr, 0, n, buf);
  return ((NewStringUTFFn)t[167])(env, buf);
}

// FMOD's AudioTrack output, as libroblox drives it: org/fmod/AudioDevice
// init(2 ch, 48000 Hz, 512 frames, 4 blocks), a few writes, close. Returns
// "init:1 writes:N" (run with SDL_AUDIODRIVER=dummy where there is no sound).
typedef void* (*AllocObjectFn)(void**, void*);
typedef unsigned char (*CallBoolFn)(void**, void*, void*, ...);
void* Java_com_example_Test_audio(void** env, void* cls) {
  (void)cls;
  void** t = (void**)*env;
  void* dc = ((FindClassFn)t[6])(env, "org/fmod/AudioDevice");
  void* dev = ((AllocObjectFn)t[27])(env, dc);
  void* init = ((GetIdFn)t[33])(env, dc, "init", "(IIII)Z");
  void* write = ((GetIdFn)t[33])(env, dc, "write", "([BI)V");
  void* close = ((GetIdFn)t[33])(env, dc, "close", "()V");
  const unsigned char ok = ((CallBoolFn)t[37])(env, dev, init, 2, 48000, 512, 4);
  int writes = 0;
  if (ok) {
    void* buf = ((NewByteArrayFn)t[176])(env, 2048);
    for (; writes < 8; ++writes) ((CallVoidFn)t[61])(env, dev, write, buf, 2048);
    ((CallVoidFn)t[61])(env, dev, close);
  }
  char out[64];
  snprintf(out, sizeof out, "init:%d writes:%d", ok, writes);
  return ((NewStringUTFFn)t[167])(env, out);
}

// Login cookies (src/cookies.cpp): the library side of CookieProtocol.
// Registering the handler makes it report three Set-Cookie headers at once;
// nativeSetMultipleCookies records what tonic restores.
static char g_restored[512];
void Java_com_roblox_engine_jni_NativeSettingsInterface_nativeSetMultipleCookies(void** env, void* cls, void* url, void* c) {
  (void)cls;
  snprintf(g_restored, sizeof g_restored, "%s|%s", jstr(env, url), jstr(env, c));
}
typedef void* (*NewObjectArrayFn)(void**, int, void*, void*);
typedef void (*SetObjectArrayElementFn)(void**, void*, int, void*);
void Java_com_roblox_universalapp_cookie_JNICookieProtocol_updateOnSetCookieHandler(void** env, void* self, void* h) {
  (void)self;
  void** t = (void**)*env;
  static const char* headers[] = {"a=1; Path=/; Secure", ".ROBLOSECURITY=testsessionvalue; Max-Age=3600; HttpOnly",
                                  "gone=; Max-Age=0"};
  void* arr = ((NewObjectArrayFn)t[172])(env, 3, ((FindClassFn)t[6])(env, "java/lang/String"), NULL);
  for (int i = 0; i < 3; ++i) ((SetObjectArrayElementFn)t[174])(env, arr, i, ((NewStringUTFFn)t[167])(env, headers[i]));
  void* m = ((GetIdFn)t[33])(env, ((GetObjectClassFn)t[31])(env, h), "onSetCookie",
                             "([Ljava/lang/String;Ljava/lang/String;)V");
  ((CallVoidFn)t[61])(env, h, m, arr, ((NewStringUTFFn)t[167])(env, "https://www.roblox.com"));
}
void* Java_com_example_Test_cookies(void** env, void* cls) {
  (void)cls;
  return ((NewStringUTFFn)((void**)*env)[167])(env, g_restored);
}

// A TextBox gets focus the way libroblox reports it (showKeyboard with a
// NativeTextBoxInfo built by NewObject), then a frame is presented: tonic
// should draw the text over the box (src/textoverlay.cpp; the check is in its
// log with TONIC_TEXTBOX_CHECK=1). After drawFrame.
typedef void* (*NewObjectFn)(void**, void*, void*, ...);
typedef void (*SetByteArrayRegionFn)(void**, void*, int, int, const void*);
void* Java_com_example_Test_typedFrame(void** env, void* cls) {
  (void)cls;
  void** t = (void**)*env;
  if (!g_egl_d) return ((NewStringUTFFn)t[167])(env, "no frame drawn yet");
  void* info_cls = ((FindClassFn)t[6])(env, "com/roblox/engine/jni/model/NativeTextBoxInfo");
  void* ctor = ((GetIdFn)t[33])(env, info_cls, "<init>", "(FFFFFZIIIIIIZZZFFFF)V");
  void* info = ((NewObjectFn)t[28])(env, info_cls, ctor, 10.0, 10.0, 200.0, 60.0, 40.0, 0, 0, 1, (int)0xFFFFFFFF, 0, 1,
                                    0, 0, 0, 1, 4.0, 0.0, 4.0, 0.0);
  void* text = ((NewByteArrayFn)t[176])(env, 6);
  ((SetByteArrayRegionFn)t[208])(env, text, 0, 6, "WWWWWW");
  void* jgl = ((FindClassFn)t[6])(env, "com/roblox/engine/jni/NativeGLJavaInterface");
  void* show = ((GetIdFn)t[113])(env, jgl, "showKeyboard", "(JZ[BLcom/roblox/engine/jni/model/NativeTextBoxInfo;)V");
  ((CallStaticVoidFn2)t[141])(env, jgl, show, (long long)0x1234, 0, text, info);
  glClearColor(0, 0, 0, 1);
  glClear(0x4000);
  eglSwapBuffers(g_egl_d, g_egl_s);
  return ((NewStringUTFFn)t[167])(env, "ok");
}

// ALooper (src/looper.cpp): a pipe on this thread's looper, with a callback,
// run by pollOnce; and the main thread already has a looper.
extern void* ALooper_forThread(void);
extern void* ALooper_prepare(int opts);
extern int ALooper_addFd(void* l, int fd, int ident, int events, int (*cb)(int, int, void*), void* data);
extern int ALooper_pollOnce(int timeout, int* fd, int* events, void** data);
static int g_looper_hits;
static int on_pipe(int fd, int events, void* data) {
  char c;
  (void)events;
  if (read(fd, &c, 1) == 1 && data == &g_looper_hits) ++g_looper_hits;
  return 1;
}
void* Java_com_example_Test_looper(void** env, void* cls) {
  (void)cls;
  char buf[64];
  const int had = ALooper_forThread() != NULL;
  void* l = ALooper_prepare(0);
  int p[2];
  if (pipe(p) != 0) return ((NewStringUTFFn)((void**)*env)[167])(env, "pipe failed");
  ALooper_addFd(l, p[0], -2, 1, on_pipe, &g_looper_hits);
  const int t = ALooper_pollOnce(0, NULL, NULL, NULL);  // nothing yet: timeout (-3)
  if (write(p[1], "x", 1) != 1) {}
  const int r = ALooper_pollOnce(1000, NULL, NULL, NULL);  // callback (-2)
  snprintf(buf, sizeof buf, "main:%d idle:%d ready:%d hits:%d", had, t, r, g_looper_hits);
  return ((NewStringUTFFn)((void**)*env)[167])(env, buf);
}

// Vulkan the way libroblox does it on Android: dlopen("libvulkan.so"),
// VK_KHR_android_surface on the ANativeWindow, a swapchain, frames cleared to
// magenta (make test-vulkan: tonic's layer must turn that into a desktop
// window surface). Needs the Vulkan headers to build; without them it says so.
#if __has_include(<vulkan/vulkan_core.h>)
#include <dlfcn.h>
#include <vulkan/vulkan_core.h>
typedef struct {
  VkStructureType sType;
  const void* pNext;
  VkFlags flags;
  void* window;
} AndroidSurfaceInfo;
void* Java_com_example_Test_vulkanFrames(void** env, void* cls, void* surface) {
  (void)cls;
  void** t = (void**)*env;
  char buf[256];
#define FAIL(...) do { snprintf(buf, sizeof buf, __VA_ARGS__); return ((NewStringUTFFn)t[167])(env, buf); } while (0)
  void* win = ANativeWindow_fromSurface(env, surface);
  void* lib = dlopen("libvulkan.so", RTLD_NOW);
  if (!lib) FAIL("no libvulkan.so");
  PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
#define I(n) PFN_vk##n n = (PFN_vk##n)gipa(inst, "vk" #n)
  VkInstance inst = VK_NULL_HANDLE;
  I(EnumerateInstanceExtensionProperties);
  I(CreateInstance);
  uint32_t ne = 0;
  EnumerateInstanceExtensionProperties(NULL, &ne, NULL);
  VkExtensionProperties ext[256];
  if (ne > 256) ne = 256;
  EnumerateInstanceExtensionProperties(NULL, &ne, ext);
  int android = 0;
  for (uint32_t i = 0; i < ne; ++i) android |= !strcmp(ext[i].extensionName, "VK_KHR_android_surface");
  if (!android) FAIL("VK_KHR_android_surface not offered");
  const char* iexts[] = {"VK_KHR_surface", "VK_KHR_android_surface"};
  VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "test", 1, "test", 1, VK_API_VERSION_1_1};
  VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 2, iexts};
  if (CreateInstance(&ici, NULL, &inst) != VK_SUCCESS) FAIL("vkCreateInstance failed");
  PFN_vkVoidFunction cas = gipa(inst, "vkCreateAndroidSurfaceKHR");
  AndroidSurfaceInfo si = {(VkStructureType)1000008000, NULL, 0, win};
  VkSurfaceKHR surf;
  if (((VkResult(*)(VkInstance, const void*, const void*, VkSurfaceKHR*))cas)(inst, &si, NULL, &surf) != VK_SUCCESS)
    FAIL("vkCreateAndroidSurfaceKHR failed");
  I(EnumeratePhysicalDevices);
  I(GetPhysicalDeviceQueueFamilyProperties);
  I(GetPhysicalDeviceSurfaceSupportKHR);
  I(GetPhysicalDeviceSurfaceCapabilitiesKHR);
  I(CreateDevice);
  I(GetDeviceProcAddr);
  uint32_t np = 1;
  VkPhysicalDevice phys;
  EnumeratePhysicalDevices(inst, &np, &phys);
  if (!np) FAIL("no GPU");
  uint32_t nq = 0, fam = ~0u;
  GetPhysicalDeviceQueueFamilyProperties(phys, &nq, NULL);
  VkQueueFamilyProperties qf[16];
  if (nq > 16) nq = 16;
  GetPhysicalDeviceQueueFamilyProperties(phys, &nq, qf);
  for (uint32_t i = 0; i < nq && fam == ~0u; ++i) {
    VkBool32 ok = 0;
    GetPhysicalDeviceSurfaceSupportKHR(phys, i, surf, &ok);
    if (ok && (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) fam = i;
  }
  if (fam == ~0u) FAIL("no queue can present");
  float prio = 1;
  VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, fam, 1, &prio};
  const char* dexts[] = {"VK_KHR_swapchain", "VK_ANDROID_external_memory_android_hardware_buffer"};
  VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qci, 0, NULL, 2, dexts, NULL};
  VkDevice dev;
  if (CreateDevice(phys, &dci, NULL, &dev) != VK_SUCCESS) FAIL("vkCreateDevice failed");
#define D(n) PFN_vk##n n = (PFN_vk##n)GetDeviceProcAddr(dev, "vk" #n)
  D(GetDeviceQueue); D(CreateSwapchainKHR); D(GetSwapchainImagesKHR); D(AcquireNextImageKHR); D(QueuePresentKHR);
  D(CreateCommandPool); D(AllocateCommandBuffers); D(BeginCommandBuffer); D(EndCommandBuffer); D(CmdPipelineBarrier);
  D(CmdClearColorImage); D(QueueSubmit); D(QueueWaitIdle); D(CreateSemaphore); D(ResetCommandBuffer);
  VkQueue q;
  GetDeviceQueue(dev, fam, 0, &q);
  VkSurfaceCapabilitiesKHR caps;
  GetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surf, &caps);
  if (caps.currentExtent.width == 0xFFFFFFFFu) FAIL("surface size not filled in");
  VkSwapchainCreateInfoKHR sci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
  sci.surface = surf;
  sci.minImageCount = caps.minImageCount + 1;
  sci.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
  sci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
  sci.imageExtent = caps.currentExtent;
  sci.imageArrayLayers = 1;
  sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  sci.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
  sci.compositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;  // Android's usual choice
  sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
  sci.clipped = 1;
  VkSwapchainKHR sc;
  if (CreateSwapchainKHR(dev, &sci, NULL, &sc) != VK_SUCCESS) FAIL("swapchain failed");
  uint32_t ni = 8;
  VkImage img[8];
  GetSwapchainImagesKHR(dev, sc, &ni, img);
  VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, fam};
  VkCommandPool pool;
  CreateCommandPool(dev, &pci, NULL, &pool);
  VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
  VkCommandBuffer cb;
  AllocateCommandBuffers(dev, &cai, &cb);
  VkSemaphoreCreateInfo sei = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, NULL, 0};
  VkSemaphore acquired, rendered;
  CreateSemaphore(dev, &sei, NULL, &acquired);
  CreateSemaphore(dev, &sei, NULL, &rendered);
  {  // a focused TextBox, as in typedFrame: tonic draws the typed text on the Vulkan frames too
    void* info_cls = ((FindClassFn)t[6])(env, "com/roblox/engine/jni/model/NativeTextBoxInfo");
    void* ctor = ((GetIdFn)t[33])(env, info_cls, "<init>", "(FFFFFZIIIIIIZZZFFFF)V");
    void* info = ((NewObjectFn)t[28])(env, info_cls, ctor, 10.0, 10.0, 200.0, 60.0, 40.0, 0, 0, 1, (int)0xFFFFFFFF, 0, 1,
                                      0, 0, 0, 1, 4.0, 0.0, 4.0, 0.0);
    void* text = ((NewByteArrayFn)t[176])(env, 6);
    ((SetByteArrayRegionFn)t[208])(env, text, 0, 6, "WWWWWW");
    void* jgl = ((FindClassFn)t[6])(env, "com/roblox/engine/jni/NativeGLJavaInterface");
    void* show = ((GetIdFn)t[113])(env, jgl, "showKeyboard", "(JZ[BLcom/roblox/engine/jni/model/NativeTextBoxInfo;)V");
    ((CallStaticVoidFn2)t[141])(env, jgl, show, (long long)0x1234, 0, text, info);
  }
  const char* nf = getenv("TEST_VK_FRAMES");
  const int frames = nf ? atoi(nf) : 120;
  int presented = 0;
  for (int f = 0; f < frames; ++f) {
    uint32_t idx;
    if (AcquireNextImageKHR(dev, sc, UINT64_MAX, acquired, VK_NULL_HANDLE, &idx) < 0) break;
    ResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, 0, NULL};
    BeginCommandBuffer(cb, &bi);
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                              VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED,
                              VK_QUEUE_FAMILY_IGNORED, img[idx], {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    VkClearColorValue magenta = {{1, 0, 1, 1}};
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    CmdClearColorImage(cb, img[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &magenta, 1, &range);
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = 0;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    EndCommandBuffer(cb);
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo s = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 1, &acquired, &stage, 1, &cb, 1, &rendered};
    QueueSubmit(q, 1, &s, VK_NULL_HANDLE);
    VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, NULL, 1, &rendered, 1, &sc, &idx, NULL};
    if (QueuePresentKHR(q, &pi) >= 0) ++presented;
    QueueWaitIdle(q);
  }
  printf("testlib: Vulkan presented %d of %d frames (%ux%u)\n", presented, frames, caps.currentExtent.width,
         caps.currentExtent.height);
  if (getenv("TEST_VK_HANG"))  // like Roblox idling: no more frames, but still running
    for (;;) sleep(1000);
  FAIL("ok:vulkan %d", presented);
}
#endif

// Gamepads (make test-gamepad): what tonic passes, as Android's app would.
void Java_com_roblox_engine_jni_NativeInputInterface_nativeGamepadConnectEventWithGamepadType(void* env, void* cls, int id, int type) {
  (void)env, (void)cls;
  printf("testlib: pad %d connected, type %d\n", id, type);
}
void Java_com_roblox_engine_jni_NativeInputInterface_nativeGamepadDisconnectEvent(void* env, void* cls, int id) {
  (void)env, (void)cls;
  printf("testlib: pad %d disconnected\n", id);
}
void Java_com_roblox_engine_jni_NativeInputInterface_nativeGamepadButtonEvent(void* env, void* cls, int id, int key, int down) {
  (void)env, (void)cls;
  printf("testlib: pad %d key %d %s\n", id, key, down ? "down" : "up");
}
void Java_com_roblox_engine_jni_NativeInputInterface_nativeGamepadAxisEvent(void* env, void* cls, int id, int axis, float x, float y, float z) {
  (void)env, (void)cls;
  if (x != 0 || y != 0 || z != 0) printf("testlib: pad %d axis %d %.2f %.2f %.2f\n", id, axis, x, y, z);
}
