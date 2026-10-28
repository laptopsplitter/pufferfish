#pragma once
// NVIDIA Smooth Motion: the NVIDIA driver's own frame generation (575 and
// newer, GeForce RTX 40 series and newer, Vulkan only). It is the driver's
// Vulkan layer VK_LAYER_NV_present, switched on with
// NVPRESENT_ENABLE_SMOOTH_MOTION=1; it adds frames after Roblox's, so Roblox
// (and its FPS counters) still count the real ones. Config: smooth_motion.
#include <string>

namespace tonic {

struct SmoothMotionSupport {
  bool ok = false;
  std::string why;  // when not: what is missing; when ok: the GPU
};
SmoothMotionSupport smooth_motion_support();  // looked up once
// At Play, before Roblox's Vulkan starts: the setting into the environment.
void smooth_motion_apply();
bool smooth_motion_active();  // on for this run (the stats overlay says so)

}  // namespace tonic
