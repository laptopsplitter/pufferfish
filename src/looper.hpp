// looper.hpp - Android's ALooper (the NDK event loop every Android thread can
// have), on epoll. The library adds file descriptors to a thread's looper
// with a callback (ALooper_addFd) and expects that thread to wait on it
// (ALooper_pollOnce) and run the callbacks. Android's main thread does this
// all the time in Java's Looper; here main_loop does (mainloop.hpp).
#pragma once

namespace tonic {

void register_looper_shims();      // before relocation
void looper_prepare_main_thread(); // on the thread that becomes the main loop, before JNI_OnLoad

// Wait up to TIMEOUT_MS on the calling thread's looper and run the callbacks
// of descriptors that are ready. Returns at once when woken (looper_wake_main).
void looper_poll_main(int timeout_ms);
void looper_wake_main();

}  // namespace tonic
