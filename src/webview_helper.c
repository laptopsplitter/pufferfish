// tonic-webview: the WebKitGTK window that stands in for Android's WebView,
// in its own process. One line per message each way (\\, \n, \r escaped):
//   in:  eval SCRIPT | show | hide | title TEXT | close
//   out: ready | loaded URL | js TEXT | closed | console TEXT | log TEXT
//   --login also sends: session EXPIRES\tVALUE (never logged), user ID\tNAME\tDISPLAY
// Only https roblox.com pages can call into tonic; permission requests are refused.
#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <webkit2/webkit2.h>

static GtkWidget* g_window;
static WebKitWebView* g_view;
static gboolean g_loaded = FALSE;
static GPtrArray* g_pending;  // scripts waiting for the page to load

// --login
static gboolean g_login = FALSE;
static char* g_session = NULL;  // the .ROBLOSECURITY value, once roblox.com set it
static gint64 g_session_expires = 0;
static const char* kWhoAmI = "https://users.roblox.com/v1/users/authenticated";

static void emit(const char* kind, const char* text) {
  GString* s = g_string_new(kind);
  if (text) {
    g_string_append_c(s, ' ');
    for (const char* p = text; *p; ++p) {
      if (*p == '\\') g_string_append(s, "\\\\");
      else if (*p == '\n') g_string_append(s, "\\n");
      else if (*p == '\r') g_string_append(s, "\\r");
      else g_string_append_c(s, *p);
    }
  }
  g_string_append_c(s, '\n');
  fputs(s->str, stdout);
  fflush(stdout);
  g_string_free(s, TRUE);
}

static char* unescape(const char* in) {
  GString* s = g_string_new(NULL);
  for (const char* p = in; *p; ++p) {
    if (*p == '\\' && p[1]) {
      ++p;
      g_string_append_c(s, *p == 'n' ? '\n' : *p == 'r' ? '\r' : *p);
    } else {
      g_string_append_c(s, *p);
    }
  }
  return g_string_free(s, FALSE);
}

static void run_script(const char* script) {
  webkit_web_view_evaluate_javascript(g_view, script, -1, NULL, NULL, NULL, NULL, NULL);
}

// https://roblox.com/... or https://<anything>.roblox.com/...
static gboolean test_files(void) { return getenv("TONIC_WEBVIEW_TEST_FILES") != NULL; }  // make test-webview's page

static gboolean roblox_page(const char* uri) {
  if (uri && test_files() && g_str_has_prefix(uri, "file:")) return TRUE;
  if (!uri || !g_str_has_prefix(uri, "https://")) return FALSE;
  const char* h = uri + 8;
  size_t n = strcspn(h, "/:?#");
  if (memchr(h, '@', n)) return FALSE;  // user@host
  static const char kDom[] = "roblox.com";
  const size_t k = sizeof kDom - 1;
  if (n == k) return g_ascii_strncasecmp(h, kDom, k) == 0;
  return n > k + 1 && h[n - k - 1] == '.' && g_ascii_strncasecmp(h + n - k, kDom, k) == 0;
}

static void on_bridge(WebKitUserContentManager* m, WebKitJavascriptResult* r, gpointer d) {
  (void)m, (void)d;
  if (!roblox_page(webkit_web_view_get_uri(g_view))) {
    emit("log", "ignored a call from a page that is not Roblox's");
    return;
  }
  JSCValue* v = webkit_javascript_result_get_js_value(r);
  char* text = jsc_value_to_string(v);
  emit("js", text);
  g_free(text);
}

// --- --login ------------------------------------------------------------------

static void login_check_cookies(void);

