#include "textoverlay.hpp"
#include "spawn.hpp"
#include "device.hpp"
#include "framegen.hpp"

#include <dlfcn.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>

#include "assets.hpp"
#include "closescreen.hpp"
#include "overlaywin.hpp"
#include "config.hpp"
#include "input.hpp"
#include "game.hpp"
#include "netping.hpp"
#include "window.hpp"

#include <sys/resource.h>
#include <unistd.h>

namespace tonic {

namespace {

#define LOG(fmt, ...) fprintf(stderr, "[textbox] " fmt, ##__VA_ARGS__)

// --- the box ------------------------------------------------------------------

struct Info {
  float x = 0, y = 0, w = 0, h = 0, font_size = 0;
  bool multiline = false;
  int x_align = 0, y_align = 0;
  uint32_t color = 0xFF000000;
  int font = 0, input_type = 0;
  float pad_l = 0, pad_t = 0, pad_r = 0, pad_b = 0;
  bool valid = false;
};
std::mutex g_mu;
Info g_info;
long g_info_gen = 0;

float f_of(uint64_t bits) {
  double d;
  memcpy(&d, &bits, 8);
  return static_cast<float>(d);
}

// --- GL (looked up in the host's libGLESv2, no headers needed) ----------------

using GLuint = unsigned;
using GLint = int;
using GLenum = unsigned;
constexpr GLenum kTexture2D = 0x0DE1, kBlend = 0x0BE2, kDepthTest = 0x0B71, kCullFace = 0x0B44,
                 kScissorTest = 0x0C11, kStencilTest = 0x0B90, kArrayBuffer = 0x8892, kTexture0 = 0x84C0,
                 kRGBA = 0x1908, kUnsignedByte = 0x1401, kFloat = 0x1406, kTriangleStrip = 0x0005,
                 kDynamicDraw = 0x88E8, kVertexShader = 0x8B31, kFragmentShader = 0x8B30,
                 kLinear = 0x2601, kClampToEdge = 0x812F, kTexMinFilter = 0x2801, kTexMagFilter = 0x2800,
                 kTexWrapS = 0x2802, kTexWrapT = 0x2803, kSrcAlpha = 0x0302, kOneMinusSrcAlpha = 0x0303,
                 kOne = 1, kFuncAdd = 0x8006, kRasterizerDiscard = 0x8C89;
// glGet names
constexpr GLenum kCurrentProgram = 0x8B8D, kArrayBufferBinding = 0x8894, kVertexArrayBinding = 0x85B5,
                 kActiveTexture = 0x84E0, kTextureBinding2D = 0x8069, kSamplerBinding = 0x8919,
                 kViewport = 0x0BA2, kScissorBox = 0x0C10, kDrawFramebufferBinding = 0x8CA6,
                 kBlendSrcRgb = 0x80C9, kBlendDstRgb = 0x80C8, kBlendSrcAlpha = 0x80CB, kBlendDstAlpha = 0x80CA,
                 kBlendEqRgb = 0x8009, kBlendEqAlpha = 0x883D, kColorWritemask = 0x0C23,
                 kUnpackAlignment = 0x0CF5, kUnpackRowLength = 0x0CF2, kPixelUnpackBufferBinding = 0x88EF,
                 kPixelUnpackBuffer = 0x88EC, kDrawFramebuffer = 0x8CA9,
                 kUnpackSkipRows = 0x0CF3, kUnpackSkipPixels = 0x0CF4;

struct GL {
  void (*GetIntegerv)(GLenum, GLint*);
  void (*GetBooleanv)(GLenum, unsigned char*);
  unsigned char (*IsEnabled)(GLenum);
  void (*Enable)(GLenum);
  void (*Disable)(GLenum);
  void (*BlendFuncSeparate)(GLenum, GLenum, GLenum, GLenum);
  void (*BlendEquationSeparate)(GLenum, GLenum);
  void (*Viewport)(GLint, GLint, GLint, GLint);
  void (*Scissor)(GLint, GLint, GLint, GLint);
  void (*ColorMask)(unsigned char, unsigned char, unsigned char, unsigned char);
  void (*UseProgram)(GLuint);
  void (*BindBuffer)(GLenum, GLuint);
  void (*BindVertexArray)(GLuint);
  void (*GenVertexArrays)(GLint, GLuint*);
  void (*BindTexture)(GLenum, GLuint);
  void (*ActiveTexture)(GLenum);
  void (*BindSampler)(GLuint, GLuint);
  void (*BindFramebuffer)(GLenum, GLuint);
  void (*PixelStorei)(GLenum, GLint);
  void (*GenTextures)(GLint, GLuint*);
  void (*TexImage2D)(GLenum, GLint, GLint, GLint, GLint, GLint, GLenum, GLenum, const void*);
  void (*CopyTexImage2D)(GLenum, GLint, GLenum, GLint, GLint, GLint, GLint, GLint);
  unsigned (*GetError)();
  void (*Uniform1f)(GLint, float);
  void (*TexParameteri)(GLenum, GLenum, GLint);
  GLuint (*CreateShader)(GLenum);
  void (*ShaderSource)(GLuint, GLint, const char* const*, const GLint*);
  void (*CompileShader)(GLuint);
  GLuint (*CreateProgram)();
  void (*AttachShader)(GLuint, GLuint);
  void (*BindAttribLocation)(GLuint, GLuint, const char*);
  void (*LinkProgram)(GLuint);
  void (*GetProgramiv)(GLuint, GLenum, GLint*);
  GLint (*GetUniformLocation)(GLuint, const char*);
  void (*Uniform1i)(GLint, GLint);
  void (*Uniform4f)(GLint, float, float, float, float);
  void (*Uniform2f)(GLint, float, float);
  void (*GenBuffers)(GLint, GLuint*);
  void (*BufferData)(GLenum, intptr_t, const void*, GLenum);
  void (*VertexAttribPointer)(GLuint, GLint, GLenum, unsigned char, GLint, const void*);
  void (*EnableVertexAttribArray)(GLuint);
  void (*DrawArrays)(GLenum, GLint, GLint);
  const unsigned char* (*GetString)(GLenum);
  void (*GetVertexAttribiv)(GLuint, GLenum, GLint*);
  void (*GetVertexAttribPointerv)(GLuint, GLenum, void**);
  void (*DisableVertexAttribArray)(GLuint);
  void (*ReadPixels)(GLint, GLint, GLint, GLint, GLenum, GLenum, void*);
} gl;

bool load_gl() {
  void* lib = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_GLOBAL);
  if (!lib) return false;
  bool ok = true;
  auto get = [&](auto& slot, const char* name, bool required = true) {
    slot = reinterpret_cast<std::remove_reference_t<decltype(slot)>>(dlsym(lib, name));
    if (!slot && required) ok = false;
  };
  get(gl.GetIntegerv, "glGetIntegerv");
  get(gl.GetBooleanv, "glGetBooleanv");
  get(gl.IsEnabled, "glIsEnabled");
  get(gl.Enable, "glEnable");
  get(gl.Disable, "glDisable");
  get(gl.BlendFuncSeparate, "glBlendFuncSeparate");
  get(gl.BlendEquationSeparate, "glBlendEquationSeparate");
  get(gl.Viewport, "glViewport");
  get(gl.Scissor, "glScissor");
  get(gl.ColorMask, "glColorMask");
  get(gl.UseProgram, "glUseProgram");
  get(gl.BindBuffer, "glBindBuffer");
  get(gl.BindVertexArray, "glBindVertexArray");
  get(gl.GenVertexArrays, "glGenVertexArrays");
  get(gl.BindTexture, "glBindTexture");
  get(gl.ActiveTexture, "glActiveTexture");
  get(gl.BindSampler, "glBindSampler");
  get(gl.BindFramebuffer, "glBindFramebuffer");
  get(gl.PixelStorei, "glPixelStorei");
  get(gl.GenTextures, "glGenTextures");
  get(gl.TexImage2D, "glTexImage2D");
  get(gl.CopyTexImage2D, "glCopyTexImage2D");
  get(gl.GetError, "glGetError");
  get(gl.Uniform1f, "glUniform1f");
  get(gl.TexParameteri, "glTexParameteri");
  get(gl.CreateShader, "glCreateShader");
  get(gl.ShaderSource, "glShaderSource");
  get(gl.CompileShader, "glCompileShader");
  get(gl.CreateProgram, "glCreateProgram");
  get(gl.AttachShader, "glAttachShader");
  get(gl.BindAttribLocation, "glBindAttribLocation");
  get(gl.LinkProgram, "glLinkProgram");
  get(gl.GetProgramiv, "glGetProgramiv");
  get(gl.GetUniformLocation, "glGetUniformLocation");
  get(gl.Uniform1i, "glUniform1i");
  get(gl.Uniform4f, "glUniform4f");
  get(gl.Uniform2f, "glUniform2f");
  get(gl.GenBuffers, "glGenBuffers");
  get(gl.BufferData, "glBufferData");
  get(gl.VertexAttribPointer, "glVertexAttribPointer");
  get(gl.EnableVertexAttribArray, "glEnableVertexAttribArray");
  get(gl.DrawArrays, "glDrawArrays");
  get(gl.GetString, "glGetString");
  get(gl.GetVertexAttribiv, "glGetVertexAttribiv");
  get(gl.GetVertexAttribPointerv, "glGetVertexAttribPointerv");
  get(gl.DisableVertexAttribArray, "glDisableVertexAttribArray");
  get(gl.ReadPixels, "glReadPixels");
  return ok;
}

// --- fonts ----------------------------------------------------------------------

FT_Library g_ft = nullptr;
std::mutex g_ft_mu;  // FreeType is used from the render thread and the window thread
std::map<std::string, FT_Face> g_faces;
std::map<int, std::pair<std::string, float>> g_mapping;  // font enum -> (file, ratio)
bool g_mapping_loaded = false;

void load_mapping() {
  g_mapping_loaded = true;
  std::ifstream f(asset_root() + "/android/fonts/font-mappings.json");
  std::stringstream ss;
  ss << f.rdbuf();
  const std::string s = ss.str();
  size_t pos = 0;
  while ((pos = s.find('{', pos)) != std::string::npos) {
    const size_t end = s.find('}', pos);
    if (end == std::string::npos) break;
    const std::string obj = s.substr(pos, end - pos);
    pos = end;
    auto field = [&](const char* key) -> std::string {
      const size_t k = obj.find(std::string("\"") + key + "\"");
      if (k == std::string::npos) return {};
      size_t v = obj.find(':', k);
      if (v == std::string::npos) return {};
      ++v;
      while (v < obj.size() && (obj[v] == ' ' || obj[v] == '\n' || obj[v] == '\t' || obj[v] == '\r')) ++v;
      if (v < obj.size() && obj[v] == '"') {
        const size_t q = obj.find('"', v + 1);
        return obj.substr(v + 1, q - v - 1);
      }
      size_t e = v;
      while (e < obj.size() && obj[e] != ',' && obj[e] != '\n') ++e;
      return obj.substr(v, e - v);
    };
    const std::string en = field("enum"), font = field("font"), ratio = field("fromRbxFontRatio");
    if (en.empty() || font.empty()) continue;
    g_mapping[atoi(en.c_str())] = {font, ratio.empty() ? 0.793f : static_cast<float>(atof(ratio.c_str()))};
  }
  LOG("%zu font mappings\n", g_mapping.size());
}

FT_Face open_face(const std::string& path) {
  auto it = g_faces.find(path);
  if (it != g_faces.end()) return it->second;
  FT_Face face = nullptr;
  if (FT_New_Face(g_ft, path.c_str(), 0, &face) != 0) face = nullptr;
  g_faces[path] = face;
  return face;
}

// RbxKeyboard.m / eh.e: the face and the pixel size for FONT at FONT_SIZE.
FT_Face pick_face(int font, float font_size, float& px) {
  if (!g_mapping_loaded) load_mapping();
  auto m = g_mapping.find(font);
  if (m != g_mapping.end()) {
    if (FT_Face f = open_face(asset_root() + "/content/fonts/" + m->second.first)) {
      px = font_size * m->second.second;
      return f;
    }
  }
  const char* file = font == 4 ? "SourceSansPro-Bold.ttf" : font == 5 ? "SourceSansPro-Light.ttf" : "SourceSansPro-Regular.ttf";
  px = font_size * 0.795f;
  if (FT_Face f = open_face(asset_root() + "/fonts/" + file)) return f;
  for (const char* p : {"/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/share/fonts/noto/NotoSans-Regular.ttf",
                        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
                        "/usr/local/share/fonts/dejavu/DejaVuSans.ttf", "/usr/local/share/fonts/noto/NotoSans-Regular.ttf"})
    if (FT_Face f = open_face(p)) return f;
  return nullptr;
}

std::vector<uint32_t> decode_utf8(const std::string& s) {
  std::vector<uint32_t> out;
  for (size_t i = 0; i < s.size();) {
    const unsigned char c = s[i];
    int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
    uint32_t cp = n == 1 ? c : n == 2 ? (c & 0x1F) : n == 3 ? (c & 0x0F) : (c & 0x07);
    for (int k = 1; k < n && i + k < s.size(); ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
    out.push_back(cp);
    i += n;
  }
  return out;
}

// --- the rendered line ----------------------------------------------------------

struct Line {
  std::vector<uint8_t> rgba;  // white, coverage in alpha
  int w = 0, h = 0;
  int ascent = 0;
  int caret_x = 0;  // pixels from the left of the bitmap
};

Line render_line(FT_Face face, float px, const std::vector<uint32_t>& text) {
  Line ln;
  FT_Set_Pixel_Sizes(face, 0, static_cast<unsigned>(std::max(1.f, std::round(px))));
  const int asc = static_cast<int>(face->size->metrics.ascender >> 6);
  const int desc = static_cast<int>(-(face->size->metrics.descender >> 6));
  ln.ascent = asc;
  ln.h = std::max(1, asc + desc);
  int width = 0;
  for (uint32_t cp : text)
    if (FT_Load_Char(face, cp, FT_LOAD_DEFAULT) == 0) width += static_cast<int>(face->glyph->advance.x >> 6);
  ln.w = std::max(1, width + 2);
  ln.rgba.assign(static_cast<size_t>(ln.w) * ln.h * 4, 0);
  for (size_t i = 0; i < ln.rgba.size(); i += 4) ln.rgba[i] = ln.rgba[i + 1] = ln.rgba[i + 2] = 255;
  int pen = 0;
  for (uint32_t cp : text) {
    if (FT_Load_Char(face, cp, FT_LOAD_RENDER) != 0) continue;
    const FT_GlyphSlot g = face->glyph;
    const FT_Bitmap& bm = g->bitmap;
    for (unsigned r = 0; r < bm.rows; ++r)
      for (unsigned c = 0; c < bm.width; ++c) {
        const int x = pen + g->bitmap_left + static_cast<int>(c), y = asc - g->bitmap_top + static_cast<int>(r);
        if (x < 0 || y < 0 || x >= ln.w || y >= ln.h) continue;
        uint8_t& a = ln.rgba[(static_cast<size_t>(y) * ln.w + x) * 4 + 3];
        a = std::max<uint8_t>(a, bm.buffer[r * bm.pitch + c]);
      }
    pen += static_cast<int>(g->advance.x >> 6);
  }
  ln.caret_x = pen;
  return ln;
}

// --- drawing ----------------------------------------------------------------------

bool g_ready = false, g_failed = false;
bool g_es3 = false;  // vertex array objects (else attributes 0 and 1 are saved and restored)
GLuint g_prog = 0, g_vao = 0, g_vbo = 0, g_tex = 0, g_white = 0;
GLint g_u_tex = -1, g_u_color = -1, g_u_size = -1, g_u_image = -1;
GLint g_u_filter = -1, g_u_sharp = -1, g_u_texel = -1;  // picture filters (mod panel)
GLuint g_frame_tex = 0;  // resolution cap: the game's frame, copied to scale it up
GLuint g_close_tex = 0;  // the closing screen (closescreen.hpp)
long g_close_gen = -1;
int g_frame_w = 0, g_frame_h = 0;
std::string g_drawn_text;
long g_drawn_gen = -1;
Line g_line;
float g_px = 0;

bool init_gl() {
  if (!load_gl()) {
    LOG("GLES functions missing: typed text is not drawn\n");
    return false;
  }
  bool ft_ok;
  {
    std::lock_guard<std::mutex> l(g_ft_mu);
    ft_ok = g_ft || FT_Init_FreeType(&g_ft) == 0;
  }
  if (!ft_ok) {
    LOG("FreeType failed: typed text is not drawn\n");
    return false;
  }
  static const char* vs =
      "#version 100\nattribute vec2 a_pos; attribute vec2 a_uv; uniform vec2 u_size; varying vec2 v_uv;\n"
      "void main() { v_uv = a_uv; gl_Position = vec4(a_pos.x / u_size.x * 2.0 - 1.0, 1.0 - a_pos.y / u_size.y * 2.0, 0.0, 1.0); }\n";
  // The game's frame (u_image) goes through the picture filter and sharpening
  // from the mod panel; text is drawn in u_color.
  static const char* fs =
      "#version 100\nprecision mediump float; uniform sampler2D u_tex; uniform vec4 u_color; varying vec2 v_uv;\n"
      "uniform float u_image; uniform float u_filter; uniform float u_sharp; uniform vec2 u_texel;\n"
      "vec3 filt(vec3 c) {\n"
      "  float l = dot(c, vec3(0.299, 0.587, 0.114));\n"
      "  if (u_filter < 0.5) return c;\n"
      "  if (u_filter < 1.5) return clamp(mix(vec3(l), c, 1.45) * 1.06 - 0.03, 0.0, 1.0);\n"  // vivid
      "  if (u_filter < 2.5) return vec3(l);\n"                                                // black and white
      "  if (u_filter < 3.5) return clamp(c * vec3(1.08, 1.0, 0.86), 0.0, 1.0);\n"             // warm
      "  if (u_filter < 4.5) return clamp(c * vec3(0.88, 1.0, 1.12), 0.0, 1.0);\n"             // cool
      "  if (u_filter < 5.5) return c * vec3(1.0, 0.82, 0.55) * 0.85;\n"                       // night light
      "  if (u_filter < 6.5) return clamp((c - 0.5) * 1.35 + 0.5, 0.0, 1.0);\n"                // high contrast
      "  if (u_filter < 7.5) return floor(c * 5.0 + 0.5) / 5.0;\n"                             // retro
      "  return vec3(l) * vec3(1.07, 0.95, 0.78);\n"                                           // sepia
      "}\n"
      "void main() { vec4 t = texture2D(u_tex, v_uv);\n"
      "  if (u_image < 0.5) { gl_FragColor = vec4(u_color.rgb, u_color.a * t.a); return; }\n"
      "  if (u_image > 1.5) { gl_FragColor = t; return; }\n"
      "  vec3 c = t.rgb;\n"
      "  if (u_sharp > 0.0) {\n"
      "    vec3 n = texture2D(u_tex, v_uv + vec2(u_texel.x, 0.0)).rgb + texture2D(u_tex, v_uv - vec2(u_texel.x, 0.0)).rgb\n"
      "           + texture2D(u_tex, v_uv + vec2(0.0, u_texel.y)).rgb + texture2D(u_tex, v_uv - vec2(0.0, u_texel.y)).rgb;\n"
      "    c = clamp(c + (c - n * 0.25) * u_sharp, 0.0, 1.0);\n"
      "  }\n"
      "  gl_FragColor = vec4(filt(c), 1.0); }\n";
  const GLuint v = gl.CreateShader(kVertexShader), f = gl.CreateShader(kFragmentShader);
  gl.ShaderSource(v, 1, &vs, nullptr);
  gl.CompileShader(v);
  gl.ShaderSource(f, 1, &fs, nullptr);
  gl.CompileShader(f);
  g_prog = gl.CreateProgram();
  gl.AttachShader(g_prog, v);
  gl.AttachShader(g_prog, f);
  gl.BindAttribLocation(g_prog, 0, "a_pos");
  gl.BindAttribLocation(g_prog, 1, "a_uv");
  gl.LinkProgram(g_prog);
  GLint linked = 0;
  gl.GetProgramiv(g_prog, 0x8B82 /* GL_LINK_STATUS */, &linked);
  if (!linked) {
    LOG("overlay shader did not link: typed text is not drawn\n");
    return false;
  }
  const char* ver = reinterpret_cast<const char*>(gl.GetString(0x1F02 /* GL_VERSION */));
  g_es3 = ver && strstr(ver, "OpenGL ES ") && atoi(ver + 10) >= 3;
  g_u_tex = gl.GetUniformLocation(g_prog, "u_tex");
  g_u_color = gl.GetUniformLocation(g_prog, "u_color");
  g_u_size = gl.GetUniformLocation(g_prog, "u_size");
  g_u_image = gl.GetUniformLocation(g_prog, "u_image");
  g_u_filter = gl.GetUniformLocation(g_prog, "u_filter");
  g_u_sharp = gl.GetUniformLocation(g_prog, "u_sharp");
  g_u_texel = gl.GetUniformLocation(g_prog, "u_texel");
  gl.GenBuffers(1, &g_vbo);
  if (g_es3) {
    GLint old = 0;
    gl.GetIntegerv(kVertexArrayBinding, &old);
    gl.GenVertexArrays(1, &g_vao);
    gl.BindVertexArray(g_vao);
    gl.BindBuffer(kArrayBuffer, g_vbo);
    gl.EnableVertexAttribArray(0);
    gl.EnableVertexAttribArray(1);
    gl.VertexAttribPointer(0, 2, kFloat, 0, 16, reinterpret_cast<const void*>(0));
    gl.VertexAttribPointer(1, 2, kFloat, 0, 16, reinterpret_cast<const void*>(8));
    gl.BindVertexArray(old);
  }
  GLuint t[2];
  gl.GenTextures(2, t);
  g_tex = t[0];
  g_white = t[1];
  const uint8_t white[4] = {255, 255, 255, 255};
  gl.BindTexture(kTexture2D, g_white);
  gl.TexImage2D(kTexture2D, 0, kRGBA, 1, 1, 0, kRGBA, kUnsignedByte, white);
  for (GLuint id : {g_tex, g_white}) {
    gl.BindTexture(kTexture2D, id);
    gl.TexParameteri(kTexture2D, kTexMinFilter, kLinear);
    gl.TexParameteri(kTexture2D, kTexMagFilter, kLinear);
    gl.TexParameteri(kTexture2D, kTexWrapS, kClampToEdge);
    gl.TexParameteri(kTexture2D, kTexWrapT, kClampToEdge);
  }
  return true;
}

void quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1) {
  const float d[16] = {x0, y0, u0, v0, x1, y0, u1, v0, x0, y1, u0, v1, x1, y1, u1, v1};
  gl.BufferData(kArrayBuffer, sizeof d, d, kDynamicDraw);
  gl.DrawArrays(kTriangleStrip, 0, 4);
}

}  // namespace

