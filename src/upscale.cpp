#include "upscale.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "../shaders/third_party/NIS_Config.h"
#include "config.hpp"
#include "upscale_spv.h"

namespace tonic {

namespace {

#define LOG(fmt, ...) fprintf(stderr, "[upscale] " fmt, ##__VA_ARGS__)

uint32_t bits(float f) {
  uint32_t u;
  memcpy(&u, &f, 4);
  return u;
}

VkFormat unorm_of(VkFormat f) {
  if (f == VK_FORMAT_R8G8B8A8_SRGB) return VK_FORMAT_R8G8B8A8_UNORM;
  if (f == VK_FORMAT_B8G8R8A8_SRGB) return VK_FORMAT_B8G8R8A8_UNORM;
  return f;
}
bool eight_bit(VkFormat f) {
  return f == VK_FORMAT_R8G8B8A8_UNORM || f == VK_FORMAT_R8G8B8A8_SRGB || f == VK_FORMAT_B8G8R8A8_UNORM ||
         f == VK_FORMAT_B8G8R8A8_SRGB;
}

}  // namespace

UpscaleMode upscale_mode() {
  const std::string m = config_get("upscaler", "off");
  return m == "fsr" ? UpscaleMode::FSR : m == "nis" ? UpscaleMode::NIS : UpscaleMode::Off;
}

const char* upscale_name(UpscaleMode m) { return m == UpscaleMode::FSR ? "FSR 1" : m == UpscaleMode::NIS ? "NIS" : "plain"; }

bool Upscaler::make_image(VkFormat f, VkExtent2D e, VkImageUsageFlags u, VkImage& im, VkDeviceMemory& m) {
  VkImageCreateInfo ii{};
  ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  ii.imageType = VK_IMAGE_TYPE_2D;
  ii.format = f;
  ii.extent = {e.width, e.height, 1};
  ii.mipLevels = ii.arrayLayers = 1;
  ii.samples = VK_SAMPLE_COUNT_1_BIT;
  ii.tiling = VK_IMAGE_TILING_OPTIMAL;
  ii.usage = u;
  ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (fn<PFN_vkCreateImage>("vkCreateImage")(s_.dev, &ii, nullptr, &im) != VK_SUCCESS) return false;
  VkMemoryRequirements req{};
  fn<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements")(s_.dev, im, &req);
  uint32_t type = ~0u;
  for (uint32_t t = 0; t < s_.mem.memoryTypeCount && type == ~0u; ++t)
    if ((req.memoryTypeBits & (1u << t)) && (s_.mem.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) type = t;
  if (type == ~0u) return false;
  VkMemoryAllocateInfo mi{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, req.size, type};
  return fn<PFN_vkAllocateMemory>("vkAllocateMemory")(s_.dev, &mi, nullptr, &m) == VK_SUCCESS &&
         fn<PFN_vkBindImageMemory>("vkBindImageMemory")(s_.dev, im, m, 0) == VK_SUCCESS;
}

VkImageView Upscaler::make_view(VkImage im, VkFormat f) {
  VkImageViewCreateInfo vi{};
  vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  vi.image = im;
  vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vi.format = f;
  vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VkImageView v = VK_NULL_HANDLE;
  fn<PFN_vkCreateImageView>("vkCreateImageView")(s_.dev, &vi, nullptr, &v);
  return v;
}

VkPipeline Upscaler::make_pipeline(const uint32_t* code, size_t bytes, bool swap_rb) {
  VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0, bytes, code};
  VkShaderModule mod = VK_NULL_HANDLE;
  if (fn<PFN_vkCreateShaderModule>("vkCreateShaderModule")(s_.dev, &mi, nullptr, &mod) != VK_SUCCESS) return VK_NULL_HANDLE;
  const VkBool32 swap = swap_rb ? VK_TRUE : VK_FALSE;
  VkSpecializationMapEntry me{0, 0, sizeof(VkBool32)};
  VkSpecializationInfo spec{1, &me, sizeof swap, &swap};
  VkComputePipelineCreateInfo pi{};
  pi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
  pi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  pi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  pi.stage.module = mod;
  pi.stage.pName = "main";
  pi.stage.pSpecializationInfo = &spec;
  pi.layout = layout_;
  VkPipeline p = VK_NULL_HANDLE;
  fn<PFN_vkCreateComputePipelines>("vkCreateComputePipelines")(s_.dev, VK_NULL_HANDLE, 1, &pi, nullptr, &p);
  fn<PFN_vkDestroyShaderModule>("vkDestroyShaderModule")(s_.dev, mod, nullptr);
  return p;
}

// NIS's filter tables, as small float textures (2 x 64 texels of 4).
bool Upscaler::upload_nis_tables() {
  const VkDeviceSize one = sizeof coef_scale;
  VkBufferCreateInfo bi{};
  bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  bi.size = one * 2;
  bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  VkBuffer staging = VK_NULL_HANDLE;
  VkDeviceMemory smem = VK_NULL_HANDLE;
  if (fn<PFN_vkCreateBuffer>("vkCreateBuffer")(s_.dev, &bi, nullptr, &staging) != VK_SUCCESS) return false;
  VkMemoryRequirements req{};
  fn<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(s_.dev, staging, &req);
  const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  uint32_t type = ~0u;
  for (uint32_t t = 0; t < s_.mem.memoryTypeCount && type == ~0u; ++t)
    if ((req.memoryTypeBits & (1u << t)) && (s_.mem.memoryTypes[t].propertyFlags & want) == want) type = t;
  bool ok = type != ~0u;
  VkMemoryAllocateInfo mi{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, req.size, type};
  ok = ok && fn<PFN_vkAllocateMemory>("vkAllocateMemory")(s_.dev, &mi, nullptr, &smem) == VK_SUCCESS &&
       fn<PFN_vkBindBufferMemory>("vkBindBufferMemory")(s_.dev, staging, smem, 0) == VK_SUCCESS;
  void* p = nullptr;
  ok = ok && fn<PFN_vkMapMemory>("vkMapMemory")(s_.dev, smem, 0, VK_WHOLE_SIZE, 0, &p) == VK_SUCCESS;
  if (ok) {
    memcpy(p, coef_scale, one);
    memcpy(static_cast<char*>(p) + one, coef_usm, one);
    fn<PFN_vkUnmapMemory>("vkUnmapMemory")(s_.dev, smem);
  }
  const VkExtent2D e{kFilterSize / 4, kPhaseCount};
  for (int i = 0; i < 2 && ok; ++i) {
    ok = make_image(VK_FORMAT_R32G32B32A32_SFLOAT, e, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, coef_[i],
                    coef_mem_[i]);
    if (ok) coef_view_[i] = make_view(coef_[i], VK_FORMAT_R32G32B32A32_SFLOAT);
  }
  // One command buffer, waited for: this runs once, when the game's swapchain is made.
  VkCommandPool pool = VK_NULL_HANDLE;
  VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr, VK_COMMAND_POOL_CREATE_TRANSIENT_BIT, s_.family};
  ok = ok && fn<PFN_vkCreateCommandPool>("vkCreateCommandPool")(s_.dev, &pci, nullptr, &pool) == VK_SUCCESS;
  VkCommandBuffer cb = VK_NULL_HANDLE;
  VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
  ok = ok && fn<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(s_.dev, &ai, &cb) == VK_SUCCESS;
  if (ok) {
    VkCommandBufferBeginInfo b{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, nullptr};
    fn<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(cb, &b);
    for (int i = 0; i < 2; ++i) {
      VkImageMemoryBarrier br{};
      br.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
      br.srcQueueFamilyIndex = br.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      br.image = coef_[i];
      br.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      br.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      br.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      br.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      fn<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                                                           nullptr, 0, nullptr, 1, &br);
      VkBufferImageCopy c{};
      c.bufferOffset = one * i;
      c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      c.imageExtent = {e.width, e.height, 1};
      fn<PFN_vkCmdCopyBufferToImage>("vkCmdCopyBufferToImage")(cb, staging, coef_[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
      br.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      br.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      br.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      br.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
      fn<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                                                           0, nullptr, 0, nullptr, 1, &br);
    }
    fn<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(cb);
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, 0};
    VkFence fence = VK_NULL_HANDLE;
    fn<PFN_vkCreateFence>("vkCreateFence")(s_.dev, &fi, nullptr, &fence);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    ok = fn<PFN_vkQueueSubmit>("vkQueueSubmit")(s_.queue, 1, &si, fence) == VK_SUCCESS &&
         fn<PFN_vkWaitForFences>("vkWaitForFences")(s_.dev, 1, &fence, VK_TRUE, 5000000000ull) == VK_SUCCESS;
    fn<PFN_vkDestroyFence>("vkDestroyFence")(s_.dev, fence, nullptr);
  }
  if (pool) fn<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(s_.dev, pool, nullptr);
  if (staging) fn<PFN_vkDestroyBuffer>("vkDestroyBuffer")(s_.dev, staging, nullptr);
  if (smem) fn<PFN_vkFreeMemory>("vkFreeMemory")(s_.dev, smem, nullptr);
  return ok;
}