// The answer of users.roblox.com/v1/users/authenticated: {"id":N,"name":"..",...}
static void on_whoami(GObject* src, GAsyncResult* res, gpointer d) {
  (void)d;
  JSCValue* v = webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(src), res, NULL);
  char* text = v ? jsc_value_to_string(v) : NULL;
  long long id = 0;
  char name[64] = "", display[64] = "";
  if (text) {
    const char* p = strstr(text, "\"id\"");
    if (p && (p = strchr(p, ':'))) id = strtoll(p + 1, NULL, 10);
    const char* n = strstr(text, "\"name\"");
    if (n && (n = strchr(n, ':')) && (n = strchr(n, '"'))) {
      ++n;
      size_t i = 0;
      while (*n && *n != '"' && i + 1 < sizeof name) name[i++] = *n++;
      name[i] = 0;
    }
    const char* dn = strstr(text, "\"displayName\"");
    if (dn && (dn = strchr(dn, ':')) && (dn = strchr(dn, '"'))) {
      ++dn;
      size_t i = 0;
      while (*dn && *dn != '"' && i + 1 < sizeof display) display[i++] = *dn++;
      display[i] = 0;
    }
  }
  if (id > 0 && g_session) {
    GString* sess = g_string_new(NULL);
    g_string_printf(sess, "%" G_GINT64_FORMAT "\t%s", g_session_expires, g_session);
    emit("session", sess->str);
    g_string_free(sess, TRUE);
    char user[160];
    snprintf(user, sizeof user, "%lld\t%s\t%s", id, name, *display ? display : name);
    emit("user", user);
    gtk_main_quit();
  } else {
    emit("log", "Roblox did not accept the session; back to the sign-in page");
    g_free(g_session);
    g_session = NULL;
    webkit_web_view_load_uri(WEBKIT_WEB_VIEW(src), "https://www.roblox.com/login");
  }
  g_free(text);
  if (v) g_object_unref(v);
}

static void on_cookies(GObject* src, GAsyncResult* res, gpointer d) {
  (void)d;
  GList* list = webkit_cookie_manager_get_cookies_finish(WEBKIT_COOKIE_MANAGER(src), res, NULL);
  for (GList* l = list; l; l = l->next) {
    SoupCookie* c = l->data;
    const char* value = soup_cookie_get_value(c);
    if (!g_session && !strcmp(soup_cookie_get_name(c), ".ROBLOSECURITY") && value && *value) {
      g_session = g_strdup(value);
      GDateTime* exp = soup_cookie_get_expires(c);
      g_session_expires = exp ? g_date_time_to_unix(exp) : 0;
      emit("log", "roblox.com signed you in; checking the session with Roblox");
      webkit_web_view_load_uri(g_view, kWhoAmI);
    }
  }
  g_list_free_full(list, (GDestroyNotify)soup_cookie_free);
}

static void login_check_cookies(void) {
  if (g_session) return;
  WebKitCookieManager* cm = webkit_web_context_get_cookie_manager(webkit_web_view_get_context(g_view));
  webkit_cookie_manager_get_cookies(cm, "https://www.roblox.com/", NULL, on_cookies, NULL);
}

static void on_cookies_changed(WebKitCookieManager* cm, gpointer d) {
  (void)cm, (void)d;
  login_check_cookies();
}

static void on_load(WebKitWebView* v, WebKitLoadEvent ev, gpointer d) {
  (void)d;
  if (ev == WEBKIT_LOAD_STARTED) g_loaded = FALSE;
  if (ev != WEBKIT_LOAD_FINISHED) return;
  if (g_login) {
    const char* uri = webkit_web_view_get_uri(v);
    if (g_session && uri && g_str_has_prefix(uri, kWhoAmI))
      webkit_web_view_evaluate_javascript(v, "document.body ? document.body.innerText : ''", -1, NULL, NULL, NULL, on_whoami, NULL);
    else
      login_check_cookies();
  }
  g_loaded = TRUE;
  emit("loaded", webkit_web_view_get_uri(v));
  for (guint i = 0; i < g_pending->len; ++i) run_script(g_ptr_array_index(g_pending, i));
  g_ptr_array_set_size(g_pending, 0);
}

// WebKitGTK reports console messages through the web inspector machinery; the
// simplest stable hook is the "console-message-sent" of WebKitWebPage, which
// lives in the web process. Instead, wrap console.* in the page (top frame)
// and post them over a second message handler.
static void on_console(WebKitUserContentManager* m, WebKitJavascriptResult* r, gpointer d) {
  (void)m, (void)d;
  char* text = jsc_value_to_string(webkit_javascript_result_get_js_value(r));
  if (strlen(text) > 300) text[300] = 0;
  emit("console", text);
  g_free(text);
}