void textoverlay_set_info(const std::vector<uint64_t>& a) {
  // (FFFFF Z IIIIII ZZZ [FFFF]): x, y, width, height, fontSize, multiline,
  // xAlignment, yAlignment, textColor, font, textInputType, returnKeyType,
  // manualFocusRelease, textWrapped, editable, paddings left/top/right/bottom.
  if (a.size() < 15) return;
  Info i;
  i.x = f_of(a[0]), i.y = f_of(a[1]), i.w = f_of(a[2]), i.h = f_of(a[3]), i.font_size = f_of(a[4]);
  i.multiline = a[5] & 0xff;
  i.x_align = static_cast<int>(a[6]), i.y_align = static_cast<int>(a[7]);
  i.color = static_cast<uint32_t>(a[8]);
  i.font = static_cast<int>(a[9]), i.input_type = static_cast<int>(a[10]);
  if (a.size() >= 19) i.pad_l = f_of(a[15]), i.pad_t = f_of(a[16]), i.pad_r = f_of(a[17]), i.pad_b = f_of(a[18]);
  // In density units, as on Android (Roblox's app multiplies them by the
  // display density, wh/p0.l): pixels here, so they follow the UI size.
  const float d = device_dpi_scale();
  i.x *= d, i.y *= d, i.w *= d, i.h *= d, i.font_size *= d;
  i.pad_l *= d, i.pad_t *= d, i.pad_r *= d, i.pad_b *= d;
  i.valid = true;
  std::lock_guard<std::mutex> l(g_mu);
  g_info = i;
  ++g_info_gen;
  LOG("box at %.0f,%.0f size %.0fx%.0f, font %d size %.0f, input type %d\n", i.x, i.y, i.w, i.h, i.font, i.font_size,
      i.input_type);
}

