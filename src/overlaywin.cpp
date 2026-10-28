#include "overlaywin.hpp"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <vector>

#include "closescreen.hpp"

namespace tonic {

namespace {

#define LOG(fmt, ...) fprintf(stderr, "[closing] " fmt, ##__VA_ARGS__)

std::atomic<bool> g_on{false};
bool g_wayland = false;
int g_w = 0, g_h = 0;  // the overlay's size (window pixels on X11, logical on Wayland)
long g_gen = -1;

// --- Wayland: a sub-surface of the game window's surface --------------------------------

wl_display* g_dpy = nullptr;
wl_event_queue* g_queue = nullptr;
wl_display* g_wrapped = nullptr;
wl_registry* g_registry = nullptr;
wl_compositor* g_compositor = nullptr;
wl_subcompositor* g_subcompositor = nullptr;
wl_shm* g_shm = nullptr;
wl_surface* g_parent = nullptr;
wl_surface* g_child = nullptr;
wl_subsurface* g_sub = nullptr;
struct Buf {
  wl_buffer* buf = nullptr;
  uint8_t* px = nullptr;
  size_t size = 0;
  bool busy = false;
};
Buf g_bufs[2];
wl_shm_pool* g_pool = nullptr;
int g_fd = -1;
uint8_t* g_map = nullptr;
size_t g_map_size = 0;

void on_global(void*, wl_registry* r, uint32_t name, const char* iface, uint32_t version) {
  if (!strcmp(iface, wl_compositor_interface.name))
    g_compositor = static_cast<wl_compositor*>(wl_registry_bind(r, name, &wl_compositor_interface, std::min(version, 4u)));
  else if (!strcmp(iface, wl_subcompositor_interface.name))
    g_subcompositor = static_cast<wl_subcompositor*>(wl_registry_bind(r, name, &wl_subcompositor_interface, 1));
  else if (!strcmp(iface, wl_shm_interface.name))
    g_shm = static_cast<wl_shm*>(wl_registry_bind(r, name, &wl_shm_interface, 1));
}
void on_global_remove(void*, wl_registry*, uint32_t) {}
const wl_registry_listener kRegistry = {on_global, on_global_remove};
void on_release(void* data, wl_buffer*) { static_cast<Buf*>(data)->busy = false; }
const wl_buffer_listener kBuffer = {on_release};

void wl_free_buffers() {
  for (Buf& b : g_bufs) {
    if (b.buf) wl_buffer_destroy(b.buf);
    b = Buf{};
  }
  if (g_pool) wl_shm_pool_destroy(g_pool), g_pool = nullptr;
  if (g_map) munmap(g_map, g_map_size), g_map = nullptr;
  if (g_fd >= 0) close(g_fd), g_fd = -1;
}

bool wl_make_buffers(int w, int h) {
  wl_free_buffers();
  const int stride = w * 4;
  const size_t one = static_cast<size_t>(stride) * h;
  g_map_size = one * 2;
  g_fd = memfd_create("tonic-closing", MFD_CLOEXEC);
  if (g_fd < 0 || ftruncate(g_fd, static_cast<off_t>(g_map_size)) != 0) return false;
  void* m = mmap(nullptr, g_map_size, PROT_READ | PROT_WRITE, MAP_SHARED, g_fd, 0);
  if (m == MAP_FAILED) return false;
  g_map = static_cast<uint8_t*>(m);
  g_pool = wl_shm_create_pool(g_shm, g_fd, static_cast<int32_t>(g_map_size));
  for (int i = 0; i < 2; ++i) {
    Buf& b = g_bufs[i];
    b.buf = wl_shm_pool_create_buffer(g_pool, static_cast<int32_t>(one * i), w, h, stride, WL_SHM_FORMAT_XRGB8888);
    b.px = g_map + one * i;
    b.size = one;
    wl_buffer_add_listener(b.buf, &kBuffer, &b);
  }
  return true;
}

bool wl_globals(void* display) {
  g_dpy = static_cast<wl_display*>(display);
  if (!g_dpy) return false;
  // Our own event queue, so SDL's event handling is not disturbed.
  if (!g_queue) {
    g_queue = wl_display_create_queue(g_dpy);
    g_wrapped = static_cast<wl_display*>(wl_proxy_create_wrapper(g_dpy));
    wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(g_wrapped), g_queue);
    g_registry = wl_display_get_registry(g_wrapped);
    wl_registry_add_listener(g_registry, &kRegistry, nullptr);
    wl_display_roundtrip_queue(g_dpy, g_queue);
  }
  return g_compositor != nullptr;
}

bool wl_show(void* display, void* parent, int w, int h) {
  g_parent = static_cast<wl_surface*>(parent);
  if (!wl_globals(display) || !g_parent) return false;
  if (!g_compositor || !g_subcompositor || !g_shm) {
    LOG("this compositor has no sub-surfaces: the closing screen is drawn in Roblox's frames instead\n");
    return false;
  }
  g_child = wl_compositor_create_surface(g_compositor);
  wl_region* none = wl_compositor_create_region(g_compositor);  // clicks go through to the game window
  wl_surface_set_input_region(g_child, none);
  wl_region_destroy(none);
  // Fully opaque: nothing of the game (or the desktop) shows through.
  wl_region* all = wl_compositor_create_region(g_compositor);
  wl_region_add(all, 0, 0, w, h);
  wl_surface_set_opaque_region(g_child, all);
  wl_region_destroy(all);
  g_sub = wl_subcompositor_get_subsurface(g_subcompositor, g_child, g_parent);
  wl_subsurface_set_position(g_sub, 0, 0);
  wl_subsurface_place_above(g_sub, g_parent);
  wl_subsurface_set_desync(g_sub);
  if (!wl_make_buffers(w, h)) return false;
  return true;
}

void wl_draw(const std::vector<uint8_t>& rgba, int w, int h) {
  wl_display_dispatch_queue_pending(g_dpy, g_queue);  // buffer releases
  Buf* b = !g_bufs[0].busy ? &g_bufs[0] : !g_bufs[1].busy ? &g_bufs[1] : nullptr;
  if (!b) return;
  const uint8_t* s = rgba.data();
  uint8_t* d = b->px;
  for (size_t i = 0, n = static_cast<size_t>(w) * h; i < n; ++i, s += 4, d += 4) d[0] = s[2], d[1] = s[1], d[2] = s[0], d[3] = 255;
  wl_surface_attach(g_child, b->buf, 0, 0);
  wl_surface_damage(g_child, 0, 0, w, h);
  wl_surface_commit(g_child);
  b->busy = true;
  static bool parent_committed = false;
  if (!parent_committed) {  // the sub-surface takes its place on the parent's next commit
    wl_surface_commit(g_parent);
    parent_committed = true;
  }
  wl_display_flush(g_dpy);
}

void wl_hide() {
  if (g_sub) wl_subsurface_destroy(g_sub), g_sub = nullptr;
  if (g_child) wl_surface_destroy(g_child), g_child = nullptr;
  wl_free_buffers();
  if (g_parent) wl_surface_commit(g_parent);
  if (g_dpy) wl_display_flush(g_dpy);
}

// --- X11: a child window over the game window -----------------------------------------------

Display* g_xdpy = nullptr;
Window g_xchild = 0;
GC g_gc = nullptr;
XImage* g_ximg = nullptr;
std::vector<uint8_t> g_xpx;

bool x_show(void* display, void* parent, int w, int h) {
  g_xdpy = static_cast<Display*>(display);
  const Window p = static_cast<Window>(reinterpret_cast<uintptr_t>(parent));
  if (!g_xdpy || !p) return false;
  const int scr = DefaultScreen(g_xdpy);
  if (DefaultDepth(g_xdpy, scr) < 24) return false;
  g_xchild = XCreateSimpleWindow(g_xdpy, p, 0, 0, w, h, 0, 0, 0);
  XMapRaised(g_xdpy, g_xchild);  // no input selected: clicks reach the game window
  g_gc = XCreateGC(g_xdpy, g_xchild, 0, nullptr);
  g_xpx.assign(static_cast<size_t>(w) * h * 4, 0);
  g_ximg = XCreateImage(g_xdpy, DefaultVisual(g_xdpy, scr), DefaultDepth(g_xdpy, scr), ZPixmap, 0,
                        reinterpret_cast<char*>(g_xpx.data()), w, h, 32, w * 4);
  return g_ximg != nullptr;
}

void x_draw(const std::vector<uint8_t>& rgba, int w, int h) {
  const uint8_t* s = rgba.data();
  uint8_t* d = g_xpx.data();
  for (size_t i = 0, n = static_cast<size_t>(w) * h; i < n; ++i, s += 4, d += 4) d[0] = s[2], d[1] = s[1], d[2] = s[0], d[3] = 255;
  XPutImage(g_xdpy, g_xchild, g_gc, g_ximg, 0, 0, 0, 0, w, h);
  XFlush(g_xdpy);
}

void x_hide() {
  if (g_ximg) {
    g_ximg->data = nullptr;  // ours (g_xpx)
    XDestroyImage(g_ximg);
    g_ximg = nullptr;
  }
  if (g_gc) XFreeGC(g_xdpy, g_gc), g_gc = nullptr;
  if (g_xchild) XDestroyWindow(g_xdpy, g_xchild), g_xchild = 0;
  if (g_xdpy) XFlush(g_xdpy);
}

}  // namespace