// Keep everything inside this window: web pages load here; other link types
// (app schemes, intent://, mailto:), new windows and downloads are refused and
// reported, so nothing gets handed to the desktop's default browser.
static gboolean web_scheme(const char* uri) {
  return g_str_has_prefix(uri, "https:") || g_str_has_prefix(uri, "http:") || g_str_has_prefix(uri, "about:") ||
         g_str_has_prefix(uri, "data:") || g_str_has_prefix(uri, "blob:") ||  // (not file: your own files stay out)
         (test_files() && g_str_has_prefix(uri, "file:"));
}
static void report_blocked(const char* what, const char* uri) {
  char* scheme = g_uri_parse_scheme(uri);
  char msg[160];
  snprintf(msg, sizeof msg, "blocked %s to a %s: link", what, scheme ? scheme : "?");  // scheme only
  emit("log", msg);
  g_free(scheme);
}
static gboolean on_policy(WebKitWebView* v, WebKitPolicyDecision* d, WebKitPolicyDecisionType type, gpointer u) {
  (void)v, (void)u;
  if (type == WEBKIT_POLICY_DECISION_TYPE_RESPONSE) {
    WebKitResponsePolicyDecision* r = WEBKIT_RESPONSE_POLICY_DECISION(d);
    if (!webkit_response_policy_decision_is_mime_type_supported(r)) {
      emit("log", "blocked a download");
      webkit_policy_decision_ignore(d);
      return TRUE;
    }
    return FALSE;
  }
  WebKitNavigationAction* a = webkit_navigation_policy_decision_get_navigation_action(WEBKIT_NAVIGATION_POLICY_DECISION(d));
  const char* uri = webkit_uri_request_get_uri(webkit_navigation_action_get_request(a));
  if (type == WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION) {
    report_blocked("a new window", uri ? uri : "");
    webkit_policy_decision_ignore(d);
    return TRUE;
  }
  if (uri && !web_scheme(uri)) {
    report_blocked("navigation", uri);
    webkit_policy_decision_ignore(d);
    return TRUE;
  }
  return FALSE;
}

// The page's web process died (WebKit then shows the page again from the
// start). Say why, so it is not mistaken for the site reloading itself.
static void on_web_process_terminated(WebKitWebView* v, WebKitWebProcessTerminationReason why, gpointer u) {
  (void)v, (void)u;
  emit("log", why == WEBKIT_WEB_PROCESS_EXCEEDED_MEMORY_LIMIT ? "the page's web process ran out of memory"
              : why == WEBKIT_WEB_PROCESS_CRASHED             ? "the page's web process crashed"
                                                              : "the page's web process was stopped");
}

static gboolean on_permission(WebKitWebView* v, WebKitPermissionRequest* req, gpointer u) {
  (void)v, (void)u;
  emit("log", "refused a page's permission request (camera, microphone, location or notifications)");
  webkit_permission_request_deny(req);
  return TRUE;
}

static gboolean on_delete(GtkWidget* w, GdkEvent* e, gpointer d) {
  (void)w, (void)e, (void)d;
  emit("closed", NULL);
  gtk_main_quit();
  return FALSE;
}

static gboolean on_stdin(GIOChannel* ch, GIOCondition cond, gpointer d) {
  (void)d;
  if (cond & (G_IO_HUP | G_IO_ERR)) {
    gtk_main_quit();  // tonic went away
    return FALSE;
  }
  char* line = NULL;
  gsize len = 0;
  if (g_io_channel_read_line(ch, &line, &len, NULL, NULL) != G_IO_STATUS_NORMAL || !line) {
    gtk_main_quit();
    return FALSE;
  }
  g_strchomp(line);
  if (!strncmp(line, "eval ", 5)) {
    char* script = unescape(line + 5);
    if (g_loaded) {
      run_script(script);
      g_free(script);
    } else {
      g_ptr_array_add(g_pending, script);
    }
  } else if (!strcmp(line, "show")) {
    gtk_widget_show_all(g_window);
    gtk_window_present(GTK_WINDOW(g_window));
  } else if (!strcmp(line, "hide")) {
    gtk_widget_hide(g_window);
  } else if (!strncmp(line, "title ", 6)) {
    char* t = unescape(line + 6);
    gtk_window_set_title(GTK_WINDOW(g_window), t);
    g_free(t);
  } else if (!strcmp(line, "close")) {
    gtk_main_quit();
  }
  g_free(line);
  return TRUE;
}