namespace {

// --- the engine's GL state, saved around our drawing ----------------------------

struct Attr { GLint enabled, buffer, size, type, norm, stride; void* ptr; };
struct Saved {
  GLint prog, abuf, vao = 0, active, tex, sampler = 0, vp[4], sc[4], fbo, rfbo = 0, bsr, bdr, bsa, bda, ber, bea, ua, url = 0,
      upb = 0, usr = 0, usp = 0;
  unsigned char cm[4];
  bool blend, depth, cull, scissor, stencil, discard;
  Attr attr[2] = {};
};

void save(Saved& st) {
  gl.GetIntegerv(kCurrentProgram, &st.prog);
  gl.GetIntegerv(kArrayBufferBinding, &st.abuf);
  if (g_es3) gl.GetIntegerv(kVertexArrayBinding, &st.vao);
  gl.GetIntegerv(kActiveTexture, &st.active);
  gl.ActiveTexture(kTexture0);
  gl.GetIntegerv(kTextureBinding2D, &st.tex);
  if (gl.BindSampler) gl.GetIntegerv(kSamplerBinding, &st.sampler);
  gl.GetIntegerv(kViewport, st.vp);
  gl.GetIntegerv(kScissorBox, st.sc);
  gl.GetIntegerv(kDrawFramebufferBinding, &st.fbo);
  gl.GetIntegerv(0x8CAA, &st.rfbo);  // GL_READ_FRAMEBUFFER_BINDING
  gl.GetIntegerv(kBlendSrcRgb, &st.bsr);
  gl.GetIntegerv(kBlendDstRgb, &st.bdr);
  gl.GetIntegerv(kBlendSrcAlpha, &st.bsa);
  gl.GetIntegerv(kBlendDstAlpha, &st.bda);
  gl.GetIntegerv(kBlendEqRgb, &st.ber);
  gl.GetIntegerv(kBlendEqAlpha, &st.bea);
  gl.GetBooleanv(kColorWritemask, st.cm);
  gl.GetIntegerv(kUnpackAlignment, &st.ua);
  gl.GetIntegerv(kUnpackRowLength, &st.url);
  gl.GetIntegerv(kUnpackSkipRows, &st.usr);
  gl.GetIntegerv(kUnpackSkipPixels, &st.usp);
  gl.GetIntegerv(kPixelUnpackBufferBinding, &st.upb);
  st.blend = gl.IsEnabled(kBlend), st.depth = gl.IsEnabled(kDepthTest), st.cull = gl.IsEnabled(kCullFace),
  st.scissor = gl.IsEnabled(kScissorTest), st.stencil = gl.IsEnabled(kStencilTest),
  st.discard = gl.IsEnabled(kRasterizerDiscard);
  if (!g_es3)
    for (GLuint i = 0; i < 2; ++i) {
      Attr& a = st.attr[i];
      gl.GetVertexAttribiv(i, 0x8622, &a.enabled);  // GL_VERTEX_ATTRIB_ARRAY_ENABLED
      gl.GetVertexAttribiv(i, 0x889F, &a.buffer);   // ..._BUFFER_BINDING
      gl.GetVertexAttribiv(i, 0x8623, &a.size);
      gl.GetVertexAttribiv(i, 0x8625, &a.type);
      gl.GetVertexAttribiv(i, 0x886A, &a.norm);
      gl.GetVertexAttribiv(i, 0x8624, &a.stride);
      gl.GetVertexAttribPointerv(i, 0x8645, &a.ptr);  // GL_VERTEX_ATTRIB_ARRAY_POINTER
    }
  gl.PixelStorei(kUnpackAlignment, 1);
  gl.PixelStorei(kUnpackRowLength, 0);
  gl.PixelStorei(kUnpackSkipRows, 0);
  gl.PixelStorei(kUnpackSkipPixels, 0);
  gl.BindBuffer(kPixelUnpackBuffer, 0);
}

// Our drawing setup: the window's framebuffer, alpha blending, our program.
void begin(int sw, int sh) {
  gl.BindFramebuffer(kDrawFramebuffer, 0);
  gl.Viewport(0, 0, sw, sh);
  gl.Disable(kDepthTest);
  gl.Disable(kCullFace);
  gl.Disable(kStencilTest);
  gl.Disable(kRasterizerDiscard);
  gl.Disable(kScissorTest);
  gl.Enable(kBlend);
  gl.BlendEquationSeparate(kFuncAdd, kFuncAdd);
  gl.BlendFuncSeparate(kSrcAlpha, kOneMinusSrcAlpha, kOne, kOneMinusSrcAlpha);
  gl.ColorMask(1, 1, 1, 1);
  if (gl.BindSampler) gl.BindSampler(0, 0);
  gl.UseProgram(g_prog);
  if (g_es3) {
    gl.BindVertexArray(g_vao);
    gl.BindBuffer(kArrayBuffer, g_vbo);
  } else {
    gl.BindBuffer(kArrayBuffer, g_vbo);
    gl.EnableVertexAttribArray(0);
    gl.EnableVertexAttribArray(1);
    gl.VertexAttribPointer(0, 2, kFloat, 0, 16, reinterpret_cast<const void*>(0));
    gl.VertexAttribPointer(1, 2, kFloat, 0, 16, reinterpret_cast<const void*>(8));
  }
  gl.Uniform1i(g_u_tex, 0);
  gl.Uniform2f(g_u_size, static_cast<float>(sw), static_cast<float>(sh));
  if (gl.Uniform1f) gl.Uniform1f(g_u_image, 0);
}

void restore(const Saved& st) {
  if (!g_es3)
    for (GLuint i = 0; i < 2; ++i) {
      const Attr& a = st.attr[i];
      gl.BindBuffer(kArrayBuffer, a.buffer);
      gl.VertexAttribPointer(i, a.size, a.type, a.norm, a.stride, a.ptr);
      (a.enabled ? gl.EnableVertexAttribArray : gl.DisableVertexAttribArray)(i);
    }
  gl.BindBuffer(kPixelUnpackBuffer, st.upb);
  gl.PixelStorei(kUnpackAlignment, st.ua);
  gl.PixelStorei(kUnpackRowLength, st.url);
  gl.PixelStorei(kUnpackSkipRows, st.usr);
  gl.PixelStorei(kUnpackSkipPixels, st.usp);
  gl.UseProgram(st.prog);
  if (g_es3) gl.BindVertexArray(st.vao);
  gl.BindBuffer(kArrayBuffer, st.abuf);
  gl.BindTexture(kTexture2D, st.tex);
  if (gl.BindSampler) gl.BindSampler(0, st.sampler);
  gl.ActiveTexture(st.active);
  gl.BindFramebuffer(kDrawFramebuffer, st.fbo);
  gl.BindFramebuffer(0x8CA8, st.rfbo);  // GL_READ_FRAMEBUFFER
  gl.Viewport(st.vp[0], st.vp[1], st.vp[2], st.vp[3]);
  gl.Scissor(st.sc[0], st.sc[1], st.sc[2], st.sc[3]);
  gl.BlendFuncSeparate(st.bsr, st.bdr, st.bsa, st.bda);
  gl.BlendEquationSeparate(st.ber, st.bea);
  gl.ColorMask(st.cm[0], st.cm[1], st.cm[2], st.cm[3]);
  (st.blend ? gl.Enable : gl.Disable)(kBlend);
  (st.depth ? gl.Enable : gl.Disable)(kDepthTest);
  (st.cull ? gl.Enable : gl.Disable)(kCullFace);
  (st.scissor ? gl.Enable : gl.Disable)(kScissorTest);
  (st.stencil ? gl.Enable : gl.Disable)(kStencilTest);
  (st.discard ? gl.Enable : gl.Disable)(kRasterizerDiscard);
}

void color(uint32_t rgb, float a) {
  gl.Uniform4f(g_u_color, ((rgb >> 16) & 0xff) / 255.f, ((rgb >> 8) & 0xff) / 255.f, (rgb & 0xff) / 255.f, a);
}
void rect(float x0, float y0, float x1, float y1) {
  gl.BindTexture(kTexture2D, g_white);
  quad(x0, y0, x1, y1, 0, 0, 1, 1);
}

bool ready_gl() {
  if (!g_ready) {
    g_ready = true;
    g_failed = !init_gl();
  }
  return !g_failed;
}

// --- the focused TextBox ------------------------------------------------------------

void draw_textbox(int, int sh, const std::string& text, const Info& info, long gen) {
  if (text != g_drawn_text || gen != g_drawn_gen) {
    g_drawn_text = text;
    g_drawn_gen = gen;
    float px = 0;
    FT_Face face = pick_face(info.font, info.font_size, px);
    std::vector<uint32_t> cps = decode_utf8(text);
    // TYPE_TEXT_VARIATION_PASSWORD for these input types (RbxKeyboard.l).
    if (info.input_type == 5 || info.input_type == 9 || info.input_type == 10)
      for (uint32_t& c : cps) c = face && FT_Get_Char_Index(face, 0x2022) ? 0x2022 : '*';
    g_line = face ? render_line(face, px, cps) : Line{};
    g_px = px;
    if (face) {
      gl.BindTexture(kTexture2D, g_tex);
      gl.TexImage2D(kTexture2D, 0, kRGBA, g_line.w, g_line.h, 0, kRGBA, kUnsignedByte, g_line.rgba.data());
    }
  }
  if (g_line.rgba.empty()) return;
  const uint32_t c = info.color;
  color(c & 0xFFFFFF, (c >> 24) ? ((c >> 24) & 0xff) / 255.f : 1.f);

  // The content box (padding inside the TextBox), clipped like an EditText.
  const float cx = info.x + info.pad_l, cy = info.y + info.pad_t;
  const float cw = std::max(1.f, info.w - info.pad_l - info.pad_r), ch = std::max(1.f, info.h - info.pad_t - info.pad_b);
  gl.Enable(kScissorTest);
  gl.Scissor(static_cast<GLint>(cx), static_cast<GLint>(sh - (cy + ch)), static_cast<GLint>(cw) + 1,
             static_cast<GLint>(ch) + 1);
  const float caret_w = std::max(2.f, std::round(g_px / 12.f));
  const float lw = static_cast<float>(g_line.caret_x) + caret_w, lh = static_cast<float>(g_line.h);
  float tx;
  if (lw > cw) tx = cx + cw - lw;  // longer than the box: keep the end (and the caret) in view
  else if (info.x_align == 1) tx = cx + cw - lw;
  else if (info.x_align == 2) tx = cx + (cw - lw) / 2;
  else tx = cx;
  const float ty = info.y_align == 0 ? cy : info.y_align == 2 ? cy + ch - lh : cy + (ch - lh) / 2;
  tx = std::round(tx);
  const float tyr = std::round(ty);
  gl.BindTexture(kTexture2D, g_tex);
  quad(tx, tyr, tx + g_line.w, tyr + g_line.h, 0, 0, 1, 1);
  // A blinking caret after the text, as the EditText shows.
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now().time_since_epoch()).count();
  if ((ms / 500) % 2 == 0) {
    const float x0 = tx + g_line.caret_x;
    rect(x0, tyr + lh * 0.1f, x0 + caret_w, tyr + lh * 0.9f);
  }
  // TONIC_TEXTBOX_CHECK=1 (make test-window): count the box's pixels in the
  // text colour, to show the text was drawn.
  if (getenv("TONIC_TEXTBOX_CHECK")) {
    const int bx = static_cast<int>(cx), by = static_cast<int>(sh - (cy + ch)), bw = static_cast<int>(cw),
              bh = static_cast<int>(ch);
    std::vector<uint8_t> px(static_cast<size_t>(bw) * bh * 4);
    gl.ReadPixels(bx, by, bw, bh, kRGBA, kUnsignedByte, px.data());
    int lit = 0;
    for (size_t i = 0; i < px.size(); i += 4)
      if (std::abs(px[i] - static_cast<int>((c >> 16) & 0xff)) < 40 &&
          std::abs(px[i + 1] - static_cast<int>((c >> 8) & 0xff)) < 40 && std::abs(px[i + 2] - static_cast<int>(c & 0xff)) < 40)
        ++lit;
    LOG("check: %d of %d box pixels in the text colour\n", lit, bw * bh);
  }
  gl.Disable(kScissorTest);
}

