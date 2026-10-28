#pragma once
// Upscalers for the Vulkan resolution cap: FSR 1 (EASU + RCAS) or NIS, as a
// compute pass on the finished frame. Shaders in shaders/third_party (MIT).
#include <vulkan/vulkan.h>

#include <string>
#include <vector>

namespace tonic {

enum class UpscaleMode { Off, FSR, NIS };
UpscaleMode upscale_mode();  // from the settings
const char* upscale_name(UpscaleMode m);

struct UpscaleSetup {
  VkDevice dev = VK_NULL_HANDLE;
  PFN_vkGetDeviceProcAddr gdpa = nullptr;
  VkPhysicalDeviceMemoryProperties mem{};
  VkQueue queue = VK_NULL_HANDLE;  // for the one-time upload (NIS's tables)
  uint32_t family = 0;
  UpscaleMode mode = UpscaleMode::Off;
  float strength = 0.5f;  // 0..1 (sharpening)
  VkExtent2D in{}, out{};
  VkFormat game_format = VK_FORMAT_UNDEFINED;  // viewed as UNORM when sRGB (made mutable for it)
  std::vector<VkImage> game;                   // sampled
  VkFormat swap_format = VK_FORMAT_UNDEFINED;  // 8-bit RGBA/BGRA (UNORM or sRGB)
  std::vector<VkImage> swap;                   // the result is copied in (transfer dst)
};

class Upscaler {
 public:
  bool init(const UpscaleSetup& s);  // false: not here (the plain stretch is used)
  // Into CB: GAME[i] in SHADER_READ_ONLY_OPTIMAL -> passes -> copied into
  // SWAP[i] (in TRANSFER_DST_OPTIMAL). Barriers between the passes included.
  void record(VkCommandBuffer cb, size_t i);
  void destroy();
  bool ready() const { return ready_; }
  ~Upscaler() { destroy(); }

 private:
  template <typename T>
  T fn(const char* name) const { return reinterpret_cast<T>(s_.gdpa(s_.dev, name)); }
  bool make_image(VkFormat f, VkExtent2D e, VkImageUsageFlags u, VkImage& im, VkDeviceMemory& m);
  VkImageView make_view(VkImage im, VkFormat f);
  VkPipeline make_pipeline(const uint32_t* code, size_t bytes, bool swap_rb);
  bool upload_nis_tables();

  UpscaleSetup s_;
  bool ready_ = false;
  bool swap_rb_ = false;
  VkSampler sampler_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout dsl_ = VK_NULL_HANDLE;
  VkPipelineLayout layout_ = VK_NULL_HANDLE;
  VkPipeline easu_ = VK_NULL_HANDLE, rcas_ = VK_NULL_HANDLE, nis_ = VK_NULL_HANDLE;
  VkDescriptorPool pool_ = VK_NULL_HANDLE;
  uint32_t easu_con_[16] = {}, rcas_con_[4] = {};
  // NIS: settings buffer, coefficient tables
  VkBuffer ubo_ = VK_NULL_HANDLE;
  VkDeviceMemory ubo_mem_ = VK_NULL_HANDLE;
  VkImage coef_[2] = {};
  VkDeviceMemory coef_mem_[2] = {};
  VkImageView coef_view_[2] = {};
  struct PerImage {
    VkImageView game_view = VK_NULL_HANDLE;
    VkImage mid = VK_NULL_HANDLE, fin = VK_NULL_HANDLE;  // FSR: EASU -> mid -> RCAS -> fin; NIS -> fin
    VkDeviceMemory mid_mem = VK_NULL_HANDLE, fin_mem = VK_NULL_HANDLE;
    VkImageView mid_view = VK_NULL_HANDLE, fin_view = VK_NULL_HANDLE;
    VkDescriptorSet set_a = VK_NULL_HANDLE, set_b = VK_NULL_HANDLE;
  };
  std::vector<PerImage> img_;
};

}  // namespace tonic