// Roblox's Android WebView exposes window.__globalRobloxAndroidBridge__ with
// one method, executeRoblox(String). Here it posts the string to us.
static const char* kBridge =
    "window.__globalRobloxAndroidBridge__ = { executeRoblox: function (s) {"
    " window.webkit.messageHandlers.robloxBridge.postMessage(String(s)); } };"
    "(function () { var post = function (lvl, args) { try {"
    "  window.webkit.messageHandlers.tonicConsole.postMessage(lvl + ': ' +"
    "   Array.prototype.map.call(args, function (a) { try { return typeof a === 'string' ? a : JSON.stringify(a); }"
    "   catch (e) { return String(a); } }).join(' ')); } catch (e) {} };"
    " ['log', 'warn', 'error', 'info'].forEach(function (lvl) { var orig = console[lvl];"
    "  console[lvl] = function () { post(lvl, arguments); return orig.apply(console, arguments); }; });"
    " window.addEventListener('error', function (e) { post('uncaught', [e.message + ' @ ' + e.filename + ':' + e.lineno]); });"
    "})();";

int main(int argc, char** argv) {
  const char *url = NULL, *title = "Roblox", *ua = NULL;
  gboolean hidden = FALSE;
  int w = 480, h = 720;
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--url") && i + 1 < argc) url = argv[++i];
    else if (!strcmp(argv[i], "--title") && i + 1 < argc) title = argv[++i];
    else if (!strcmp(argv[i], "--ua") && i + 1 < argc) ua = argv[++i];
    else if (!strcmp(argv[i], "--hidden")) hidden = TRUE;
    else if (!strcmp(argv[i], "--login")) g_login = TRUE;
    else if (!strcmp(argv[i], "--size") && i + 1 < argc) sscanf(argv[++i], "%dx%d", &w, &h);
  }
  if (!url) {
    fprintf(stderr, "usage: %s --url URL [--title T] [--ua UA] [--hidden] [--size WxH]\n", argv[0]);
    return 2;
  }
  // WebKitGTK's DMA-BUF renderer makes some Wayland compositors drop the
  // connection ("Error 71 (Protocol error) dispatching to Wayland display")
  // as soon as the window is shown. Set it to 0 in the environment to try it.
  setenv("WEBKIT_DISABLE_DMABUF_RENDERER", "1", 0);
#if defined(__FreeBSD__) || defined(__OpenBSD__)
  // WebKitGTK's GPU compositing aborts the page process where there is no
  // usable OpenGL context (seen in VirtualBox: "Couldn't find current GLX or
  // EGL context"). Set it to 0 in the environment to try it.
  {
    const char* gpu = getenv("TONIC_WEBVIEW_GPU");
    if (!(gpu && *gpu == '1')) setenv("WEBKIT_DISABLE_COMPOSITING_MODE", "1", 0);
  }