// --- loading: is the game drawing yet? ----------------------------------------------

using Clock = std::chrono::steady_clock;
std::atomic<bool> g_ready_seen{false};
std::deque<Clock::time_point> g_recent;  // frames in the last second (render thread)

Clock::time_point g_first_frame{}, g_steady_since{}, g_prev_frame{};

// Called for every frame the game presents: it counts as ready once frames
// come at 30 or more a second (Roblox drawing its own UI, past the slow
// first frames of startup). On a machine that never gets there (software
// rendering, a slow GPU), also once the game has drawn without a pause for 5
// seconds, at least 20 seconds after its first frame; else the loading screen
// would stay up until its 2-minute timeout.
void count_frame() {
  if (g_ready_seen) return;
  const auto now = Clock::now();
  if (g_first_frame == Clock::time_point{}) g_first_frame = g_steady_since = now;
  if (now - g_prev_frame > std::chrono::milliseconds(1500)) g_steady_since = now;  // a pause: start again
  g_prev_frame = now;
  g_recent.push_back(now);
  while (!g_recent.empty() && now - g_recent.front() > std::chrono::seconds(1)) g_recent.pop_front();
  if (g_recent.size() >= 30) {
    g_ready_seen = true;
    LOG("loaded: the game draws at speed\n");
  } else if (now - g_first_frame >= std::chrono::seconds(20) && now - g_steady_since >= std::chrono::seconds(5)) {
    g_ready_seen = true;
    LOG("loaded: the game draws steadily, at %zu frames per second\n", g_recent.size());
  }
}

