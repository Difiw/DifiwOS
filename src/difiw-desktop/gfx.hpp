// Difiw 2D software renderer: surfaces, alpha blending, rounded shapes, text, images.
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dg {

using u32 = std::uint32_t;  // 0xAARRGGBB, straight alpha

constexpr u32 rgba(int r, int g, int b, int a = 255) {
    return (static_cast<u32>(a) << 24) | (static_cast<u32>(r) << 16) | (static_cast<u32>(g) << 8) | static_cast<u32>(b);
}
inline int ca(u32 c) { return static_cast<int>(c >> 24); }
inline int cr(u32 c) { return static_cast<int>((c >> 16) & 0xff); }
inline int cg(u32 c) { return static_cast<int>((c >> 8) & 0xff); }
inline int cb(u32 c) { return static_cast<int>(c & 0xff); }
inline u32 with_alpha(u32 c, int a) { return (c & 0x00ffffffu) | (static_cast<u32>(a) << 24); }
u32 mix(u32 a, u32 b, float t);  // linear mix incl. alpha

struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
    int right() const { return x + w; }
    int bottom() const { return y + h; }
    bool has(int px, int py) const { return px >= x && py >= y && px < x + w && py < y + h; }
    bool empty() const { return w <= 0 || h <= 0; }
    Rect inset(int d) const { return {x + d, y + d, w - 2 * d, h - 2 * d}; }
    Rect intersect(const Rect& o) const;
};

struct Image {
    int w = 0, h = 0;
    std::vector<u32> px;
    bool ok() const { return w > 0 && h > 0 && px.size() == static_cast<std::size_t>(w) * h; }
};

// A drawing target. It either owns its pixels or wraps external memory (shared buffers).
struct Surface {
    int w = 0, h = 0, stride = 0;  // stride in pixels
    u32* px = nullptr;
    std::vector<u32> own;
    Rect clip;

    void alloc(int width, int height);
    void wrap(u32* data, int width, int height);
    void reset_clip() { clip = {0, 0, w, h}; }
};

// ---- primitives (all respect Surface::clip and blend by the color's alpha)
void fill(Surface& s, Rect r, u32 c);
void rrect(Surface& s, Rect r, int radius, u32 c);
void rrect_outline(Surface& s, Rect r, int radius, int thick, u32 c);
void rrect_gradient(Surface& s, Rect r, int radius, u32 top, u32 bottom);
// Outline whose color travels through the blue/teal/violet palette; `phase` rotates it.
void rrect_outline_glow(Surface& s, Rect r, int radius, int thick, float phase, int alpha);
void shadow(Surface& s, Rect r, int radius, int spread, int dy, u32 c);
void circle(Surface& s, int cx, int cy, int radius, u32 c);
void ring(Surface& s, int cx, int cy, int radius, int thick, u32 c);
void line(Surface& s, int x0, int y0, int x1, int y1, u32 c, int thick = 1);
void blit(Surface& dst, int dx, int dy, const Surface& src);  // opaque copy, clipped
void blit_blend(Surface& dst, int dx, int dy, const Surface& src);

// ---- text (bitmap PSF font, UTF-8)
bool load_font(const char* path);
bool font_ok();
int font_w();
int font_h();
int text_width(const std::string& s, int scale = 1);
int draw_text(Surface& s, int x, int y, const std::string& text, u32 c, int scale = 1);
std::string fit_text(const std::string& s, int max_px, int scale = 1);  // adds "~" when cut
std::size_t u8_next(const std::string& s, std::size_t i);
std::size_t u8_prev(const std::string& s, std::size_t i);
int u8_len(const std::string& s);
std::uint32_t u8_codepoint(const std::string& s, std::size_t& i);

// ---- images
bool load_ppm(const char* path, Image& out);
Image scale_cover(const Image& src, int w, int h);  // bilinear, crop to fill
void box_blur(Image& img, int radius, int passes);
void to_surface(const Image& img, Surface& s);

// ---- glass: frosted panel over a pre-blurred background
void glass(Surface& s, const Image& blurred, Rect r, int radius, bool transparency);

// ---- helpers
u32 glow_color(float t);  // looping blue -> teal -> violet palette, t in [0,1)
float ease_out(float t);
float ease_in_out(float t);
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

}  // namespace dg