#endif
  gtk_init(&argc, &argv);
  g_pending = g_ptr_array_new_with_free_func(g_free);

  WebKitUserContentManager* ucm = webkit_user_content_manager_new();
  g_signal_connect(ucm, "script-message-received::robloxBridge", G_CALLBACK(on_bridge), NULL);
  webkit_user_content_manager_register_script_message_handler(ucm, "robloxBridge");
  g_signal_connect(ucm, "script-message-received::tonicConsole", G_CALLBACK(on_console), NULL);
  webkit_user_content_manager_register_script_message_handler(ucm, "tonicConsole");
  if (!g_login) {  // (the sign-in window needs no bridge)
    WebKitUserScript* bridge = webkit_user_script_new(kBridge, WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
                                                      WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, NULL, NULL);
    webkit_user_content_manager_add_script(ucm, bridge);
    webkit_user_script_unref(bridge);
  }

  // Each tonic instance keeps its own web data (TONIC_WEBVIEW_DATA, set by
  // tonic): a page one account opened never shows another account's state.
  const char* data_dir = getenv("TONIC_WEBVIEW_DATA");
  if (data_dir && *data_dir) {
    g_mkdir_with_parents(data_dir, 0700);
    chmod(data_dir, 0700);  // the sign-in cookies are in here
    char* cache_dir = g_build_filename(data_dir, "cache", NULL);
    WebKitWebsiteDataManager* dm =
        webkit_website_data_manager_new("base-data-directory", data_dir, "base-cache-directory", cache_dir, NULL);
    WebKitWebContext* ctx = webkit_web_context_new_with_website_data_manager(dm);
    g_view = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "user-content-manager", ucm, "web-context", ctx, NULL));
    g_free(cache_dir);
  } else {
    g_view = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "user-content-manager", ucm, NULL));
  }
  WebKitSettings* st = webkit_web_view_get_settings(g_view);
  webkit_settings_set_enable_javascript(st, TRUE);
  if (ua && *ua) webkit_settings_set_user_agent(st, ua);
  webkit_settings_set_enable_developer_extras(st, FALSE);
  webkit_settings_set_javascript_can_access_clipboard(st, FALSE);
  webkit_settings_set_allow_file_access_from_file_urls(st, FALSE);
  webkit_settings_set_allow_universal_access_from_file_urls(st, FALSE);
#if defined(__FreeBSD__) || defined(__OpenBSD__)
  // No GPU drawing (and so no WebGL, which needs it): without a usable OpenGL
  // context WebKit's page process aborts, sometimes only once a page's script
  // asks for WebGL. TONIC_WEBVIEW_GPU=1 keeps both, for machines where GL works.
  const char* gpu = getenv("TONIC_WEBVIEW_GPU");
  if (!(gpu && *gpu == '1')) {
    webkit_settings_set_hardware_acceleration_policy(st, WEBKIT_HARDWARE_ACCELERATION_POLICY_NEVER);
    webkit_settings_set_enable_webgl(st, FALSE);
  }
#endif
  g_signal_connect(g_view, "load-changed", G_CALLBACK(on_load), NULL);
  if (g_login)
    g_signal_connect(webkit_web_context_get_cookie_manager(webkit_web_view_get_context(g_view)), "changed",
                     G_CALLBACK(on_cookies_changed), NULL);
  g_signal_connect(g_view, "decide-policy", G_CALLBACK(on_policy), NULL);
  g_signal_connect(g_view, "web-process-terminated", G_CALLBACK(on_web_process_terminated), NULL);
  g_signal_connect(g_view, "permission-request", G_CALLBACK(on_permission), NULL);

  g_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_window_set_title(GTK_WINDOW(g_window), title);
  gtk_window_set_icon_from_file(GTK_WINDOW(g_window), "assets/icon.png", NULL);  /* tonic's icon (started from tonic's folder) */
  gtk_window_set_default_size(GTK_WINDOW(g_window), w, h);
  gtk_container_add(GTK_CONTAINER(g_window), GTK_WIDGET(g_view));
  g_signal_connect(g_window, "delete-event", G_CALLBACK(on_delete), NULL);
  gtk_widget_realize(g_window);
  if (!hidden) gtk_widget_show_all(g_window);
  else gtk_widget_show(GTK_WIDGET(g_view));  // loads and runs scripts while the window stays hidden

  if (!g_login) {  // (the sign-in window takes no commands: it ends itself, or you close it)
    GIOChannel* in = g_io_channel_unix_new(0);
    g_io_add_watch(in, G_IO_IN | G_IO_HUP | G_IO_ERR, on_stdin, NULL);
  }

  emit("ready", NULL);
  webkit_web_view_load_uri(g_view, url);
  gtk_main();
  return 0;
}