// --- the stats overlay (F9): built on the CPU; on Vulkan only the text and
// graph pixels are written, so nothing is read back from the GPU.

std::deque<Clock::time_point> g_frames_1s;  // presents in the last second
Clock::time_point g_last_frame{};
float g_worst_acc = 0;

FT_Face stats_face() {
  const std::string root = asset_root();
  for (const char* t : {"/content/fonts/BuilderSans-SemiBold.otf", "/content/fonts/BuilderSans-Medium.otf",
                        "/fonts/SourceSansPro-Semibold.ttf", "/fonts/SourceSansPro-Regular.ttf"})
    if (FT_Face f = open_face(root + t)) return f;
  for (const char* p : {"/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                        "/usr/local/share/fonts/dejavu/DejaVuSans.ttf", "/usr/local/share/fonts/noto/NotoSans-Regular.ttf"})
    if (FT_Face f = open_face(p)) return f;
  return nullptr;
}
FT_Face stats_bold_face() {
  const std::string root = asset_root();
  for (const char* t : {"/content/fonts/BuilderSans-ExtraBold.otf", "/content/fonts/BuilderSans-Bold.otf",
                        "/fonts/SourceSansPro-Bold.ttf", "/fonts/GothamSSm-Bold.otf"})
    if (FT_Face f = open_face(root + t)) return f;
  for (const char* p : {"/usr/share/fonts/TTF/DejaVuSans-Bold.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
                        "/usr/local/share/fonts/dejavu/DejaVuSans-Bold.ttf", "/usr/local/share/fonts/noto/NotoSans-Bold.ttf",
                        "/usr/share/fonts/noto/NotoSans-Bold.ttf"})
    if (FT_Face f = open_face(p)) return f;
  return stats_face();
}

std::atomic<int> g_stat_fps{0};
std::atomic<float> g_stat_worst{0};
Clock::time_point g_stat_at{};

// The line: the frame rate of each quarter second (newest at g_ft_i - 1), a
// new point four times a second, the last minute's worth.
constexpr int kGraphN = 240;
float g_fps_hist[kGraphN];
int g_ft_i = 0, g_ft_n = 0;
Clock::time_point g_hist_at{};
int g_q_frames = 0;  // frames since the last point

// The average in a game: from 20 s after it was launched (its loading left
// out), over the time tonic's window is in front (the background frame rate
// is yours, not the game's), without stalls over half a second (a teleport's
// loading). Starts again with each game.
int g_avg_launch = 0;
Clock::time_point g_avg_from{};
double g_avg_secs = 0;
long g_avg_frames = 0;

void count_stats() {
  const auto now = Clock::now();
  if (g_last_frame.time_since_epoch().count()) {
    const float ms = std::chrono::duration<float, std::milli>(now - g_last_frame).count();
    g_worst_acc = std::max(g_worst_acc, ms);
    const int launches = game_launches();
    if (launches != g_avg_launch) {
      g_avg_launch = launches;
      g_avg_from = now + std::chrono::seconds(20);
      g_avg_secs = 0, g_avg_frames = 0;
    }
    if (launches > 0 && now > g_avg_from && ms < 500 && window_focused()) {
      g_avg_secs += ms / 1000.0;
      ++g_avg_frames;
    }
  }
  g_last_frame = now;
  g_frames_1s.push_back(now);
  while (!g_frames_1s.empty() && now - g_frames_1s.front() > std::chrono::seconds(1)) g_frames_1s.pop_front();
  ++g_q_frames;
  if (!g_hist_at.time_since_epoch().count()) g_hist_at = now;
  if (now - g_hist_at >= std::chrono::milliseconds(250)) {  // four times a second: that quarter's frame rate
    const double secs = std::chrono::duration<double>(now - g_hist_at).count();
    g_fps_hist[g_ft_i] = static_cast<float>(g_q_frames / secs);
    g_ft_i = (g_ft_i + 1) % kGraphN;
    g_ft_n = std::min(g_ft_n + 1, kGraphN);
    g_q_frames = 0;
    g_hist_at = now;
  }
  if (now - g_stat_at > std::chrono::milliseconds(250)) {  // published 4 times a second
    g_stat_at = now;
    g_stat_fps = static_cast<int>(g_frames_1s.size());
    g_stat_worst = g_worst_acc;
    g_worst_acc = 0;
  }
}

// tonic's share of all the CPU cores, in percent, over the last half second or more.
float cpu_percent() {
  static Clock::time_point at{};
  static double cpu_at = 0;
  static float pct = 0;
  const auto now = Clock::now();
  if (now - at < std::chrono::milliseconds(500)) return pct;
  rusage ru{};
  getrusage(RUSAGE_SELF, &ru);
  const double cpu = ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6 + ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6;
  if (at.time_since_epoch().count()) {
    const double wall = std::chrono::duration<double>(now - at).count();
    const long cores = std::max(1L, sysconf(_SC_NPROCESSORS_ONLN));
    pct = static_cast<float>(std::clamp((cpu - cpu_at) / wall / cores * 100.0, 0.0, 100.0));
  }
  at = now, cpu_at = cpu;
  return pct;
}

// The picture: RGBA, straight alpha, top row first.
struct Panel {
  std::vector<uint8_t> rgba;
  int w = 0, h = 0;
  long gen = 0;
};
Panel g_panel;
Clock::time_point g_panel_at{};

void over(Panel& p, int x, int y, uint32_t rgb, float a) {
  if (a <= 0 || x < 0 || y < 0 || x >= p.w || y >= p.h) return;
  uint8_t* d = &p.rgba[(static_cast<size_t>(y) * p.w + x) * 4];
  a = std::min(a, 1.f);
  const float da = d[3] / 255.f, oa = a + da * (1 - a);
  if (oa <= 0) return;
  const float c[3] = {static_cast<float>((rgb >> 16) & 0xff), static_cast<float>((rgb >> 8) & 0xff), static_cast<float>(rgb & 0xff)};
  for (int i = 0; i < 3; ++i) d[i] = static_cast<uint8_t>((c[i] * a + d[i] * da * (1 - a)) / oa + 0.5f);
  d[3] = static_cast<uint8_t>(oa * 255 + 0.5f);
}
void put_text(Panel& p, const Line& ln, int x, int baseline, uint32_t rgb, float a = 1) {
  const int top = baseline - ln.ascent;
  for (int y = 0; y < ln.h; ++y)
    for (int x2 = 0; x2 < ln.w; ++x2)
      if (const uint8_t c = ln.rgba[(static_cast<size_t>(y) * ln.w + x2) * 4 + 3]) over(p, x + x2, top + y, rgb, a * c / 255.f);
}
Line text_line(FT_Face f, float px, const std::string& s) { return f ? render_line(f, px, decode_utf8(s)) : Line{}; }

// (Re)makes the picture, four times a second. FreeType's lock held by the caller.
bool build_panel(int sw, bool vulkan) {
  const auto now = Clock::now();
  if (!g_panel.rgba.empty() && now - g_panel_at < std::chrono::milliseconds(250)) return true;
  g_panel_at = now;
  if (!g_ft) FT_Init_FreeType(&g_ft);
  FT_Face bold = g_ft ? stats_bold_face() : nullptr, reg = g_ft ? stats_face() : nullptr;
  if (!bold || !reg) return false;
  const float px = std::max(13.f, sw / 90.f);

  // The line: (label, value, colour) pieces; values bold, labels in the same colour, dimmer.
  struct Piece { std::string label, value; uint32_t colour; };
  std::vector<Piece> pieces;
  char b1[32], b2[32];
  const int fps = g_stat_fps;
  snprintf(b1, sizeof b1, "%d", fps);
  pieces.push_back({"FPS", b1, fps >= 55 ? 0x6EE7A0u : fps >= 28 ? 0xF7D154u : 0xFF6B6Bu});
  snprintf(b1, sizeof b1, "%.1f", fps ? 1000.0 / fps : 0.0);
  pieces.push_back({"MS", b1, 0xE8EAF0});
  if (g_avg_frames > 30 && g_avg_secs > 2) snprintf(b1, sizeof b1, "%.0f", g_avg_frames / g_avg_secs);
  else snprintf(b1, sizeof b1, "--");
  pieces.push_back({"AVG", b1, 0x4FD1FF});
  const float pm = netping_ms();
  if (pm >= 0) snprintf(b2, sizeof b2, "%.0f ms", pm);
  else snprintf(b2, sizeof b2, "%s", pm == -2 ? "no reply" : pm == -3 ? "no ping" : "--");
  pieces.push_back({"PING", b2, 0xB98CFF});
  snprintf(b1, sizeof b1, "%.0f%%", cpu_percent());
  pieces.push_back({"CPU", b1, 0xFFB547});
  snprintf(b1, sizeof b1, "%.1f ms", g_stat_worst.load());
  pieces.push_back({"WORST", b1, 0xFF7A6B});
  pieces.push_back({"", vulkan ? (smooth_motion_active() ? "Vulkan (~2x shown: Smooth Motion)" : "Vulkan") : "OpenGL", 0xA6B05C});

  struct Placed { Line ln; int x; uint32_t colour; float alpha; };
  std::vector<Placed> placed;
  const int gap = static_cast<int>(px * 0.35f), space = static_cast<int>(px * 1.1f);
  int x = 0, asc = 0, h = 0;
  for (size_t i = 0; i < pieces.size(); ++i) {
    const Piece& pc = pieces[i];
    Line v = text_line(bold, px, pc.value);
    asc = std::max(asc, v.ascent), h = std::max(h, v.h);
    placed.push_back({v, x, pc.colour, 1.f});
    x += v.caret_x;
    if (!pc.label.empty()) {
      Line l = text_line(reg, px * 0.8f, pc.label);
      placed.push_back({l, x + gap, pc.colour, 0.75f});
      x += gap + l.caret_x;
    }
    if (i + 1 < pieces.size()) x += space;
  }
  const int pad = 8, graph_h = static_cast<int>(px * 2.2f);
  g_panel.w = x + 2 * pad + 2, g_panel.h = h + pad + graph_h + pad / 2;
  g_panel.rgba.assign(static_cast<size_t>(g_panel.w) * g_panel.h * 4, 0);
  if (!vulkan)  // the strip: square, black, 35 % (Vulkan: none)
    for (int y = 0; y < g_panel.h; ++y)
      for (int xx = 0; xx < g_panel.w; ++xx) over(g_panel, xx, y, 0x000000, 0.35f);
  const int baseline = pad / 2 + asc;
  for (const Placed& p : placed) put_text(g_panel, p.ln, pad + p.x, baseline, p.colour, p.alpha);

  // The frame rate of each quarter second: one line, newest on the right, a step
  // left each quarter second; green 55 and up, yellow from 28, red below.
  const int n = g_ft_n;
  if (n >= 2) {
    const float gx0 = static_cast<float>(pad), gx1 = static_cast<float>(g_panel.w - pad);
    const float gy0 = static_cast<float>(h + pad), gy1 = gy0 + graph_h;
    auto sample = [&](int k) { return g_fps_hist[(g_ft_i - n + k + kGraphN * 2) % kGraphN]; };
    float top = 60;  // frame rate at the top: at least 60, room above the highest
    for (int k = 0; k < n; ++k) top = std::max(top, sample(k) * 1.1f);
    auto xpos = [&](int k) { return gx0 + (gx1 - gx0) * (k + (kGraphN - n)) / (kGraphN - 1); };
    auto ypos = [&](float f) { return gy1 - 1 - std::min(f, top) / top * (gy1 - gy0 - 2); };
    const float rad = std::max(0.8f, px / 18.f);
    float x0 = xpos(0), y0 = ypos(sample(0));
    for (int k = 1; k < n; ++k) {
      const float f = sample(k), x1 = xpos(k), y1 = ypos(f);
      const float lower = std::min(f, sample(k - 1));
      const uint32_t col = lower >= 55 ? 0x6EE7A0 : lower >= 28 ? 0xF7D154 : 0xFF6B6B;
      const int steps = static_cast<int>(std::max(std::fabs(x1 - x0), std::fabs(y1 - y0)) * 2) + 1;
      for (int t = 0; t <= steps; ++t) {
        const float fx = x0 + (x1 - x0) * t / steps, fy = y0 + (y1 - y0) * t / steps;
        for (int oy = static_cast<int>(fy - rad - 1); oy <= static_cast<int>(fy + rad + 1); ++oy)
          for (int ox = static_cast<int>(fx - rad - 1); ox <= static_cast<int>(fx + rad + 1); ++ox) {
            const float cov = std::clamp(rad + 0.5f - std::hypot(ox + 0.5f - fx, oy + 0.5f - fy), 0.f, 1.f);
            if (cov > 0) over(g_panel, ox, oy, col, cov);
          }
      }
      x0 = x1, y0 = y1;
    }
  }
  if (vulkan) {
    // No strip behind it: a thin dark outline keeps the text and the line
    // readable over anything the game draws.
    const std::vector<uint8_t> src = g_panel.rgba;
    const int r = std::max(1, static_cast<int>(px / 14));
    auto alpha = [&](int x, int y) { return src[(static_cast<size_t>(y) * g_panel.w + x) * 4 + 3]; };
    for (int y = 0; y < g_panel.h; ++y)
      for (int x = 0; x < g_panel.w; ++x) {
        int near = 0;
        for (int dy = -r; dy <= r && near < 255; ++dy)
          for (int dx = -r; dx <= r; ++dx) {
            const int nx = x + dx, ny = y + dy;
            if (nx >= 0 && ny >= 0 && nx < g_panel.w && ny < g_panel.h) near = std::max<int>(near, alpha(nx, ny));
          }
        if (!near) continue;
        uint8_t* d = &g_panel.rgba[(static_cast<size_t>(y) * g_panel.w + x) * 4];
        const float a = d[3] / 255.f, o = 0.85f * near / 255.f;  // the text over a dark rim
        const float oa = a + o * (1 - a);
        if (oa <= 0) continue;
        for (int i = 0; i < 3; ++i) d[i] = static_cast<uint8_t>(d[i] * a / oa + 0.5f);
        d[3] = static_cast<uint8_t>(oa * 255 + 0.5f);
      }
  }
  ++g_panel.gen;
  return true;
}

GLuint g_stats_tex = 0;
long g_stats_tex_gen = -1;

void draw_stats(int sw, int sh) {
  (void)sh;
  {
    std::lock_guard<std::mutex> l(g_ft_mu);
    if (!build_panel(sw, false)) return;
  }
  if (!g_stats_tex) {
    gl.GenTextures(1, &g_stats_tex);
    gl.BindTexture(kTexture2D, g_stats_tex);
    gl.TexParameteri(kTexture2D, kTexMinFilter, kLinear);
    gl.TexParameteri(kTexture2D, kTexMagFilter, kLinear);
    gl.TexParameteri(kTexture2D, kTexWrapS, kClampToEdge);
    gl.TexParameteri(kTexture2D, kTexWrapT, kClampToEdge);
    g_stats_tex_gen = -1;
  }
  gl.BindTexture(kTexture2D, g_stats_tex);
  if (g_stats_tex_gen != g_panel.gen) {
    g_stats_tex_gen = g_panel.gen;
    gl.TexImage2D(kTexture2D, 0, kRGBA, g_panel.w, g_panel.h, 0, kRGBA, kUnsignedByte, g_panel.rgba.data());
  }
  if (gl.Uniform1f) gl.Uniform1f(g_u_image, 2);  // the picture as it is, its own colours and alpha
  color(0xFFFFFF, 1);
  const float x = 12, y = 12;
  quad(x, y, x + g_panel.w, y + g_panel.h, 0, 0, 1, 1);
  if (gl.Uniform1f) gl.Uniform1f(g_u_image, 0);
}

}  // namespace

// GL objects belong to the context that made them (vertex arrays are never
// shared): Roblox draws a game in a different context from its app screens,
// so everything is made again when the current context changes.
void follow_context() {
  static auto current = reinterpret_cast<void* (*)()>(dlsym(dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL), "eglGetCurrentContext"));
  static void* last = nullptr;
  void* ctx = current ? current() : nullptr;
  if (ctx == last) return;
  if (last) LOG("drawing context changed: overlay set up again\n");
  last = ctx;
  g_ready = g_failed = false;
  g_stats_tex = 0;
  g_frame_tex = 0;
  g_frame_w = g_frame_h = 0;
  g_close_tex = 0;
  g_close_gen = -1;
  g_drawn_text.clear();
  g_drawn_gen = -1;
}