bool Upscaler::init(const UpscaleSetup& s) {
  destroy();
  s_ = s;
  if (s_.mode == UpscaleMode::Off || s_.game.empty() || s_.game.size() != s_.swap.size()) return false;
  if (!eight_bit(s_.swap_format) || !eight_bit(s_.game_format)) {
    LOG("%s needs an 8-bit window format; plain scaling instead\n", upscale_name(s_.mode));
    return false;
  }
  // The result is copied into the window's image bit for bit, so its red and
  // blue are swapped in the last pass when the window is BGRA.
  swap_rb_ = s_.swap_format == VK_FORMAT_B8G8R8A8_UNORM || s_.swap_format == VK_FORMAT_B8G8R8A8_SRGB;
  const bool fsr = s_.mode == UpscaleMode::FSR;
  const float st = std::clamp(s_.strength, 0.f, 1.f);

  VkSamplerCreateInfo smp{};
  smp.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  smp.magFilter = smp.minFilter = VK_FILTER_LINEAR;
  smp.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  smp.addressModeU = smp.addressModeV = smp.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  smp.maxLod = 0;
  if (fn<PFN_vkCreateSampler>("vkCreateSampler")(s_.dev, &smp, nullptr, &sampler_) != VK_SUCCESS) return false;

  // Descriptor layouts, as the shaders declare them.
  std::vector<VkDescriptorSetLayoutBinding> b;
  if (fsr) {
    b = {{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
         {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
  } else {
    b = {{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
         {1, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
         {2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
         {3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
         {4, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
         {5, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
  }
  VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0,
                                     static_cast<uint32_t>(b.size()), b.data()};
  if (fn<PFN_vkCreateDescriptorSetLayout>("vkCreateDescriptorSetLayout")(s_.dev, &dl, nullptr, &dsl_) != VK_SUCCESS) return false;
  VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 64};
  VkPipelineLayoutCreateInfo pl{};
  pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pl.setLayoutCount = 1;
  pl.pSetLayouts = &dsl_;
  pl.pushConstantRangeCount = fsr ? 1 : 0;
  pl.pPushConstantRanges = &pcr;
  if (fn<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout")(s_.dev, &pl, nullptr, &layout_) != VK_SUCCESS) return false;
  if (fsr) {
    easu_ = make_pipeline(kSpv_fsr_easu, sizeof kSpv_fsr_easu, false);
    rcas_ = make_pipeline(kSpv_fsr_rcas, sizeof kSpv_fsr_rcas, swap_rb_);
    if (!easu_ || !rcas_) return false;
    // FsrEasuCon / FsrRcasCon (ffx_fsr1.h), here so no CPU half of the header is needed.
    const float iw = static_cast<float>(s_.in.width), ih = static_cast<float>(s_.in.height);
    const float ow = static_cast<float>(s_.out.width), oh = static_cast<float>(s_.out.height);
    const float c[16] = {iw / ow, ih / oh, 0.5f * iw / ow - 0.5f, 0.5f * ih / oh - 0.5f,
                         1.f / iw, 1.f / ih, 1.f / iw, -1.f / ih,
                         -1.f / iw, 2.f / ih, 1.f / iw, 2.f / ih,
                         0.f, 4.f / ih, 0.f, 0.f};
    for (int i = 0; i < 16; ++i) easu_con_[i] = bits(c[i]);
    easu_con_[14] = easu_con_[15] = 0;
    // Strength 1 = sharpest (0 stops), 0 = 2 stops less.
    rcas_con_[0] = bits(std::exp2(-(1.f - st) * 2.f));
  } else {
    nis_ = make_pipeline(kSpv_nis, sizeof kSpv_nis, swap_rb_);
    if (!nis_ || !upload_nis_tables()) return false;
    NISConfig cfg{};
    NVScalerUpdateConfig(cfg, st, 0, 0, s_.in.width, s_.in.height, s_.in.width, s_.in.height, 0, 0, s_.out.width, s_.out.height,
                         s_.out.width, s_.out.height, NISHDRMode::None);
    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = sizeof cfg;
    bi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    if (fn<PFN_vkCreateBuffer>("vkCreateBuffer")(s_.dev, &bi, nullptr, &ubo_) != VK_SUCCESS) return false;
    VkMemoryRequirements req{};
    fn<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(s_.dev, ubo_, &req);
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    uint32_t type = ~0u;
    for (uint32_t t = 0; t < s_.mem.memoryTypeCount && type == ~0u; ++t)
      if ((req.memoryTypeBits & (1u << t)) && (s_.mem.memoryTypes[t].propertyFlags & want) == want) type = t;
    VkMemoryAllocateInfo mi{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, req.size, type};
    void* p = nullptr;
    if (type == ~0u || fn<PFN_vkAllocateMemory>("vkAllocateMemory")(s_.dev, &mi, nullptr, &ubo_mem_) != VK_SUCCESS ||
        fn<PFN_vkBindBufferMemory>("vkBindBufferMemory")(s_.dev, ubo_, ubo_mem_, 0) != VK_SUCCESS ||
        fn<PFN_vkMapMemory>("vkMapMemory")(s_.dev, ubo_mem_, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS)
      return false;
    memcpy(p, &cfg, sizeof cfg);
    fn<PFN_vkUnmapMemory>("vkUnmapMemory")(s_.dev, ubo_mem_);
  }

  // Per image: the game's frame as a texture, the work images, the descriptor sets.
  const uint32_t n = static_cast<uint32_t>(s_.game.size());
  std::vector<VkDescriptorPoolSize> sizes = {
      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * n}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 * n},
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, n},             {VK_DESCRIPTOR_TYPE_SAMPLER, n},
      {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 3 * n}};
  VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr, 0, 2 * n,
                                static_cast<uint32_t>(sizes.size()), sizes.data()};
  if (fn<PFN_vkCreateDescriptorPool>("vkCreateDescriptorPool")(s_.dev, &dp, nullptr, &pool_) != VK_SUCCESS) return false;
  const VkImageUsageFlags work = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  auto update = fn<PFN_vkUpdateDescriptorSets>("vkUpdateDescriptorSets");
  img_.resize(n);
  for (uint32_t i = 0; i < n; ++i) {
    PerImage& im = img_[i];
    im.game_view = make_view(s_.game[i], unorm_of(s_.game_format));  // the stored values as they are
    if (!im.game_view || !make_image(VK_FORMAT_R8G8B8A8_UNORM, s_.out, work, im.fin, im.fin_mem)) return false;
    im.fin_view = make_view(im.fin, VK_FORMAT_R8G8B8A8_UNORM);
    VkDescriptorSet sets[2];
    VkDescriptorSetLayout lays[2] = {dsl_, dsl_};
    VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, pool_, fsr ? 2u : 1u, lays};
    if (fn<PFN_vkAllocateDescriptorSets>("vkAllocateDescriptorSets")(s_.dev, &da, sets) != VK_SUCCESS) return false;
    im.set_a = sets[0];
    if (fsr) {
      im.set_b = sets[1];
      if (!make_image(VK_FORMAT_R8G8B8A8_UNORM, s_.out, work, im.mid, im.mid_mem)) return false;
      im.mid_view = make_view(im.mid, VK_FORMAT_R8G8B8A8_UNORM);
      VkDescriptorImageInfo a_in{sampler_, im.game_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
      VkDescriptorImageInfo a_out{VK_NULL_HANDLE, im.mid_view, VK_IMAGE_LAYOUT_GENERAL};
      VkDescriptorImageInfo b_in{sampler_, im.mid_view, VK_IMAGE_LAYOUT_GENERAL};
      VkDescriptorImageInfo b_out{VK_NULL_HANDLE, im.fin_view, VK_IMAGE_LAYOUT_GENERAL};
      VkWriteDescriptorSet w[4]{};
      const VkDescriptorImageInfo* infos[4] = {&a_in, &a_out, &b_in, &b_out};
      for (int k = 0; k < 4; ++k) {
        w[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[k].dstSet = k < 2 ? im.set_a : im.set_b;
        w[k].dstBinding = k % 2;
        w[k].descriptorCount = 1;
        w[k].descriptorType = k % 2 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[k].pImageInfo = infos[k];
      }
      update(s_.dev, 4, w, 0, nullptr);
    } else {
      VkDescriptorBufferInfo ub{ubo_, 0, VK_WHOLE_SIZE};
      VkDescriptorImageInfo smpl{sampler_, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
      VkDescriptorImageInfo in{VK_NULL_HANDLE, im.game_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
      VkDescriptorImageInfo out{VK_NULL_HANDLE, im.fin_view, VK_IMAGE_LAYOUT_GENERAL};
      VkDescriptorImageInfo c0{VK_NULL_HANDLE, coef_view_[0], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
      VkDescriptorImageInfo c1{VK_NULL_HANDLE, coef_view_[1], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
      VkWriteDescriptorSet w[6]{};
      const VkDescriptorType types[6] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_SAMPLER,
                                         VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                         VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE};
      const VkDescriptorImageInfo* infos[6] = {nullptr, &smpl, &in, &out, &c0, &c1};
      for (int k = 0; k < 6; ++k) {
        w[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[k].dstSet = im.set_a;
        w[k].dstBinding = k;
        w[k].descriptorCount = 1;
        w[k].descriptorType = types[k];
        if (k == 0) w[k].pBufferInfo = &ub;
        else w[k].pImageInfo = infos[k];
      }
      update(s_.dev, 6, w, 0, nullptr);
    }
  }
  ready_ = true;
  LOG("%s: %ux%u -> %ux%u, sharpening %.0f %%\n", upscale_name(s_.mode), s_.in.width, s_.in.height, s_.out.width, s_.out.height,
      st * 100);
  return true;
}

void Upscaler::record(VkCommandBuffer cb, size_t i) {
  if (!ready_ || i >= img_.size()) return;
  PerImage& im = img_[i];
  auto pb = fn<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");
  auto bar = [&](VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags src, VkAccessFlags dst, VkPipelineStageFlags s1,
                 VkPipelineStageFlags s2) {
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    pb(cb, s1, s2, 0, 0, nullptr, 0, nullptr, 1, &b);
  };
  auto bind = fn<PFN_vkCmdBindPipeline>("vkCmdBindPipeline");
  auto sets = fn<PFN_vkCmdBindDescriptorSets>("vkCmdBindDescriptorSets");
  auto push = fn<PFN_vkCmdPushConstants>("vkCmdPushConstants");
  auto dispatch = fn<PFN_vkCmdDispatch>("vkCmdDispatch");
  const uint32_t W = s_.out.width, H = s_.out.height;
  const VkPipelineStageFlags CS = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
  bar(im.fin, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, CS);
  if (s_.mode == UpscaleMode::FSR) {
    bar(im.mid, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, CS);
    bind(cb, VK_PIPELINE_BIND_POINT_COMPUTE, easu_);
    sets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 1, &im.set_a, 0, nullptr);
    push(cb, layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof easu_con_, easu_con_);
    dispatch(cb, (W + 15) / 16, (H + 15) / 16, 1);
    bar(im.mid, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, CS, CS);
    bind(cb, VK_PIPELINE_BIND_POINT_COMPUTE, rcas_);
    sets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 1, &im.set_b, 0, nullptr);
    push(cb, layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof rcas_con_, rcas_con_);
    dispatch(cb, (W + 15) / 16, (H + 15) / 16, 1);
  } else {
    bind(cb, VK_PIPELINE_BIND_POINT_COMPUTE, nis_);
    sets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 1, &im.set_a, 0, nullptr);
    dispatch(cb, (W + 31) / 32, (H + 23) / 24, 1);
  }
  // Into the window's image, bit for bit (red/blue already in its order).
  bar(im.fin, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, CS,
      VK_PIPELINE_STAGE_TRANSFER_BIT);
  VkImageCopy c{};
  c.srcSubresource = c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  c.extent = {W, H, 1};
  fn<PFN_vkCmdCopyImage>("vkCmdCopyImage")(cb, im.fin, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s_.swap[i],
                                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
}

void Upscaler::destroy() {
  if (!s_.dev || !s_.gdpa) return;
  auto dv = s_.dev;
  auto view = fn<PFN_vkDestroyImageView>("vkDestroyImageView");
  auto image = fn<PFN_vkDestroyImage>("vkDestroyImage");
  auto mem = fn<PFN_vkFreeMemory>("vkFreeMemory");
  for (PerImage& im : img_) {
    if (im.game_view) view(dv, im.game_view, nullptr);
    if (im.mid_view) view(dv, im.mid_view, nullptr);
    if (im.fin_view) view(dv, im.fin_view, nullptr);
    if (im.mid) image(dv, im.mid, nullptr);
    if (im.fin) image(dv, im.fin, nullptr);
    if (im.mid_mem) mem(dv, im.mid_mem, nullptr);
    if (im.fin_mem) mem(dv, im.fin_mem, nullptr);
  }
  img_.clear();
  for (int i = 0; i < 2; ++i) {
    if (coef_view_[i]) view(dv, coef_view_[i], nullptr);
    if (coef_[i]) image(dv, coef_[i], nullptr);
    if (coef_mem_[i]) mem(dv, coef_mem_[i], nullptr);
    coef_view_[i] = VK_NULL_HANDLE, coef_[i] = VK_NULL_HANDLE, coef_mem_[i] = VK_NULL_HANDLE;
  }
  if (ubo_) fn<PFN_vkDestroyBuffer>("vkDestroyBuffer")(dv, ubo_, nullptr);
  if (ubo_mem_) mem(dv, ubo_mem_, nullptr);
  if (pool_) fn<PFN_vkDestroyDescriptorPool>("vkDestroyDescriptorPool")(dv, pool_, nullptr);
  for (VkPipeline p : {easu_, rcas_, nis_})
    if (p) fn<PFN_vkDestroyPipeline>("vkDestroyPipeline")(dv, p, nullptr);
  if (layout_) fn<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout")(dv, layout_, nullptr);
  if (dsl_) fn<PFN_vkDestroyDescriptorSetLayout>("vkDestroyDescriptorSetLayout")(dv, dsl_, nullptr);
  if (sampler_) fn<PFN_vkDestroySampler>("vkDestroySampler")(dv, sampler_, nullptr);
  ubo_ = VK_NULL_HANDLE, ubo_mem_ = VK_NULL_HANDLE, pool_ = VK_NULL_HANDLE;
  easu_ = rcas_ = nis_ = VK_NULL_HANDLE;
  layout_ = VK_NULL_HANDLE, dsl_ = VK_NULL_HANDLE, sampler_ = VK_NULL_HANDLE;
  ready_ = false;
  s_ = UpscaleSetup{};
}

}  // namespace tonic
