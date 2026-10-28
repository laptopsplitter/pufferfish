#include "vkshim.hpp"
#include "upscale.hpp"

#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vulkan/vulkan_core.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "config.hpp"
#include "profile.hpp"
#include "shims.hpp"
#include "textoverlay.hpp"
#include "window.hpp"

namespace tonic {

namespace {

#define LOG(fmt, ...) fprintf(stderr, "[vulkan] " fmt, ##__VA_ARGS__)
using Clock = std::chrono::steady_clock;

// The Android and desktop surface structures, declared here so no window
// system headers are needed.
constexpr VkStructureType kWaylandSurfaceInfo = static_cast<VkStructureType>(1000006000);
constexpr VkStructureType kXlibSurfaceInfo = static_cast<VkStructureType>(1000004000);
struct AndroidSurfaceInfo {
  VkStructureType sType;
  const void* pNext;
  VkFlags flags;
  void* window;
};
struct WaylandSurfaceInfo {
  VkStructureType sType;
  const void* pNext;
  VkFlags flags;
  void* display;
  void* surface;
};
struct XlibSurfaceInfo {
  VkStructureType sType;
  const void* pNext;
  VkFlags flags;
  void* dpy;
  unsigned long window;
};
using PFN_CreatePlatformSurface = VkResult (*)(VkInstance, const void*, const VkAllocationCallbacks*, VkSurfaceKHR*);

// --- the decision: Vulkan this run, or OpenGL ---------------------------------------

std::atomic<bool> g_enabled{false};  // Vulkan offered to Roblox this run
std::atomic<bool> g_presented{false};
Clock::time_point g_started = Clock::now();
std::string g_marker;  // exists while a Vulkan start has not shown a frame yet

// --- the host's Vulkan loader -------------------------------------------------------------

void* g_host = nullptr;
PFN_vkGetInstanceProcAddr h_gipa = nullptr;
int g_tag;  // our "libvulkan.so" handle is &g_tag
bool g_wayland = false;
std::string g_platform_ext;

VkInstance g_instance = VK_NULL_HANDLE;
template <typename T>
T ifn(const char* name) {
  return reinterpret_cast<T>(h_gipa(g_instance, name));
}

std::vector<VkExtensionProperties> host_instance_exts() {
  auto e = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(h_gipa(nullptr, "vkEnumerateInstanceExtensionProperties"));
  uint32_t n = 0;
  if (!e || e(nullptr, &n, nullptr) != VK_SUCCESS) return {};
  std::vector<VkExtensionProperties> v(n);
  e(nullptr, &n, v.data());
  v.resize(n);
  return v;
}
bool has_ext(const std::vector<VkExtensionProperties>& v, const char* name) {
  return std::any_of(v.begin(), v.end(), [name](const VkExtensionProperties& p) { return !strcmp(p.extensionName, name); });
}

// --- devices, queues, swapchains ------------------------------------------------------------

struct Device {
  VkDevice dev = VK_NULL_HANDLE;
  VkPhysicalDevice phys = VK_NULL_HANDLE;
  PFN_vkGetDeviceProcAddr gdpa = nullptr;
  template <typename T>
  T fn(const char* name) const {
    return reinterpret_cast<T>(gdpa(dev, name));
  }
  // The overlay's things, on the presenting queue's family.
  uint32_t family = ~0u;
  VkCommandPool pool = VK_NULL_HANDLE;
  struct Slot {
    VkCommandBuffer cmd[2] = {};
    VkFence fence = VK_NULL_HANDLE;
    bool pending = false;  // the fence has a submit to wait for
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    uint8_t* mapped = nullptr;
    VkDeviceSize size = 0;
  };
  // Two frames' overlays in flight: the next frame's are drawn while the last
  // copy still runs on the GPU (one buffer made each frame wait for the last).
  Slot slot[2];
  int cur = 0;
};
struct Swapchain {
  VkDevice dev;
  VkFormat format;
  VkExtent2D extent;
  std::vector<VkImage> images;
  std::vector<VkSemaphore> done;  // per image: the overlay finished
  bool copyable = false;          // made with transfer usage and an 8-bit RGBA/BGRA format
  bool bgra = false;
  // Resolution cap: the game draws into images of its own at the smaller
  // size (GAME), each scaled up into the window's image when presented.
  bool virt = false;
  VkExtent2D game_extent{};
  std::vector<VkImage> game;
  std::vector<VkDeviceMemory> game_mem;
  uint32_t blit_family = ~0u;
  VkCommandPool blit_pool = VK_NULL_HANDLE;
  std::vector<VkCommandBuffer> blit_cmd;  // one per image, recorded once
  std::vector<VkSemaphore> blitted;       // per image: scaled up
  std::shared_ptr<Upscaler> up;           // FSR 1 / NIS instead of a plain stretch (upscale.hpp)
};
std::mutex g_mu;
std::map<VkDevice, std::shared_ptr<Device>> g_devices;
std::map<VkQueue, std::pair<VkDevice, uint32_t>> g_queues;
std::map<VkSwapchainKHR, Swapchain> g_swapchains;

// --- wrapped calls ----------------------------------------------------------------------------

VkResult VKAPI_CALL my_EnumerateInstanceExtensionProperties(const char* layer, uint32_t* count, VkExtensionProperties* props) {
  auto host = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(h_gipa(nullptr, "vkEnumerateInstanceExtensionProperties"));
  if (layer) return host(layer, count, props);
  std::vector<VkExtensionProperties> v = host_instance_exts();
  if (has_ext(v, g_platform_ext.c_str())) {  // Android's surface, made on the desktop's
    VkExtensionProperties a{};
    strcpy(a.extensionName, "VK_KHR_android_surface");
    a.specVersion = 6;
    v.push_back(a);
  }
  if (!props) {
    *count = static_cast<uint32_t>(v.size());
    return VK_SUCCESS;
  }
  const uint32_t n = std::min<uint32_t>(*count, static_cast<uint32_t>(v.size()));
  std::copy(v.begin(), v.begin() + n, props);
  *count = n;
  return n < v.size() ? VK_INCOMPLETE : VK_SUCCESS;
}

VkResult VKAPI_CALL my_CreateInstance(const VkInstanceCreateInfo* ci, const VkAllocationCallbacks* alloc, VkInstance* out) {
  auto host = reinterpret_cast<PFN_vkCreateInstance>(h_gipa(nullptr, "vkCreateInstance"));
  const std::vector<VkExtensionProperties> have = host_instance_exts();
  std::vector<const char*> exts;
  std::string dropped;
  auto add = [&](const char* e) {
    for (const char* x : exts)
      if (!strcmp(x, e)) return;
    exts.push_back(e);
  };
  for (uint32_t i = 0; i < ci->enabledExtensionCount; ++i) {
    const char* e = ci->ppEnabledExtensionNames[i];
    if (!strcmp(e, "VK_KHR_android_surface")) continue;
    if (has_ext(have, e)) add(e);
    else dropped += std::string(dropped.empty() ? "" : ", ") + e;
  }
  if (!has_ext(have, "VK_KHR_surface") || !has_ext(have, g_platform_ext.c_str())) {
    LOG("this Vulkan driver cannot draw to the window (%s missing): Roblox uses OpenGL\n", g_platform_ext.c_str());
    return VK_ERROR_EXTENSION_NOT_PRESENT;
  }
  add("VK_KHR_surface");
  add(g_platform_ext.c_str());
  // Layers that are not installed here (Android's validation layers) are left out.
  auto el = reinterpret_cast<PFN_vkEnumerateInstanceLayerProperties>(h_gipa(nullptr, "vkEnumerateInstanceLayerProperties"));
  std::vector<VkLayerProperties> layers;
  uint32_t nl = 0;
  if (el && el(&nl, nullptr) == VK_SUCCESS) {
    layers.resize(nl);
    el(&nl, layers.data());
  }
  std::vector<const char*> keep_layers;
  for (uint32_t i = 0; i < ci->enabledLayerCount; ++i)
    for (auto& l : layers)
      if (!strcmp(l.layerName, ci->ppEnabledLayerNames[i])) keep_layers.push_back(ci->ppEnabledLayerNames[i]);
  VkInstanceCreateInfo c = *ci;
  c.enabledExtensionCount = static_cast<uint32_t>(exts.size());
  c.ppEnabledExtensionNames = exts.data();
  c.enabledLayerCount = static_cast<uint32_t>(keep_layers.size());
  c.ppEnabledLayerNames = keep_layers.data();
  const VkResult r = host(&c, alloc, out);
  if (r == VK_SUCCESS) {
    g_instance = *out;
    LOG("instance made (%s for the window%s%s)\n", g_platform_ext.c_str(), dropped.empty() ? "" : "; left out: ",
        dropped.c_str());
  } else {
    LOG("vkCreateInstance failed (%d): Roblox should fall back to OpenGL\n", r);
  }
  return r;
}

VkResult VKAPI_CALL my_CreateAndroidSurfaceKHR(VkInstance inst, const AndroidSurfaceInfo* info, const VkAllocationCallbacks* alloc,
                                               VkSurfaceKHR* out) {
  (void)info;  // the ANativeWindow is always ours: the game window
  bool wayland;
  void *display, *surface;
  window_vk_target(wayland, display, surface);
  VkResult r;
  if (wayland) {
    WaylandSurfaceInfo w{kWaylandSurfaceInfo, nullptr, 0, display, surface};
    auto f = reinterpret_cast<PFN_CreatePlatformSurface>(h_gipa(inst, "vkCreateWaylandSurfaceKHR"));
    r = f ? f(inst, &w, alloc, out) : VK_ERROR_EXTENSION_NOT_PRESENT;
  } else {
    XlibSurfaceInfo x{kXlibSurfaceInfo, nullptr, 0, display, static_cast<unsigned long>(reinterpret_cast<uintptr_t>(surface))};
    auto f = reinterpret_cast<PFN_CreatePlatformSurface>(h_gipa(inst, "vkCreateXlibSurfaceKHR"));
    r = f ? f(inst, &x, alloc, out) : VK_ERROR_EXTENSION_NOT_PRESENT;
  }
  if (r == VK_SUCCESS) {
    window_set_vulkan();
    LOG("surface made on the %s window\n", wayland ? "Wayland" : "X11");
  } else {
    LOG("surface failed (%d)\n", r);
  }
  return r;
}

// Wayland leaves the size to the app (0xFFFFFFFF); Android code expects the
// window's size there. With a resolution cap the game is told the smaller
// size (it draws that; tonic scales it up).
void fix_caps(VkSurfaceCapabilitiesKHR& c) {
  int w, h, rw, rh;
  window_size(w, h);
  window_render_size(rw, rh);
  if ((rw != w || rh != h) && rw > 0 && rh > 0) {
    c.currentExtent = {static_cast<uint32_t>(rw), static_cast<uint32_t>(rh)};
    c.minImageExtent = {1, 1};
    c.maxImageExtent.width = std::max<uint32_t>(c.maxImageExtent.width, rw);
    c.maxImageExtent.height = std::max<uint32_t>(c.maxImageExtent.height, rh);
    return;
  }
  if (c.currentExtent.width == 0xFFFFFFFFu) {
    int w, h;
    window_size(w, h);
    c.currentExtent.width = std::clamp<uint32_t>(static_cast<uint32_t>(std::max(1, w)), c.minImageExtent.width, c.maxImageExtent.width);
    c.currentExtent.height = std::clamp<uint32_t>(static_cast<uint32_t>(std::max(1, h)), c.minImageExtent.height, c.maxImageExtent.height);
  }
}
VkResult VKAPI_CALL my_GetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice p, VkSurfaceKHR s, VkSurfaceCapabilitiesKHR* c) {
  static auto host = ifn<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>("vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
  const VkResult r = host(p, s, c);
  if (r == VK_SUCCESS) fix_caps(*c);
  return r;
}
VkResult VKAPI_CALL my_GetPhysicalDeviceSurfaceCapabilities2KHR(VkPhysicalDevice p, const VkPhysicalDeviceSurfaceInfo2KHR* i,
                                                                VkSurfaceCapabilities2KHR* c) {
  static auto host = ifn<PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR>("vkGetPhysicalDeviceSurfaceCapabilities2KHR");
  const VkResult r = host(p, i, c);
  if (r == VK_SUCCESS) fix_caps(c->surfaceCapabilities);
  return r;
}

VkResult VKAPI_CALL my_CreateDevice(VkPhysicalDevice phys, const VkDeviceCreateInfo* ci, const VkAllocationCallbacks* alloc,
                                    VkDevice* out) {
  static auto host = ifn<PFN_vkCreateDevice>("vkCreateDevice");
  static auto enumerate = ifn<PFN_vkEnumerateDeviceExtensionProperties>("vkEnumerateDeviceExtensionProperties");
  uint32_t n = 0;
  enumerate(phys, nullptr, &n, nullptr);
  std::vector<VkExtensionProperties> have(n);
  enumerate(phys, nullptr, &n, have.data());
  // Android-only extensions (hardware buffers and the like) are left out.
  std::vector<const char*> exts;
  std::string dropped;
  for (uint32_t i = 0; i < ci->enabledExtensionCount; ++i) {
    const char* e = ci->ppEnabledExtensionNames[i];
    if (has_ext(have, e)) exts.push_back(e);
    else dropped += std::string(dropped.empty() ? "" : ", ") + e;
  }
  VkDeviceCreateInfo c = *ci;
  c.enabledExtensionCount = static_cast<uint32_t>(exts.size());
  c.ppEnabledExtensionNames = exts.data();
  const VkResult r = host(phys, &c, alloc, out);
  if (r == VK_SUCCESS) {
    auto d = std::make_shared<Device>();
    d->dev = *out;
    d->phys = phys;
    d->gdpa = ifn<PFN_vkGetDeviceProcAddr>("vkGetDeviceProcAddr");
    std::lock_guard<std::mutex> l(g_mu);
    g_devices[*out] = d;
    VkPhysicalDeviceProperties pp{};
    ifn<PFN_vkGetPhysicalDeviceProperties>("vkGetPhysicalDeviceProperties")(phys, &pp);
    LOG("device: %s%s%s\n", pp.deviceName, dropped.empty() ? "" : "; left out: ", dropped.c_str());
  } else {
    LOG("vkCreateDevice failed (%d)\n", r);
  }
  return r;
}

void VKAPI_CALL my_GetDeviceQueue(VkDevice d, uint32_t family, uint32_t index, VkQueue* q) {
  std::shared_ptr<Device> dev;
  {
    std::lock_guard<std::mutex> l(g_mu);
    auto it = g_devices.find(d);
    if (it != g_devices.end()) dev = it->second;
  }
  auto host = dev ? dev->fn<PFN_vkGetDeviceQueue>("vkGetDeviceQueue") : ifn<PFN_vkGetDeviceQueue>("vkGetDeviceQueue");
  host(d, family, index, q);
  std::lock_guard<std::mutex> l(g_mu);
  if (*q) g_queues[*q] = {d, family};
}
void VKAPI_CALL my_GetDeviceQueue2(VkDevice d, const VkDeviceQueueInfo2* info, VkQueue* q) {
  std::shared_ptr<Device> dev;
  {
    std::lock_guard<std::mutex> l(g_mu);
    auto it = g_devices.find(d);
    if (it != g_devices.end()) dev = it->second;
  }
  auto host = dev ? dev->fn<PFN_vkGetDeviceQueue2>("vkGetDeviceQueue2") : ifn<PFN_vkGetDeviceQueue2>("vkGetDeviceQueue2");
  host(d, info, q);
  std::lock_guard<std::mutex> l(g_mu);
  if (*q) g_queues[*q] = {d, info->queueFamilyIndex};
}

void free_overlay(Device& d) {
  for (Device::Slot& s : d.slot) {
    if (s.pending) d.fn<PFN_vkWaitForFences>("vkWaitForFences")(d.dev, 1, &s.fence, VK_TRUE, UINT64_MAX);
    if (s.buf) d.fn<PFN_vkDestroyBuffer>("vkDestroyBuffer")(d.dev, s.buf, nullptr);
    if (s.mem) d.fn<PFN_vkFreeMemory>("vkFreeMemory")(d.dev, s.mem, nullptr);
    if (s.fence) d.fn<PFN_vkDestroyFence>("vkDestroyFence")(d.dev, s.fence, nullptr);
    s = Device::Slot{};
  }
  if (d.pool) d.fn<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(d.dev, d.pool, nullptr);  // (frees the command buffers)
  d.pool = VK_NULL_HANDLE, d.family = ~0u;
}

void VKAPI_CALL my_DestroyDevice(VkDevice dv, const VkAllocationCallbacks* alloc) {
  std::shared_ptr<Device> d;
  {
    std::lock_guard<std::mutex> l(g_mu);
    auto it = g_devices.find(dv);
    if (it != g_devices.end()) d = it->second, g_devices.erase(it);
    for (auto q = g_queues.begin(); q != g_queues.end();) q = q->second.first == dv ? g_queues.erase(q) : std::next(q);
  }
  auto host = d ? d->fn<PFN_vkDestroyDevice>("vkDestroyDevice") : ifn<PFN_vkDestroyDevice>("vkDestroyDevice");
  if (d) free_overlay(*d);
  host(dv, alloc);
}

VkResult VKAPI_CALL my_CreateSwapchainKHR(VkDevice dv, const VkSwapchainCreateInfoKHR* ci, const VkAllocationCallbacks* alloc,
                                          VkSwapchainKHR* out) {
  std::shared_ptr<Device> d;
  {
    std::lock_guard<std::mutex> l(g_mu);
    auto it = g_devices.find(dv);
    if (it != g_devices.end()) d = it->second;
  }
  if (!d) return VK_ERROR_INITIALIZATION_FAILED;
  VkSwapchainCreateInfoKHR c = *ci;
  VkSurfaceCapabilitiesKHR caps{};
  ifn<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>("vkGetPhysicalDeviceSurfaceCapabilitiesKHR")(d->phys, ci->surface, &caps);
  // The window's own size (not the capped one the game was told).
  int ww, wh;
  window_size(ww, wh);
  if (caps.currentExtent.width == 0xFFFFFFFFu)
    caps.currentExtent = {std::clamp<uint32_t>(std::max(1, ww), caps.minImageExtent.width, caps.maxImageExtent.width),
                          std::clamp<uint32_t>(std::max(1, wh), caps.minImageExtent.height, caps.maxImageExtent.height)};
  const VkExtent2D game_extent = ci->imageExtent;
  const VkExtent2D target = caps.currentExtent;
  // Resolution cap: the game asked for less than the window. Its frames are
  // scaled up (vkCmdBlitImage) into a swapchain of the window's size.
  bool virt = game_extent.width < target.width || game_extent.height < target.height;
  if (virt) {
    VkFormatProperties fp{};
    ifn<PFN_vkGetPhysicalDeviceFormatProperties>("vkGetPhysicalDeviceFormatProperties")(d->phys, ci->imageFormat, &fp);
    const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
    if ((fp.optimalTilingFeatures & need) != need || !(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
      LOG("resolution cap: this format cannot be scaled here; the game draws the whole window\n");
      virt = false;
      window_cap_unsupported();
    }
    c.imageExtent = target;
  }
  // What a desktop surface supports, where Android's choices differ.
  // Opaque whenever the desktop allows it: Android's choice (often "inherit")
  // can let the desktop show through where the game leaves alpha below 1.
  if (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) c.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  if (!(caps.supportedCompositeAlpha & c.compositeAlpha))
    c.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
                           ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                           : static_cast<VkCompositeAlphaFlagBitsKHR>(caps.supportedCompositeAlpha & -caps.supportedCompositeAlpha);
  if (!(caps.supportedTransforms & c.preTransform)) c.preTransform = caps.currentTransform;
  c.minImageCount = std::max(c.minImageCount, caps.minImageCount);
  if (caps.maxImageCount) c.minImageCount = std::min(c.minImageCount, caps.maxImageCount);
  c.imageExtent.width = std::clamp(c.imageExtent.width, caps.minImageExtent.width, caps.maxImageExtent.width);
  c.imageExtent.height = std::clamp(c.imageExtent.height, caps.minImageExtent.height, caps.maxImageExtent.height);
  const VkImageUsageFlags copy = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  const bool can_copy = (caps.supportedUsageFlags & copy) == copy;
  if (can_copy) c.imageUsage |= copy;  // for tonic's overlays and screenshots
  // Present mode: the VSync setting (off: no waiting, as with OpenGL).
  uint32_t n = 0;
  auto modes_fn = ifn<PFN_vkGetPhysicalDeviceSurfacePresentModesKHR>("vkGetPhysicalDeviceSurfacePresentModesKHR");
  modes_fn(d->phys, ci->surface, &n, nullptr);
  std::vector<VkPresentModeKHR> modes(n);
  modes_fn(d->phys, ci->surface, &n, modes.data());
  auto has = [&](VkPresentModeKHR m) { return std::find(modes.begin(), modes.end(), m) != modes.end(); };
  if (window_vsync()) c.presentMode = VK_PRESENT_MODE_FIFO_KHR;
  else if (has(VK_PRESENT_MODE_MAILBOX_KHR)) c.presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
  else if (has(VK_PRESENT_MODE_IMMEDIATE_KHR)) c.presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
  else c.presentMode = VK_PRESENT_MODE_FIFO_KHR;
  const VkResult r = d->fn<PFN_vkCreateSwapchainKHR>("vkCreateSwapchainKHR")(dv, &c, alloc, out);
  if (r != VK_SUCCESS) {
    LOG("swapchain failed (%d)\n", r);
    return r;
  }
  Swapchain sc;
  sc.dev = dv, sc.format = c.imageFormat, sc.extent = c.imageExtent;
  uint32_t ni = 0;
  auto get_images = d->fn<PFN_vkGetSwapchainImagesKHR>("vkGetSwapchainImagesKHR");
  get_images(dv, *out, &ni, nullptr);
  sc.images.resize(ni);
  get_images(dv, *out, &ni, sc.images.data());
  if (virt) {  // the game's images at its size, in the GPU's own memory
    sc.virt = true;
    sc.game_extent = game_extent;
    VkImageCreateInfo ii{};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    if (ci->flags & VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR)
      ii.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
    // The upscalers read an sRGB frame through a UNORM view (the stored values as they are).
    const VkFormat srgb_unorm = ci->imageFormat == VK_FORMAT_B8G8R8A8_SRGB   ? VK_FORMAT_B8G8R8A8_UNORM
                                : ci->imageFormat == VK_FORMAT_R8G8B8A8_SRGB ? VK_FORMAT_R8G8B8A8_UNORM
                                                                             : VK_FORMAT_UNDEFINED;
    if (srgb_unorm != VK_FORMAT_UNDEFINED) ii.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    static VkImageFormatListCreateInfo list;
    static std::vector<VkFormat> list_formats;
    for (auto* n = static_cast<const VkBaseInStructure*>(ci->pNext); n; n = n->pNext)
      if (n->sType == VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO) {
        list = *reinterpret_cast<const VkImageFormatListCreateInfo*>(n);
        list.pNext = nullptr;
        list_formats.assign(list.pViewFormats, list.pViewFormats + list.viewFormatCount);
        if (srgb_unorm != VK_FORMAT_UNDEFINED &&
            std::find(list_formats.begin(), list_formats.end(), srgb_unorm) == list_formats.end())
          list_formats.push_back(srgb_unorm);
        list.viewFormatCount = static_cast<uint32_t>(list_formats.size());
        list.pViewFormats = list_formats.data();
        ii.pNext = &list;
      }
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = ci->imageFormat;
    ii.extent = {game_extent.width, game_extent.height, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = ci->imageArrayLayers;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = ci->imageUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ii.sharingMode = ci->imageSharingMode;
    ii.queueFamilyIndexCount = ci->queueFamilyIndexCount;
    ii.pQueueFamilyIndices = ci->pQueueFamilyIndices;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkPhysicalDeviceMemoryProperties mp{};
    ifn<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties")(d->phys, &mp);
    bool ok = true;
    for (uint32_t i = 0; i < ni && ok; ++i) {
      VkImage im = VK_NULL_HANDLE;
      VkDeviceMemory m = VK_NULL_HANDLE;
      ok = d->fn<PFN_vkCreateImage>("vkCreateImage")(dv, &ii, nullptr, &im) == VK_SUCCESS;
      if (ok) {
        VkMemoryRequirements req{};
        d->fn<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements")(dv, im, &req);
        uint32_t type = ~0u;
        for (uint32_t t = 0; t < mp.memoryTypeCount && type == ~0u; ++t)
          if ((req.memoryTypeBits & (1u << t)) && (mp.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) type = t;
        for (uint32_t t = 0; t < mp.memoryTypeCount && type == ~0u; ++t)
          if (req.memoryTypeBits & (1u << t)) type = t;
        VkMemoryAllocateInfo mi{};
        mi.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mi.allocationSize = req.size;
        mi.memoryTypeIndex = type;
        ok = type != ~0u && d->fn<PFN_vkAllocateMemory>("vkAllocateMemory")(dv, &mi, nullptr, &m) == VK_SUCCESS &&
             d->fn<PFN_vkBindImageMemory>("vkBindImageMemory")(dv, im, m, 0) == VK_SUCCESS;
      }
      sc.game.push_back(im);
      sc.game_mem.push_back(m);
    }
    VkSemaphoreCreateInfo bsi{};
    bsi.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    for (uint32_t i = 0; i < ni && ok; ++i) {
      VkSemaphore s2;
      ok = d->fn<PFN_vkCreateSemaphore>("vkCreateSemaphore")(dv, &bsi, nullptr, &s2) == VK_SUCCESS;
      if (ok) sc.blitted.push_back(s2);
    }
    if (!ok) LOG("resolution cap: could not make the game's images\n");
    else LOG("resolution cap: the game draws %ux%u, scaled up to %ux%u\n", game_extent.width, game_extent.height,
             c.imageExtent.width, c.imageExtent.height);
  }
  const VkFormat f = c.imageFormat;
  sc.bgra = f == VK_FORMAT_B8G8R8A8_UNORM || f == VK_FORMAT_B8G8R8A8_SRGB;
  sc.copyable = can_copy && (sc.bgra || f == VK_FORMAT_R8G8B8A8_UNORM || f == VK_FORMAT_R8G8B8A8_SRGB);
  VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, nullptr, 0};
  for (uint32_t i = 0; i < ni && sc.copyable; ++i) {
    VkSemaphore s;
    d->fn<PFN_vkCreateSemaphore>("vkCreateSemaphore")(dv, &si, nullptr, &s);
    sc.done.push_back(s);
  }
  static const char* names[] = {"immediate", "mailbox", "fifo", "fifo-relaxed"};
  LOG("swapchain %ux%u, %u images, %s%s\n", c.imageExtent.width, c.imageExtent.height, ni,
      c.presentMode <= 3 ? names[c.presentMode] : "other", sc.copyable ? "" : " (no overlays: the window cannot be copied to)");
  std::lock_guard<std::mutex> l(g_mu);
  g_swapchains[*out] = std::move(sc);
  return r;
}

void VKAPI_CALL my_DestroySwapchainKHR(VkDevice dv, VkSwapchainKHR s, const VkAllocationCallbacks* alloc) {
  std::shared_ptr<Device> d;
  Swapchain sc{};
  bool had = false;
  {
    std::lock_guard<std::mutex> l(g_mu);
    auto it = g_devices.find(dv);
    if (it != g_devices.end()) d = it->second;
    auto s2 = g_swapchains.find(s);
    if (s2 != g_swapchains.end()) sc = std::move(s2->second), g_swapchains.erase(s2), had = true;
  }
  if (!d) return;
  if (had && !sc.done.empty()) {
    for (Device::Slot& o : d->slot)
      if (o.pending) d->fn<PFN_vkWaitForFences>("vkWaitForFences")(dv, 1, &o.fence, VK_TRUE, UINT64_MAX), o.pending = false;
    for (VkSemaphore x : sc.done) d->fn<PFN_vkDestroySemaphore>("vkDestroySemaphore")(dv, x, nullptr);
  }
  if (had && sc.virt) {  // the game's images (after the GPU is done with them)
    d->fn<PFN_vkDeviceWaitIdle>("vkDeviceWaitIdle")(dv);
    sc.up.reset();  // (its images and pipelines, now that the GPU is done with them)
    for (VkSemaphore x : sc.blitted) d->fn<PFN_vkDestroySemaphore>("vkDestroySemaphore")(dv, x, nullptr);
    if (sc.blit_pool) d->fn<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(dv, sc.blit_pool, nullptr);
    for (VkImage x : sc.game)
      if (x) d->fn<PFN_vkDestroyImage>("vkDestroyImage")(dv, x, nullptr);
    for (VkDeviceMemory x : sc.game_mem)
      if (x) d->fn<PFN_vkFreeMemory>("vkFreeMemory")(dv, x, nullptr);
  }
  d->fn<PFN_vkDestroySwapchainKHR>("vkDestroySwapchainKHR")(dv, s, alloc);
}

VkResult VKAPI_CALL my_GetSwapchainImagesKHR(VkDevice dv, VkSwapchainKHR s, uint32_t* count, VkImage* images) {
  std::shared_ptr<Device> d;
  std::vector<VkImage> game;
  {
    std::lock_guard<std::mutex> l(g_mu);
    auto it = g_devices.find(dv);
    if (it != g_devices.end()) d = it->second;
    auto si = g_swapchains.find(s);
    if (si != g_swapchains.end() && si->second.virt) game = si->second.game;
  }
  if (game.empty()) {
    auto host = d ? d->fn<PFN_vkGetSwapchainImagesKHR>("vkGetSwapchainImagesKHR") : ifn<PFN_vkGetSwapchainImagesKHR>("vkGetSwapchainImagesKHR");
    return host(dv, s, count, images);
  }
  if (!images) {
    *count = static_cast<uint32_t>(game.size());
    return VK_SUCCESS;
  }
  const uint32_t n = std::min<uint32_t>(*count, static_cast<uint32_t>(game.size()));
  std::copy(game.begin(), game.begin() + n, images);
  *count = n;
  return n < game.size() ? VK_INCOMPLETE : VK_SUCCESS;
}

void barrier(Device& d, VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags src, VkAccessFlags dst);

// Resolution cap: scales the game's image IDX up into the window's image,
// after the game's semaphores. Returns the semaphore that says it is done.
VkSemaphore blit_up(VkQueue q, Device& d, uint32_t family, Swapchain& sc, uint32_t idx, uint32_t nwait, const VkSemaphore* wait) {
  if (sc.blitted.size() != sc.images.size() || sc.game.size() != sc.images.size()) return VK_NULL_HANDLE;
  if (sc.blit_family != family) {  // recorded once per image, on the presenting queue's family
    if (sc.blit_pool) d.fn<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(d.dev, sc.blit_pool, nullptr);
    sc.blit_pool = VK_NULL_HANDLE;
    sc.blit_cmd.assign(sc.images.size(), VK_NULL_HANDLE);
    VkCommandPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pi.queueFamilyIndex = family;
    if (d.fn<PFN_vkCreateCommandPool>("vkCreateCommandPool")(d.dev, &pi, nullptr, &sc.blit_pool) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = sc.blit_pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = static_cast<uint32_t>(sc.images.size());
    if (d.fn<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(d.dev, &ai, sc.blit_cmd.data()) != VK_SUCCESS) return VK_NULL_HANDLE;
    // FSR 1 / NIS (Settings > Picture), when this queue can run compute shaders.
    sc.up.reset();
    if (const UpscaleMode mode = upscale_mode(); mode != UpscaleMode::Off) {
      uint32_t nf = 0;
      auto qf = ifn<PFN_vkGetPhysicalDeviceQueueFamilyProperties>("vkGetPhysicalDeviceQueueFamilyProperties");
      qf(d.phys, &nf, nullptr);
      std::vector<VkQueueFamilyProperties> fams(nf);
      qf(d.phys, &nf, fams.data());
      if (family < nf && (fams[family].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
        UpscaleSetup us;
        us.dev = d.dev, us.gdpa = d.gdpa, us.queue = q, us.family = family, us.mode = mode;
        ifn<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties")(d.phys, &us.mem);
        us.strength = std::clamp(config_int(mode == UpscaleMode::FSR ? "fsr_sharpness" : "nis_sharpness",
                                            mode == UpscaleMode::FSR ? 80 : 50), 0, 100) / 100.f;
        us.in = sc.game_extent, us.out = sc.extent;
        us.game_format = sc.format, us.game = sc.game;
        us.swap_format = sc.format, us.swap = sc.images;
        auto up = std::make_shared<Upscaler>();
        if (up->init(us)) sc.up = up;
        else LOG("%s could not start here: plain scaling\n", upscale_name(mode));
      } else {
        LOG("%s needs a queue that runs compute shaders: plain scaling\n", upscale_name(mode));
      }
    }
    for (size_t i = 0; i < sc.images.size(); ++i) {
      VkCommandBuffer cb = sc.blit_cmd[i];
      VkCommandBufferBeginInfo bi{};
      bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
      bi.flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
      d.fn<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(cb, &bi);
      if (sc.up) {  // compute passes; the result copied into the window's image
        barrier(d, cb, sc.game[i], VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_MEMORY_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT);
        barrier(d, cb, sc.images[i], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        sc.up->record(cb, i);
        barrier(d, cb, sc.game[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_SHADER_READ_BIT, 0);
        barrier(d, cb, sc.images[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, 0);
        d.fn<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(cb);
        continue;
      }
      barrier(d, cb, sc.game[i], VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_MEMORY_WRITE_BIT,
              VK_ACCESS_TRANSFER_READ_BIT);
      barrier(d, cb, sc.images[i], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
      VkImageBlit r{};
      r.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      r.srcOffsets[1] = {static_cast<int32_t>(sc.game_extent.width), static_cast<int32_t>(sc.game_extent.height), 1};
      r.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      r.dstOffsets[1] = {static_cast<int32_t>(sc.extent.width), static_cast<int32_t>(sc.extent.height), 1};
      d.fn<PFN_vkCmdBlitImage>("vkCmdBlitImage")(cb, sc.game[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sc.images[i],
                                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r, VK_FILTER_LINEAR);
      barrier(d, cb, sc.game[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_READ_BIT, 0);
      barrier(d, cb, sc.images[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, 0);
      d.fn<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(cb);
    }
    sc.blit_family = family;
  }
  std::vector<VkPipelineStageFlags> stages(nwait, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
  VkSubmitInfo s{};
  s.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  s.waitSemaphoreCount = nwait;
  s.pWaitSemaphores = wait;
  s.pWaitDstStageMask = stages.data();
  s.commandBufferCount = 1;
  s.pCommandBuffers = &sc.blit_cmd[idx];
  s.signalSemaphoreCount = 1;
  s.pSignalSemaphores = &sc.blitted[idx];
  if (d.fn<PFN_vkQueueSubmit>("vkQueueSubmit")(q, 1, &s, VK_NULL_HANDLE) != VK_SUCCESS) return VK_NULL_HANDLE;
  return sc.blitted[idx];
}

// --- tonic's overlays (typed text, stats) and screenshots on a Vulkan frame ----------

bool ensure_overlay(Device& d, uint32_t family, VkDeviceSize need) {
  if (d.family != family) {
    free_overlay(d);
    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                               family};
    if (d.fn<PFN_vkCreateCommandPool>("vkCreateCommandPool")(d.dev, &pi, nullptr, &d.pool) != VK_SUCCESS) return false;
    for (Device::Slot& o : d.slot) {
      VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, d.pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 2};
      if (d.fn<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(d.dev, &ai, o.cmd) != VK_SUCCESS) return false;
      VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, 0};
      if (d.fn<PFN_vkCreateFence>("vkCreateFence")(d.dev, &fi, nullptr, &o.fence) != VK_SUCCESS) return false;
    }
    d.family = family;
  }
  Device::Slot& s = d.slot[d.cur];  // (not pending: overlay() waited for it)
  if (s.size >= need) return true;
  if (s.buf) d.fn<PFN_vkDestroyBuffer>("vkDestroyBuffer")(d.dev, s.buf, nullptr), s.buf = VK_NULL_HANDLE;
  if (s.mem) d.fn<PFN_vkFreeMemory>("vkFreeMemory")(d.dev, s.mem, nullptr), s.mem = VK_NULL_HANDLE;
  s.size = 0, s.mapped = nullptr;
  VkBufferCreateInfo bi{};
  bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  bi.size = need;
  bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (d.fn<PFN_vkCreateBuffer>("vkCreateBuffer")(d.dev, &bi, nullptr, &s.buf) != VK_SUCCESS) return false;
  VkMemoryRequirements req{};
  d.fn<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(d.dev, s.buf, &req);
  VkPhysicalDeviceMemoryProperties mp{};
  ifn<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties")(d.phys, &mp);
  const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  uint32_t type = ~0u;
  for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
    if ((req.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) {
      type = i;
      break;
    }
  if (type == ~0u) return false;
  VkMemoryAllocateInfo mi{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, req.size, type};
  if (d.fn<PFN_vkAllocateMemory>("vkAllocateMemory")(d.dev, &mi, nullptr, &s.mem) != VK_SUCCESS) return false;
  d.fn<PFN_vkBindBufferMemory>("vkBindBufferMemory")(d.dev, s.buf, s.mem, 0);
  void* p = nullptr;
  if (d.fn<PFN_vkMapMemory>("vkMapMemory")(d.dev, s.mem, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS) return false;
  s.mapped = static_cast<uint8_t*>(p);
  s.size = need;
  return true;
}

void barrier(Device& d, VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags src, VkAccessFlags dst) {
  VkImageMemoryBarrier b{};
  b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  b.srcAccessMask = src;
  b.dstAccessMask = dst;
  b.oldLayout = from;
  b.newLayout = to;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = img;
  b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  d.fn<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                                                         nullptr, 0, nullptr, 1, &b);
}

// Draws the overlays (and takes the screenshot) on image IDX before it is
// presented. Returns the semaphore the present must wait for, or none (then
// present as the game asked).
VkSemaphore overlay(VkQueue q, Device& d, uint32_t family, Swapchain& sc, uint32_t idx, uint32_t nwait, const VkSemaphore* wait,
                    const std::vector<OverlayRect>& rects, bool shot, float scale) {
  const int W = static_cast<int>(sc.extent.width), H = static_cast<int>(sc.extent.height);
  // Staging layout: the screenshot first (whole image), then each rect.
  std::vector<VkDeviceSize> at;
  VkDeviceSize need = shot ? static_cast<VkDeviceSize>(W) * H * 4 : 0;
  for (auto& r : rects) at.push_back(need), need += static_cast<VkDeviceSize>(r.w) * r.h * 4;
  auto wait_fence = d.fn<PFN_vkWaitForFences>("vkWaitForFences");
  auto reset_fence = d.fn<PFN_vkResetFences>("vkResetFences");
  if (d.family != family) d.cur = 0;
  else d.cur ^= 1;  // the other buffer: the last frame's copy may still run
  Device::Slot& sl = d.slot[d.cur];
  if (sl.pending) {
    wait_fence(d.dev, 1, &sl.fence, VK_TRUE, UINT64_MAX);
    reset_fence(d.dev, 1, &sl.fence);
    sl.pending = false;
  }
  if (!need || !ensure_overlay(d, family, need)) return VK_NULL_HANDLE;
  auto begin = d.fn<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer");
  auto end = d.fn<PFN_vkEndCommandBuffer>("vkEndCommandBuffer");
  auto submit = d.fn<PFN_vkQueueSubmit>("vkQueueSubmit");
  auto to_buf = d.fn<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer");
  auto to_img = d.fn<PFN_vkCmdCopyBufferToImage>("vkCmdCopyBufferToImage");
  const VkImage img = sc.images[idx];
  std::vector<VkPipelineStageFlags> stages(nwait, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, nullptr};
  auto region = [&](VkDeviceSize off, int x, int y, int w, int h) {
    VkBufferImageCopy c{};
    c.bufferOffset = off;
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.imageOffset = {x, y, 0};
    c.imageExtent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1};
    return c;
  };
  const bool readback = shot || std::any_of(rects.begin(), rects.end(), [](const OverlayRect& r) { return r.blend; });
  bool waited = false;
  VkImageLayout layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  if (readback) {  // 1: the game's pixels to the CPU
    VkCommandBuffer cb = sl.cmd[0];
    begin(cb, &bi);
    barrier(d, cb, img, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    std::vector<VkBufferImageCopy> regions;
    if (shot) regions.push_back(region(0, 0, 0, W, H));
    for (size_t i = 0; i < rects.size(); ++i)
      if (rects[i].blend) regions.push_back(region(at[i], rects[i].x, rects[i].y, rects[i].w, rects[i].h));
    to_buf(cb, img, layout, sl.buf, static_cast<uint32_t>(regions.size()), regions.data());
    end(cb);
    VkSubmitInfo s{};
    s.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    s.waitSemaphoreCount = nwait;
    s.pWaitSemaphores = wait;
    s.pWaitDstStageMask = stages.data();
    s.commandBufferCount = 1;
    s.pCommandBuffers = &cb;
    if (submit(q, 1, &s, sl.fence) != VK_SUCCESS) return VK_NULL_HANDLE;
    wait_fence(d.dev, 1, &sl.fence, VK_TRUE, UINT64_MAX);
    reset_fence(d.dev, 1, &sl.fence);
    waited = true;
  }
  if (shot) {  // to RGBA, top row first
    auto px = std::make_shared<std::vector<uint8_t>>(sl.mapped, sl.mapped + static_cast<size_t>(W) * H * 4);
    if (sc.bgra)
      for (size_t i = 0; i < px->size(); i += 4) std::swap((*px)[i], (*px)[i + 2]);
    window_save_shot(px, W, H);
  }
  for (size_t i = 0; i < rects.size(); ++i) textoverlay_cpu_paint(rects[i], sl.mapped + at[i], rects[i].w * 4, sc.bgra, scale);
  // 2: the overlays back into the image, then on to the present.
  VkCommandBuffer cb = sl.cmd[1];
  begin(cb, &bi);
  if (!rects.empty()) {
    barrier(d, cb, img, layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT);
    layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    std::vector<VkBufferImageCopy> regions;
    for (size_t i = 0; i < rects.size(); ++i) regions.push_back(region(at[i], rects[i].x, rects[i].y, rects[i].w, rects[i].h));
    to_img(cb, sl.buf, img, layout, static_cast<uint32_t>(regions.size()), regions.data());
  }
  barrier(d, cb, img, layout, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT, 0);
  end(cb);
  VkSubmitInfo s{};
  s.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  if (!waited) {
    s.waitSemaphoreCount = nwait;
    s.pWaitSemaphores = wait;
    s.pWaitDstStageMask = stages.data();
  }
  s.commandBufferCount = 1;
  s.pCommandBuffers = &cb;
  s.signalSemaphoreCount = 1;
  s.pSignalSemaphores = &sc.done[idx];
  if (submit(q, 1, &s, sl.fence) != VK_SUCCESS) return VK_NULL_HANDLE;
  sl.pending = true;
  return sc.done[idx];
}

void first_frame() {
  if (g_presented.exchange(true)) return;
  if (!g_marker.empty()) unlink(g_marker.c_str());
  LOG("Roblox draws with Vulkan\n");
}

VkResult VKAPI_CALL my_QueuePresentKHR(VkQueue q, const VkPresentInfoKHR* pi) {
  window_present_gate();
  std::shared_ptr<Device> d;
  uint32_t family = 0;
  Swapchain* sc = nullptr;
  {
    std::lock_guard<std::mutex> l(g_mu);
    auto qi = g_queues.find(q);
    if (qi != g_queues.end()) {
      auto di = g_devices.find(qi->second.first);
      if (di != g_devices.end()) d = di->second, family = qi->second.second;
    }
    if (pi->swapchainCount == 1) {
      auto si = g_swapchains.find(pi->pSwapchains[0]);
      if (si != g_swapchains.end()) sc = &si->second;
    }
  }
  if (!d) return ifn<PFN_vkQueuePresentKHR>("vkQueuePresentKHR")(q, pi);
  auto present = d->fn<PFN_vkQueuePresentKHR>("vkQueuePresentKHR");
  VkResult r;
  VkSemaphore done = VK_NULL_HANDLE;
  uint32_t nwait = pi->waitSemaphoreCount;
  const VkSemaphore* wait = pi->pWaitSemaphores;
  if (sc && sc->virt) {  // resolution cap: scale the game's frame up first
    done = blit_up(q, *d, family, *sc, pi->pImageIndices[0], nwait, wait);
    if (done) nwait = 1, wait = &done;
  }
  if (sc && sc->copyable) {
    const uint32_t idx = pi->pImageIndices[0];
    const float scale = sc->virt && sc->game_extent.width ? static_cast<float>(sc->extent.width) / sc->game_extent.width : 1.f;
    const std::vector<OverlayRect> rects =
        textoverlay_cpu_rects(static_cast<int>(sc->extent.width), static_cast<int>(sc->extent.height), scale);
    const bool shot = window_take_shot();
    if (!rects.empty() || shot)
      if (VkSemaphore o = overlay(q, *d, family, *sc, idx, nwait, wait, rects, shot, scale)) done = o;
  }
  if (done) {
    VkPresentInfoKHR p = *pi;
    p.waitSemaphoreCount = 1;
    p.pWaitSemaphores = &done;
    r = present(q, &p);
  } else {
    r = present(q, pi);
  }
  if (sc) {
    if (r >= 0) first_frame();
    window_present_pace();
    textoverlay_presented();
    frame_done(r >= 0);
  }
  return r;
}

// --- function lookup ----------------------------------------------------------------------

PFN_vkVoidFunction ours(const char* name) {
#define OURS(n) \
  if (!strcmp(name, "vk" #n)) return reinterpret_cast<PFN_vkVoidFunction>(&my_##n);
  OURS(EnumerateInstanceExtensionProperties)
  OURS(CreateInstance)
  OURS(CreateAndroidSurfaceKHR)
  OURS(GetPhysicalDeviceSurfaceCapabilitiesKHR)
  OURS(GetPhysicalDeviceSurfaceCapabilities2KHR)
  OURS(CreateDevice)
  OURS(DestroyDevice)
  OURS(GetDeviceQueue)
  OURS(GetDeviceQueue2)
  OURS(CreateSwapchainKHR)
  OURS(DestroySwapchainKHR)
  OURS(GetSwapchainImagesKHR)
  OURS(QueuePresentKHR)
#undef OURS
  return nullptr;
}

PFN_vkVoidFunction VKAPI_CALL my_GetDeviceProcAddr(VkDevice d, const char* name);
PFN_vkVoidFunction VKAPI_CALL my_GetInstanceProcAddr(VkInstance inst, const char* name) {
  if (!strcmp(name, "vkGetInstanceProcAddr")) return reinterpret_cast<PFN_vkVoidFunction>(&my_GetInstanceProcAddr);
  if (!strcmp(name, "vkGetDeviceProcAddr")) return reinterpret_cast<PFN_vkVoidFunction>(&my_GetDeviceProcAddr);
  if (auto f = ours(name)) return f;
  return h_gipa(inst, name);
}
PFN_vkVoidFunction VKAPI_CALL my_GetDeviceProcAddr(VkDevice dv, const char* name) {
  if (!strcmp(name, "vkGetDeviceProcAddr")) return reinterpret_cast<PFN_vkVoidFunction>(&my_GetDeviceProcAddr);
  if (auto f = ours(name)) return f;
  std::shared_ptr<Device> d;
  {
    std::lock_guard<std::mutex> l(g_mu);
    auto it = g_devices.find(dv);
    if (it != g_devices.end()) d = it->second;
  }
  return d ? d->gdpa(dv, name) : ifn<PFN_vkGetDeviceProcAddr>("vkGetDeviceProcAddr")(dv, name);
}

// --- dlopen("libvulkan.so") ------------------------------------------------------------------

void* sh_dlopen(const char* name, int flags) {
  if (name && strstr(name, "libvulkan")) {
    if (!g_enabled) {
      LOG("Roblox looked for Vulkan: OpenGL this run\n");
      return nullptr;
    }
    if (!g_host) {
      g_host = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
      if (g_host) h_gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(g_host, "vkGetInstanceProcAddr"));
      if (!g_host || !h_gipa) {
        LOG("no Vulkan on this system (libvulkan.so.1): Roblox uses OpenGL\n");
        g_host = nullptr;
        return nullptr;
      }
    }
    LOG("Roblox opened Vulkan (through tonic, drawing to the %s window)\n", g_wayland ? "Wayland" : "X11");
    return &g_tag;
  }
  return dlopen(name, flags);
}
void* sh_dlsym(void* handle, const char* name) {
  if (handle == &g_tag) {
    if (!strcmp(name, "vkGetInstanceProcAddr")) return reinterpret_cast<void*>(&my_GetInstanceProcAddr);
    if (!strcmp(name, "vkGetDeviceProcAddr")) return reinterpret_cast<void*>(&my_GetDeviceProcAddr);
    if (auto f = ours(name)) return reinterpret_cast<void*>(f);
    return dlsym(g_host, name);
  }
  return dlsym(handle, name);
}
int sh_dlclose(void* handle) { return handle == &g_tag ? 0 : dlclose(handle); }

}  // namespace

void register_vulkan_shims() {
  register_shim("dlopen", reinterpret_cast<void*>(&sh_dlopen));
  register_shim("dlsym", reinterpret_cast<void*>(&sh_dlsym));
  register_shim("dlclose", reinterpret_cast<void*>(&sh_dlclose));
}

void vulkan_decide() {
  g_started = Clock::now();
  bool wayland;
  void *display, *surface;
  window_vk_target(wayland, display, surface);
  g_wayland = wayland;
  g_platform_ext = wayland ? "VK_KHR_wayland_surface" : "VK_KHR_xlib_surface";
  g_marker = profile_dir() + "/vulkan-starting";
  struct stat st{};
  const bool crashed = stat(g_marker.c_str(), &st) == 0;
  if (renderer_setting() != "vulkan") {
    unlink(g_marker.c_str());
    g_enabled = false;
    LOG("renderer: OpenGL (setting)\n");
    return;
  }
  if (crashed) {
    config_set("vulkan_failed", "1");
    unlink(g_marker.c_str());
    LOG("Vulkan did not show a frame last time: OpenGL from now on (choose Vulkan in Settings to try again)\n");
  }
  if (config_bool("vulkan_failed", false)) {
    g_enabled = false;
    LOG("renderer: OpenGL (Vulkan did not work before)\n");
    return;
  }
  if (!surface) {
    g_enabled = false;
    return;
  }
  g_enabled = true;
  std::ofstream(g_marker) << "starting\n";
  LOG("renderer: Vulkan, OpenGL if it does not work\n");
}

bool vulkan_enabled() { return g_enabled; }

std::string renderer_setting() { return config_get("renderer", "vulkan") == "opengl" ? "opengl" : "vulkan"; }

void vulkan_exit() {
  // Closed before Roblox drew anything (quit on the loading screen): that says
  // nothing about Vulkan, so it is not counted as a failure.
  if (g_enabled && !g_presented && !g_marker.empty() && Clock::now() - g_started < std::chrono::seconds(45))
    unlink(g_marker.c_str());
}

}  // namespace tonic
