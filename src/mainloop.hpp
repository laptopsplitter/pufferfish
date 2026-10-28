// mainloop.hpp - Android's UI thread: after the startup calls it keeps running
// what the library queues for it (nativeCallMessagesFromMainThread).
#pragma once

#include <functional>

#include "elfimg.hpp"

namespace tonic {

// Runs FN on the main thread, in order (queued until main_loop starts). Any thread.
void main_post(std::function<void()> fn);

// Becomes the main loop on the calling thread (the JNI_OnLoad thread). Never returns.
// TONIC_NO_MAIN_PUMP=1 skips nativeCallMessagesFromMainThread.
[[noreturn]] void main_loop(const ElfImage& img);

}  // namespace tonic