bool overlaywin_show(bool wayland, void* display, void* parent, int w, int h) {
  if (g_on) return true;
  if (w <= 0 || h <= 0) return false;
  g_wayland = wayland;
  const bool ok = wayland ? wl_show(display, parent, w, h) : x_show(display, parent, w, h);
  if (!ok) {
    if (wayland) wl_hide();
    else x_hide();
    return false;
  }
  g_on = true;
  g_w = w, g_h = h;
  g_gen = -1;
  return true;
}

void overlaywin_frame() {
  if (!g_on) return;
  const std::vector<uint8_t>* img = nullptr;
  long gen = 0;
  if (!closescreen_image(g_w, g_h, img, gen) || gen == g_gen) return;
  g_gen = gen;
  if (g_wayland) wl_draw(*img, g_w, g_h);
  else x_draw(*img, g_w, g_h);
}

void overlaywin_hide() {
  if (!g_on) return;
  g_on = false;
  if (g_wayland) wl_hide();
  else x_hide();
}

bool overlaywin_on() { return g_on; }

void overlaywin_opaque(void* display, void* surface) {
  if (!surface || !wl_globals(display)) return;
  wl_region* all = wl_compositor_create_region(g_compositor);
  wl_region_add(all, 0, 0, 1 << 20, 1 << 20);  // (clipped to the surface, whatever its size)
  wl_surface_set_opaque_region(static_cast<wl_surface*>(surface), all);  // applies on the game's next frame
  wl_region_destroy(all);
  wl_display_flush(g_dpy);
}

}  // namespace tonic