namespace {
// The picture filter and sharpening (config mod_filter, mod_sharpen 0-100),
// read 5 times a second.
std::atomic<int> g_filter{0}, g_sharp{0};
void postfx_read() {
  static std::atomic<long long> checked{0};
  const long long now = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
  if (now - checked < 200) return;
  checked = now;
  g_filter = std::clamp(config_int("mod_filter", 0), 0, 8);
  g_sharp = std::clamp(config_int("mod_sharpen", 0), 0, 100);
}
void postfx(int& filter, float& sharp) {
  postfx_read();
  filter = g_filter;
  sharp = g_sharp / 100.f * 1.5f;
}
}  // namespace

bool textoverlay_postfx_on() {
  postfx_read();
  return g_filter > 0 || g_sharp > 0;
}

bool textoverlay_read_frame(int w, int h, std::vector<uint8_t>& rgba) {
  if (w <= 0 || h <= 0) return false;
  follow_context();
  if (!ready_gl() || !gl.ReadPixels) return false;
  GLint fbo = 0, pack = 4;
  gl.GetIntegerv(0x8CAA /* GL_READ_FRAMEBUFFER_BINDING */, &fbo);
  gl.GetIntegerv(0x0D05 /* GL_PACK_ALIGNMENT */, &pack);
  GLint pbo = 0;
  gl.GetIntegerv(0x88ED /* GL_PIXEL_PACK_BUFFER_BINDING */, &pbo);
  if (pbo) gl.BindBuffer(0x88EB /* GL_PIXEL_PACK_BUFFER */, 0);
  gl.BindFramebuffer(0x8CA8, 0);
  gl.PixelStorei(0x0D05, 1);
  while (gl.GetError && gl.GetError()) {}
  rgba.assign(static_cast<size_t>(w) * h * 4, 0);
  gl.ReadPixels(0, 0, w, h, 0x1908 /* GL_RGBA */, 0x1401 /* GL_UNSIGNED_BYTE */, rgba.data());
  const bool ok = !gl.GetError || gl.GetError() == 0;
  gl.PixelStorei(0x0D05, pack);
  gl.BindFramebuffer(0x8CA8, fbo);
  if (pbo) gl.BindBuffer(0x88EB, pbo);
  return ok;
}

