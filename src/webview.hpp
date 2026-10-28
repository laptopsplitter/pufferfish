// webview.hpp - Android's WebView for the pages the app opens (captchas,
// help), via tonic-webview, wired up as the Java side does: the message-bus
// WebViewProtocol and the legacy BrowserService MemStorage keys. Page messages
// are logged by size only (they carry tokens).
#pragma once

#include "elfimg.hpp"

namespace tonic {

// Call before the library runs (registers the call listener).
void webview_init();

// Registers the handlers above. Call once the app has started, on an attached
// thread. Does nothing if the helper program is missing (says so).
void webview_enable(const ElfImage& img);

}  // namespace tonic