bool textoverlay_upscale(int rw, int rh, int ww, int wh) {
  if (rw <= 0 || rh <= 0 || ww <= 0 || wh <= 0) return true;
  follow_context();
  if (!ready_gl() || !gl.CopyTexImage2D || !gl.GetError) return false;  // (GL is looked up by ready_gl)
  Saved st;
  save(st);
  begin(ww, wh);
  gl.Disable(kBlend);
  while (gl.GetError()) {}  // the game's own errors are not ours
  // The game drew its frame in the bottom-left RW x RH of the window's
  // framebuffer: copy it out (in whatever format the window has: an unsized
  // RGB copy works from RGB, RGBA and 10-bit windows alike), then draw it over
  // the whole window.
  gl.BindFramebuffer(0x8CA8, 0);  // GL_READ_FRAMEBUFFER: the window
  if (!g_frame_tex) gl.GenTextures(1, &g_frame_tex);
  gl.BindTexture(kTexture2D, g_frame_tex);
  gl.CopyTexImage2D(kTexture2D, 0, 0x1907 /* GL_RGB */, 0, 0, rw, rh, 0);
  const unsigned err = gl.GetError();
  if (g_frame_w != rw || g_frame_h != rh) {
    gl.TexParameteri(kTexture2D, kTexMinFilter, kLinear);
    gl.TexParameteri(kTexture2D, kTexMagFilter, kLinear);
    gl.TexParameteri(kTexture2D, kTexWrapS, kClampToEdge);
    gl.TexParameteri(kTexture2D, kTexWrapT, kClampToEdge);
    g_frame_w = rw, g_frame_h = rh;
    LOG("resolution cap: scaling %dx%d up to %dx%d\n", rw, rh, ww, wh);
  }
  if (err) {
    // Never show a texture that was not filled: that is old GPU memory.
    LOG("resolution cap: copying the frame failed (GL error %#x); turning the cap off\n", err);
    restore(st);
    return false;
  }
  if (gl.Uniform1f) {
    int filter;
    float sharp;
    postfx(filter, sharp);
    gl.Uniform1f(g_u_image, 1);
    gl.Uniform1f(g_u_filter, static_cast<float>(filter));
    gl.Uniform1f(g_u_sharp, sharp);
  }
  gl.Uniform2f(g_u_texel, 1.f / rw, 1.f / rh);
  quad(0, 0, static_cast<float>(ww), static_cast<float>(wh), 0, 1, 1, 0);  // framebuffer rows run bottom up
  restore(st);
  return true;
}

void textoverlay_draw(int sw, int sh, float scale) {
  count_stats();
  count_frame();
  if (sw <= 0 || sh <= 0) return;
  // Closing: the closing screen over the whole window, nothing else.
  const std::vector<uint8_t>* close_img = nullptr;
  long close_gen = 0;
  if (!overlaywin_on() && closescreen_image(sw, sh, close_img, close_gen)) {
    follow_context();
    if (!ready_gl()) return;
    Saved st;
    save(st);
    begin(sw, sh);
    gl.Disable(kBlend);
    if (!g_close_tex) {
      gl.GenTextures(1, &g_close_tex);
      gl.BindTexture(kTexture2D, g_close_tex);
      gl.TexParameteri(kTexture2D, kTexMinFilter, kLinear);
      gl.TexParameteri(kTexture2D, kTexMagFilter, kLinear);
      gl.TexParameteri(kTexture2D, kTexWrapS, kClampToEdge);
      gl.TexParameteri(kTexture2D, kTexWrapT, kClampToEdge);
    }
    gl.BindTexture(kTexture2D, g_close_tex);
    if (close_gen != g_close_gen) {
      g_close_gen = close_gen;
      gl.TexImage2D(kTexture2D, 0, kRGBA, sw, sh, 0, kRGBA, kUnsignedByte, close_img->data());
    }
    if (gl.Uniform1f) {
      gl.Uniform1f(g_u_image, 1);
      gl.Uniform1f(g_u_filter, 0);
      gl.Uniform1f(g_u_sharp, 0);
    }
    quad(0, 0, static_cast<float>(sw), static_cast<float>(sh), 0, 0, 1, 1);
    restore(st);
    return;
  }
  std::string text;
  const bool typing = input_focused_text(text);
  Info info;
  long gen;
  {
    std::lock_guard<std::mutex> l(g_mu);
    info = g_info;
    gen = g_info_gen;
  }
  if (scale != 1.f) {  // the game's coordinates (resolution cap) -> the window's
    info.x *= scale, info.y *= scale, info.w *= scale, info.h *= scale, info.font_size *= scale;
    info.pad_l *= scale, info.pad_t *= scale, info.pad_r *= scale, info.pad_b *= scale;
    gen = gen * 1000 + static_cast<long>(scale * 100);  // re-render the text at the new size
  }
  const bool box = typing && info.valid && info.w > 0 && info.h > 0;
  static Clock::time_point checked{};
  static bool stats = false;
  if (Clock::now() - checked > std::chrono::milliseconds(200)) {  // the setting, read 5 times a second
    checked = Clock::now();
    const char* env = getenv("TONIC_STATS");
    stats = env && *env ? *env == '1' : config_bool("stats", false);
    netping_want(stats);  // (the game server is pinged only while the overlay shows)
  }
  if (!box && !stats) return;
  follow_context();
  if (!ready_gl()) return;
  Saved st;
  save(st);
  begin(sw, sh);
  if (box) {
    std::lock_guard<std::mutex> l(g_ft_mu);
    draw_textbox(sw, sh, text, info, gen);
  }
  if (stats) draw_stats(sw, sh);
  restore(st);
}

bool textoverlay_ready() { return g_ready_seen; }

// Any font fontconfig knows for PATTERN ("sans:bold", "monospace"): the last
// resort when none of the usual files exist (other systems, other font sets).
std::string fc_font(const char* pattern) {
  static std::map<std::string, std::string> cache;
  auto it = cache.find(pattern);
  if (it != cache.end()) return it->second;
  int st = -1;
  std::string path = spawn_read({"fc-match", "-f", "%{file}", pattern}, 5, &st);
  if (st != 0) path.clear();
  return cache[pattern] = path;
}

bool ui_text_bitmap(const char* text, int px, bool bold, std::vector<uint8_t>& rgba, int& w, int& h, int& advance,
                    bool mono) {
  std::lock_guard<std::mutex> l(g_ft_mu);
  if (!g_ft && FT_Init_FreeType(&g_ft) != 0) return false;
  const std::string root = asset_root();
  FT_Face f = nullptr;
  if (mono)
    for (const char* p : {"/usr/share/fonts/TTF/DejaVuSansMono.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
                          "/usr/share/fonts/liberation/LiberationMono-Regular.ttf", "/usr/share/fonts/noto/NotoSansMono-Regular.ttf",
                          "/usr/local/share/fonts/dejavu/DejaVuSansMono.ttf", "/usr/local/share/fonts/noto/NotoSansMono-Regular.ttf"})
      if (!f) f = open_face(p);
  for (const char* t : {bold ? "/content/fonts/BuilderSans-Bold.otf" : "/content/fonts/BuilderSans-Medium.otf",
                        bold ? "/fonts/SourceSansPro-Bold.ttf" : "/fonts/SourceSansPro-Regular.ttf"})
    if (!f) f = open_face(root + t);
  for (const char* p : {bold ? "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf" : "/usr/share/fonts/TTF/DejaVuSans.ttf",
                        bold ? "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
                             : "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                        "/usr/share/fonts/noto/NotoSans-Regular.ttf",
                        bold ? "/usr/local/share/fonts/dejavu/DejaVuSans-Bold.ttf" : "/usr/local/share/fonts/dejavu/DejaVuSans.ttf",
                        bold ? "/usr/local/share/fonts/noto/NotoSans-Bold.ttf" : "/usr/local/share/fonts/noto/NotoSans-Regular.ttf"})
    if (!f) f = open_face(p);
  if (!f && mono) f = open_face(fc_font("monospace"));
  if (!f) f = open_face(fc_font(bold ? "sans:bold" : "sans"));
  if (!f) return false;
  Line ln = render_line(f, static_cast<float>(px), decode_utf8(text));
  rgba = std::move(ln.rgba);
  w = ln.w, h = ln.h, advance = ln.caret_x;
  return true;
}

// --- Vulkan: the same overlays, drawn on the CPU into the presented image -------------

namespace {

bool stats_wanted() {
  static Clock::time_point checked{};
  static bool stats = false;
  if (Clock::now() - checked > std::chrono::milliseconds(200)) {
    checked = Clock::now();
    const char* env = getenv("TONIC_STATS");
    stats = env && *env ? *env == '1' : config_bool("stats", false);
    netping_want(stats);  // (the game server is pinged only while the overlay shows)
  }
  return stats;
}

// The focused TextBox for the CPU: its content box and the rendered line.
struct CpuBox {
  bool on = false;
  float cx = 0, cy = 0, cw = 0, ch = 0, tx = 0, ty = 0, caret_w = 2;
  uint32_t color = 0xFFFFFF;
  float alpha = 1;
};
std::mutex g_cpu_mu;
Line g_cpu_line;
std::string g_cpu_text;
long g_cpu_gen = -1;

CpuBox cpu_box(float scale) {
  CpuBox b;
  std::string text;
  if (!input_focused_text(text)) return b;
  Info info;
  long gen;
  {
    std::lock_guard<std::mutex> l(g_mu);
    info = g_info;
    gen = g_info_gen;
  }
  if (!info.valid || info.w <= 0 || info.h <= 0) return b;
  if (scale != 1.f) {  // the game's coordinates (resolution cap) -> the window's
    info.x *= scale, info.y *= scale, info.w *= scale, info.h *= scale, info.font_size *= scale;
    info.pad_l *= scale, info.pad_t *= scale, info.pad_r *= scale, info.pad_b *= scale;
    gen = gen * 1000 + static_cast<long>(scale * 100);
  }
  float px = 0;
  if (text != g_cpu_text || gen != g_cpu_gen) {
    std::lock_guard<std::mutex> l(g_ft_mu);
    if (!g_ft && FT_Init_FreeType(&g_ft) != 0) return b;
    g_cpu_text = text;
    g_cpu_gen = gen;
    FT_Face face = pick_face(info.font, info.font_size, px);
    std::vector<uint32_t> cps = decode_utf8(text);
    if (info.input_type == 5 || info.input_type == 9 || info.input_type == 10)
      for (uint32_t& c : cps) c = face && FT_Get_Char_Index(face, 0x2022) ? 0x2022 : '*';
    g_cpu_line = face ? render_line(face, px, cps) : Line{};
  }
  b.cx = info.x + info.pad_l, b.cy = info.y + info.pad_t;
  b.cw = std::max(1.f, info.w - info.pad_l - info.pad_r), b.ch = std::max(1.f, info.h - info.pad_t - info.pad_b);
  b.caret_w = std::max(2.f, std::round(info.font_size / 12.f));
  const float lw = static_cast<float>(g_cpu_line.caret_x) + b.caret_w, lh = static_cast<float>(g_cpu_line.h);
  if (lw > b.cw) b.tx = b.cx + b.cw - lw;
  else if (info.x_align == 1) b.tx = b.cx + b.cw - lw;
  else if (info.x_align == 2) b.tx = b.cx + (b.cw - lw) / 2;
  else b.tx = b.cx;
  b.ty = info.y_align == 0 ? b.cy : info.y_align == 2 ? b.cy + b.ch - lh : b.cy + (b.ch - lh) / 2;
  b.tx = std::round(b.tx), b.ty = std::round(b.ty);
  b.color = info.color & 0xFFFFFF;
  b.alpha = (info.color >> 24) ? ((info.color >> 24) & 0xff) / 255.f : 1.f;
  b.on = !g_cpu_line.rgba.empty();
  return b;
}

// Blends COLOR at coverage A (0-1) into pixel P (RGBA or BGRA bytes).
inline void blend_px(uint8_t* p, uint32_t color, float a, bool bgra) {
  if (a <= 0) return;
  const uint8_t r = (color >> 16) & 0xff, g = (color >> 8) & 0xff, bl = color & 0xff;
  uint8_t* R = bgra ? p + 2 : p;
  uint8_t* B = bgra ? p : p + 2;
  *R = static_cast<uint8_t>(*R + (r - *R) * a);
  p[1] = static_cast<uint8_t>(p[1] + (g - p[1]) * a);
  *B = static_cast<uint8_t>(*B + (bl - *B) * a);
}

}  // namespace

void textoverlay_presented() {
  count_stats();
  count_frame();
}

std::vector<OverlayRect> textoverlay_cpu_rects(int sw, int sh, float scale) {
  std::lock_guard<std::mutex> l(g_cpu_mu);
  std::vector<OverlayRect> out;
  if (closescreen_state() && !overlaywin_on()) return {{0, 0, sw, sh, false}};  // the closing screen covers it all
  auto clip = [&](int x, int y, int w, int h, bool blend) {
    const int x0 = std::max(0, x), y0 = std::max(0, y), x1 = std::min(sw, x + w), y1 = std::min(sh, y + h);
    if (x1 > x0 && y1 > y0) out.push_back({x0, y0, x1 - x0, y1 - y0, blend});
  };
  const CpuBox b = cpu_box(scale);
  if (b.on) clip(static_cast<int>(b.cx), static_cast<int>(b.cy), static_cast<int>(b.cw) + 1, static_cast<int>(b.ch) + 1, true);
  if (stats_wanted()) {
    // Only the text's and the line's pixels, as runs along each row: the game's
    // pixels around them are left as they are (nothing is read back).
    std::lock_guard<std::mutex> f(g_ft_mu);
    if (build_panel(sw, true)) {
      for (int y = 0; y < g_panel.h && out.size() < 8000; ++y) {
        int run = -1;
        for (int x = 0; x <= g_panel.w; ++x) {
          const bool on = x < g_panel.w && g_panel.rgba[(static_cast<size_t>(y) * g_panel.w + x) * 4 + 3] >= 64;
          if (on && run < 0) run = x;
          if (!on && run >= 0) {
            clip(12 + run, 12 + y, x - run, 1, false);
            run = -1;
          }
        }
      }
    }
  }
  return out;
}

void textoverlay_cpu_paint(const OverlayRect& r, uint8_t* px, int stride, bool bgra, float scale) {
  std::lock_guard<std::mutex> l(g_cpu_mu);
  {
    const std::vector<uint8_t>* img = nullptr;
    long gen;
    if (r.x == 0 && r.y == 0 && !overlaywin_on() && closescreen_image(r.w, r.h, img, gen)) {
      for (int y = 0; y < r.h; ++y) {
        const uint8_t* s = img->data() + static_cast<size_t>(y) * r.w * 4;
        uint8_t* d = px + y * stride;
        for (int x = 0; x < r.w; ++x, s += 4, d += 4) {
          d[0] = bgra ? s[2] : s[0], d[1] = s[1], d[2] = bgra ? s[0] : s[2], d[3] = 255;
        }
      }
      return;
    }
  }
  // Draws LINE's coverage at (LX, LY) in COLOR, clipped to the rect and to CLIP.
  auto draw_line = [&](const Line& ln, int lx, int ly, uint32_t color, float alpha, int cx0, int cy0, int cx1, int cy1) {
    for (int y = 0; y < ln.h; ++y) {
      const int wy = ly + y;
      if (wy < r.y || wy >= r.y + r.h || wy < cy0 || wy >= cy1) continue;
      for (int x = 0; x < ln.w; ++x) {
        const int wx = lx + x;
        if (wx < r.x || wx >= r.x + r.w || wx < cx0 || wx >= cx1) continue;
        const uint8_t a = ln.rgba[(static_cast<size_t>(y) * ln.w + x) * 4 + 3];
        if (a) blend_px(px + (wy - r.y) * stride + (wx - r.x) * 4, color, a / 255.f * alpha, bgra);
      }
    }
  };
  if (r.blend) {  // the TextBox
    const CpuBox b = cpu_box(scale);
    if (!b.on) return;
    const int cx0 = static_cast<int>(b.cx), cy0 = static_cast<int>(b.cy);
    const int cx1 = cx0 + static_cast<int>(b.cw) + 1, cy1 = cy0 + static_cast<int>(b.ch) + 1;
    draw_line(g_cpu_line, static_cast<int>(b.tx), static_cast<int>(b.ty), b.color, b.alpha, cx0, cy0, cx1, cy1);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
    if ((ms / 500) % 2 == 0) {  // the caret
      const int x0 = static_cast<int>(b.tx) + g_cpu_line.caret_x, lh = g_cpu_line.h;
      for (int y = static_cast<int>(b.ty + lh * 0.1f); y < static_cast<int>(b.ty + lh * 0.9f); ++y)
        for (int x = x0; x < x0 + static_cast<int>(b.caret_w); ++x)
          if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h && x >= cx0 && x < cx1 && y >= cy0 && y < cy1)
            blend_px(px + (y - r.y) * stride + (x - r.x) * 4, b.color, b.alpha, bgra);
    }
    if (getenv("TONIC_TEXTBOX_CHECK")) {  // make test-vulkan: the text reached the frame
      static int logged = 0;
      int lit = 0;
      for (int y = 0; y < r.h; ++y)
        for (int x = 0; x < r.w; ++x) {
          const uint8_t* p = px + y * stride + x * 4;
          if (p[0] > 200 && p[1] > 200 && p[2] > 200) ++lit;
        }
      if (logged++ < 3) LOG("check: %d of %d box pixels in the text colour (Vulkan)\n", lit, r.w * r.h);
    }
  } else {  // a run of the stats' text or line, or their dark outline
    for (int y = 0; y < r.h; ++y)
      for (int x = 0; x < r.w; ++x) {
        const int sx = r.x - 12 + x, sy = r.y - 12 + y;
        uint8_t* p = px + y * stride + x * 4;
        float c[3] = {0, 0, 0};
        if (sx >= 0 && sy >= 0 && sx < g_panel.w && sy < g_panel.h) {
          const uint8_t* q = &g_panel.rgba[(static_cast<size_t>(sy) * g_panel.w + sx) * 4];
          const float a = q[3] / 255.f;
          for (int i = 0; i < 3; ++i) c[i] = q[i] * a;  // (over black: the rim's own colour)
        }
        p[0] = static_cast<uint8_t>(bgra ? c[2] : c[0]), p[1] = static_cast<uint8_t>(c[1]), p[2] = static_cast<uint8_t>(bgra ? c[0] : c[2]);
        p[3] = 255;
      }
  }
}

}  // namespace tonic
