// SPDX-License-Identifier: MIT
#include "gfx.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <unordered_map>

namespace dg {

// ------------------------------------------------------------------ basics
Rect Rect::intersect(const Rect& o) const {
    const int x0 = std::max(x, o.x), y0 = std::max(y, o.y);
    const int x1 = std::min(right(), o.right()), y1 = std::min(bottom(), o.bottom());
    return {x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0)};
}

u32 mix(u32 a, u32 b, float t) {
    t = clampf(t, 0.0f, 1.0f);
    auto m = [t](int p, int q) { return static_cast<int>(static_cast<float>(p) + static_cast<float>(q - p) * t + 0.5f); };
    return rgba(m(cr(a), cr(b)), m(cg(a), cg(b)), m(cb(a), cb(b)), m(ca(a), ca(b)));
}

void Surface::alloc(int width, int height) {
    w = width; h = height; stride = width;
    own.assign(static_cast<std::size_t>(width) * height, 0xFF000000u);
    px = own.data();
    reset_clip();
}

void Surface::wrap(u32* data, int width, int height) {
    w = width; h = height; stride = width;
    own.clear();
    px = data;
    reset_clip();
}

static inline u32 over(u32 d, u32 s) {
    const int a = ca(s);
    if (a >= 255) return s | 0xFF000000u;
    if (a <= 0) return d;
    const int ia = 255 - a;
    const int r = (cr(s) * a + cr(d) * ia + 127) / 255;
    const int g = (cg(s) * a + cg(d) * ia + 127) / 255;
    const int b = (cb(s) * a + cb(d) * ia + 127) / 255;
    return rgba(r, g, b, 255);
}

static inline void put(Surface& s, int x, int y, u32 c, int cov = 255) {
    if (x < s.clip.x || y < s.clip.y || x >= s.clip.right() || y >= s.clip.bottom()) return;
    int a = ca(c);
    if (cov < 255) a = a * cov / 255;
    if (a <= 0) return;
    u32& d = s.px[static_cast<std::size_t>(y) * s.stride + x];
    d = a >= 255 ? (c | 0xFF000000u) : over(d, with_alpha(c, a));
}

// -------------------------------------------------------------- primitives
void fill(Surface& s, Rect r, u32 c) {
    r = r.intersect(s.clip).intersect({0, 0, s.w, s.h});
    if (r.empty() || ca(c) == 0) return;
    const bool opaque = ca(c) >= 255;
    for (int y = r.y; y < r.bottom(); ++y) {
        u32* row = s.px + static_cast<std::size_t>(y) * s.stride;
        if (opaque) {
            const u32 v = c | 0xFF000000u;
            for (int x = r.x; x < r.right(); ++x) row[x] = v;
        } else {
            for (int x = r.x; x < r.right(); ++x) row[x] = over(row[x], c);
        }
    }
}

// Coverage (0..255) of pixel (px,py) inside a rounded rectangle.
static int rr_cov(int px, int py, const Rect& r, int rad) {
    const bool left = px < r.x + rad, right = px >= r.right() - rad;
    const bool top = py < r.y + rad, bottom = py >= r.bottom() - rad;
    if (!((left || right) && (top || bottom))) return 255;
    const float cx = left ? static_cast<float>(r.x + rad) : static_cast<float>(r.right() - rad);
    const float cy = top ? static_cast<float>(r.y + rad) : static_cast<float>(r.bottom() - rad);
    const float dx = static_cast<float>(px) + 0.5f - cx, dy = static_cast<float>(py) + 0.5f - cy;
    const float d = std::sqrt(dx * dx + dy * dy) - static_cast<float>(rad);
    const float c = clampf(0.5f - d, 0.0f, 1.0f);
    return static_cast<int>(c * 255.0f + 0.5f);
}

// Fills [x0,x1) of row y (clipped); blends when the color is translucent.
static inline void span(Surface& s, int y, int x0, int x1, u32 c) {
    if (y < s.clip.y || y >= s.clip.bottom()) return;
    x0 = std::max(x0, s.clip.x);
    x1 = std::min(x1, s.clip.right());
    const int a = ca(c);
    if (x0 >= x1 || a <= 0) return;
    u32* row = s.px + static_cast<std::size_t>(y) * s.stride;
    if (a >= 255) {
        const u32 v = c | 0xFF000000u;
        for (int x = x0; x < x1; ++x) row[x] = v;
    } else {
        for (int x = x0; x < x1; ++x) row[x] = over(row[x], c);
    }
}

static void rrect_rows(Surface& s, Rect r, int rad, const std::function<u32(int)>* rowcol, u32 c) {
    rad = std::max(0, std::min(rad, std::min(r.w, r.h) / 2));
    const Rect vis = r.intersect(s.clip).intersect({0, 0, s.w, s.h});
    if (vis.empty()) return;
    const int xl = r.x + rad, xr = r.right() - rad;
    for (int y = vis.y; y < vis.bottom(); ++y) {
        const u32 col = rowcol ? (*rowcol)(y) : c;
        if (y >= r.y + rad && y < r.bottom() - rad) { span(s, y, vis.x, vis.right(), col); continue; }
        // top / bottom rows: antialiased corners, straight middle part as one span
        for (int x = vis.x; x < std::min(vis.right(), xl); ++x) put(s, x, y, col, rr_cov(x, y, r, rad));
        span(s, y, std::max(vis.x, xl), std::min(vis.right(), xr), col);
        for (int x = std::max(vis.x, xr); x < vis.right(); ++x) put(s, x, y, col, rr_cov(x, y, r, rad));
    }
}

void rrect(Surface& s, Rect r, int radius, u32 c) { rrect_rows(s, r, radius, nullptr, c); }

void rrect_gradient(Surface& s, Rect r, int radius, u32 top, u32 bottom) {
    const std::function<u32(int)> f = [&](int y) {
        return mix(top, bottom, r.h > 1 ? static_cast<float>(y - r.y) / static_cast<float>(r.h - 1) : 0.0f);
    };
    rrect_rows(s, r, radius, &f, 0);
}

// Visits only the pixels of a rounded outline (cost ~ perimeter, not area): fn(x, y, coverage 1..255).
template <class F>
static void outline_walk(Surface& s, Rect r, int radius, int thick, F&& fn) {
    radius = std::max(0, std::min(radius, std::min(r.w, r.h) / 2));
    const Rect inner = r.inset(thick);
    const int irad = std::max(0, radius - thick);
    const Rect vis = r.intersect(s.clip).intersect({0, 0, s.w, s.h});
    const int cz = std::max(radius, thick);  // width of the corner zone
    const int xl = r.x + cz, xr = r.right() - cz;
    auto cov = [&](int x, int y) {
        const int outer = rr_cov(x, y, r, radius);
        const int in = inner.has(x, y) ? rr_cov(x, y, inner, irad) : 0;
        return std::max(0, outer - in);
    };
    for (int y = vis.y; y < vis.bottom(); ++y) {
        const bool edge = y < r.y + thick || y >= r.bottom() - thick;
        const bool band = y < r.y + cz || y >= r.bottom() - cz;
        if (edge || band) {
            for (int x = vis.x; x < std::min(vis.right(), xl); ++x) { const int c = cov(x, y); if (c > 0) fn(x, y, c); }
            if (edge) { for (int x = std::max(vis.x, xl); x < std::min(vis.right(), xr); ++x) fn(x, y, 255); }
            for (int x = std::max(vis.x, xr); x < vis.right(); ++x) { const int c = cov(x, y); if (c > 0) fn(x, y, c); }
        } else {
            for (int x = vis.x; x < std::min(vis.right(), r.x + thick); ++x) fn(x, y, 255);
            for (int x = std::max(vis.x, r.right() - thick); x < vis.right(); ++x) fn(x, y, 255);
        }
    }
}

void rrect_outline(Surface& s, Rect r, int radius, int thick, u32 c) {
    outline_walk(s, r, radius, thick, [&](int x, int y, int cov) { put(s, x, y, c, cov); });
}

void shadow(Surface& s, Rect r, int radius, int spread, int dy, u32 c) {
    if (spread <= 0) return;
    const int a = ca(c);
    // one-pixel rings with a falling alpha: the same soft edge as stacked filled shapes at a fraction of the cost
    for (int i = spread; i >= 1; --i) {
        const float k = 1.0f - static_cast<float>(i) / static_cast<float>(spread + 1);
        const int alpha = std::max(1, static_cast<int>(static_cast<float>(a) * k * k * 0.62f));
        rrect_outline(s, {r.x - i, r.y - i + dy, r.w + 2 * i, r.h + 2 * i}, radius + i, 1, with_alpha(c, alpha));
    }
}

void circle(Surface& s, int cx, int cy, int radius, u32 c) { rrect(s, {cx - radius, cy - radius, 2 * radius, 2 * radius}, radius, c); }
void ring(Surface& s, int cx, int cy, int radius, int thick, u32 c) {
    rrect_outline(s, {cx - radius, cy - radius, 2 * radius, 2 * radius}, radius, thick, c);
}

void line(Surface& s, int x0, int y0, int x1, int y1, u32 c, int thick) {
    int dx = std::abs(x1 - x0), dy = -std::abs(y1 - y0);
    const int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        for (int ox = 0; ox < thick; ++ox)
            for (int oy = 0; oy < thick; ++oy) put(s, x0 + ox - thick / 2, y0 + oy - thick / 2, c);
        if (x0 == x1 && y0 == y1) break;
        const int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void blit(Surface& dst, int dx, int dy, const Surface& src) {
    Rect r = Rect{dx, dy, src.w, src.h}.intersect(dst.clip).intersect({0, 0, dst.w, dst.h});
    for (int y = r.y; y < r.bottom(); ++y)
        std::memcpy(dst.px + static_cast<std::size_t>(y) * dst.stride + r.x,
                    src.px + static_cast<std::size_t>(y - dy) * src.stride + (r.x - dx), static_cast<std::size_t>(r.w) * 4);
}

void blit_blend(Surface& dst, int dx, int dy, const Surface& src) {
    Rect r = Rect{dx, dy, src.w, src.h}.intersect(dst.clip).intersect({0, 0, dst.w, dst.h});
    for (int y = r.y; y < r.bottom(); ++y)
        for (int x = r.x; x < r.right(); ++x)
            put(dst, x, y, src.px[static_cast<std::size_t>(y - dy) * src.stride + (x - dx)]);
}

// -------------------------------------------------------------------- UTF-8
std::uint32_t u8_codepoint(const std::string& s, std::size_t& i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    int n = 0;
    std::uint32_t cp = 0;
    if (c < 0x80) { ++i; return c; }
    if ((c & 0xE0) == 0xC0) { n = 1; cp = c & 0x1Fu; }
    else if ((c & 0xF0) == 0xE0) { n = 2; cp = c & 0x0Fu; }
    else if ((c & 0xF8) == 0xF0) { n = 3; cp = c & 0x07u; }
    else { ++i; return 0xFFFD; }
    ++i;
    for (int k = 0; k < n; ++k) {
        if (i >= s.size() || (static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) return 0xFFFD;
        cp = (cp << 6) | (static_cast<unsigned char>(s[i]) & 0x3Fu);
        ++i;
    }
    return cp;
}

std::size_t u8_next(const std::string& s, std::size_t i) {
    ++i;
    while (i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) ++i;
    return std::min(i, s.size());
}

std::size_t u8_prev(const std::string& s, std::size_t i) {
    if (i == 0) return 0;
    --i;
    while (i > 0 && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) --i;
    return i;
}

int u8_len(const std::string& s) {
    int n = 0;
    for (std::size_t i = 0; i < s.size(); i = u8_next(s, i)) ++n;
    return n;
}

// --------------------------------------------------------------------- font
namespace {
struct Font {
    int w = 8, h = 16, bpr = 1, n = 0;
    std::vector<std::uint8_t> glyphs;
    std::unordered_map<std::uint32_t, int> map;
    bool ok = false;
} g_font;

std::uint32_t le32(const std::vector<std::uint8_t>& b, std::size_t o) {
    return b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (static_cast<std::uint32_t>(b[o + 3]) << 24);
}

int glyph_for(std::uint32_t cp) {
    auto it = g_font.map.find(cp);
    if (it != g_font.map.end()) return it->second;
    if (g_font.map.empty() && cp < static_cast<std::uint32_t>(g_font.n)) return static_cast<int>(cp);
    it = g_font.map.find('?');
    return it != g_font.map.end() ? it->second : 0;
}
}  // namespace

bool load_font(const char* path) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::vector<std::uint8_t> b;
    std::uint8_t tmp[4096];
    std::size_t n;
    while ((n = std::fread(tmp, 1, sizeof tmp, f)) > 0) b.insert(b.end(), tmp, tmp + n);
    std::fclose(f);
    Font& F = g_font;
    F = Font();
    if (b.size() > 4 && b[0] == 0x36 && b[1] == 0x04) {  // PSF1
        const int mode = b[2];
        F.h = b[3]; F.w = 8; F.bpr = 1;
        F.n = (mode & 1) ? 512 : 256;
        const std::size_t gl = static_cast<std::size_t>(F.n) * static_cast<std::size_t>(F.h);
        if (b.size() < 4 + gl) return false;
        F.glyphs.assign(b.begin() + 4, b.begin() + 4 + static_cast<std::ptrdiff_t>(gl));
        if (mode & 2) {
            std::size_t p = 4 + gl;
            for (int g = 0; g < F.n && p + 1 < b.size(); ++g) {
                bool seq = false;
                while (p + 1 < b.size()) {
                    const unsigned v = b[p] | (b[p + 1] << 8);
                    p += 2;
                    if (v == 0xFFFF) break;
                    if (v == 0xFFFE) { seq = true; continue; }
                    if (!seq && !F.map.count(v)) F.map[v] = g;
                }
            }
        }
        F.ok = true;
        return true;
    }
    if (b.size() > 32 && b[0] == 0x72 && b[1] == 0xb5 && b[2] == 0x4a && b[3] == 0x86) {  // PSF2
        const std::uint32_t hs = le32(b, 8), flags = le32(b, 12), len = le32(b, 16), cs = le32(b, 20);
        F.h = static_cast<int>(le32(b, 24));
        F.w = static_cast<int>(le32(b, 28));
        F.bpr = (F.w + 7) / 8;
        F.n = static_cast<int>(len);
        const std::size_t gl = static_cast<std::size_t>(len) * cs;
        if (b.size() < hs + gl || cs != static_cast<std::uint32_t>(F.bpr * F.h)) return false;
        F.glyphs.assign(b.begin() + hs, b.begin() + static_cast<std::ptrdiff_t>(hs + gl));
        if (flags & 1) {
            std::size_t p = hs + gl;
            for (int g = 0; g < F.n && p < b.size(); ++g) {
                bool seq = false;
                while (p < b.size() && b[p] != 0xFF) {
                    if (b[p] == 0xFE) { seq = true; ++p; continue; }
                    std::string s;
                    while (p < b.size() && b[p] != 0xFF && b[p] != 0xFE) s += static_cast<char>(b[p++]);
                    std::size_t i = 0;
                    const std::uint32_t cp = u8_codepoint(s, i);
                    if (!seq && !F.map.count(cp)) F.map[cp] = g;
                }
                ++p;
            }
        }
        F.ok = true;
        return true;
    }
    return false;
}

bool font_ok() { return g_font.ok; }
int font_w() { return g_font.w; }
int font_h() { return g_font.h; }

int text_width(const std::string& s, int scale) { return u8_len(s) * g_font.w * scale; }

int draw_text(Surface& s, int x, int y, const std::string& text, u32 c, int scale) {
    std::size_t i = 0;
    while (i < text.size()) {
        const std::uint32_t cp = u8_codepoint(text, i);
        if (cp != ' ' && g_font.ok && x + g_font.w * scale > s.clip.x && x < s.clip.right()) {
            const std::uint8_t* g = &g_font.glyphs[static_cast<std::size_t>(glyph_for(cp)) * static_cast<std::size_t>(g_font.bpr * g_font.h)];
            for (int row = 0; row < g_font.h; ++row)
                for (int col = 0; col < g_font.w; ++col)
                    if (g[row * g_font.bpr + col / 8] & (0x80 >> (col % 8)))
                        for (int sy = 0; sy < scale; ++sy)
                            for (int sx = 0; sx < scale; ++sx) put(s, x + col * scale + sx, y + row * scale + sy, c);
        }
        x += g_font.w * scale;
    }
    return x;
}

std::string fit_text(const std::string& s, int max_px, int scale) {
    const int cols = std::max(0, max_px / std::max(1, g_font.w * scale));
    if (u8_len(s) <= cols) return s;
    if (cols < 4) return s.substr(0, [&] { std::size_t i = 0; for (int k = 0; k < cols; ++k) i = u8_next(s, i); return i; }());
    std::size_t i = 0;
    for (int k = 0; k < cols - 3; ++k) i = u8_next(s, i);
    return s.substr(0, i) + "...";
}

// ------------------------------------------------------------------- images
bool load_ppm(const char* path, Image& out) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    int w = 0, h = 0, mx = 0;
    if (std::fscanf(f, "P6 %d %d %d", &w, &h, &mx) != 3 || w <= 0 || h <= 0 || mx != 255) { std::fclose(f); return false; }
    std::fgetc(f);  // single whitespace after the header
    std::vector<std::uint8_t> raw(static_cast<std::size_t>(w) * h * 3);
    const std::size_t got = std::fread(raw.data(), 1, raw.size(), f);
    std::fclose(f);
    if (got != raw.size()) return false;
    out.w = w; out.h = h;
    out.px.resize(static_cast<std::size_t>(w) * h);
    for (std::size_t i = 0; i < out.px.size(); ++i) out.px[i] = rgba(raw[i * 3], raw[i * 3 + 1], raw[i * 3 + 2]);
    return true;
}

Image scale_cover(const Image& src, int w, int h) {
    Image o;
    o.w = w; o.h = h;
    o.px.resize(static_cast<std::size_t>(w) * h);
    if (!src.ok()) return o;
    const float s = std::max(static_cast<float>(w) / static_cast<float>(src.w), static_cast<float>(h) / static_cast<float>(src.h));
    const float ox = (static_cast<float>(src.w) - static_cast<float>(w) / s) / 2.0f;
    const float oy = (static_cast<float>(src.h) - static_cast<float>(h) / s) / 2.0f;
    for (int y = 0; y < h; ++y) {
        const float fy = clampf(oy + (static_cast<float>(y) + 0.5f) / s - 0.5f, 0.0f, static_cast<float>(src.h - 1));
        const int y0 = static_cast<int>(fy), y1 = std::min(src.h - 1, y0 + 1);
        const float ty = fy - static_cast<float>(y0);
        for (int x = 0; x < w; ++x) {
            const float fx = clampf(ox + (static_cast<float>(x) + 0.5f) / s - 0.5f, 0.0f, static_cast<float>(src.w - 1));
            const int x0 = static_cast<int>(fx), x1 = std::min(src.w - 1, x0 + 1);
            const float tx = fx - static_cast<float>(x0);
            const u32 a = src.px[static_cast<std::size_t>(y0) * src.w + x0], b = src.px[static_cast<std::size_t>(y0) * src.w + x1];
            const u32 c = src.px[static_cast<std::size_t>(y1) * src.w + x0], d = src.px[static_cast<std::size_t>(y1) * src.w + x1];
            o.px[static_cast<std::size_t>(y) * w + x] = mix(mix(a, b, tx), mix(c, d, tx), ty);
        }
    }
    return o;
}

void box_blur(Image& img, int radius, int passes) {
    if (!img.ok() || radius < 1) return;
    std::vector<u32> tmp(img.px.size());
    auto pass = [&](const std::vector<u32>& in, std::vector<u32>& out, bool horiz) {
        const int len = horiz ? img.w : img.h, lines = horiz ? img.h : img.w;
        for (int l = 0; l < lines; ++l) {
            auto at = [&](int i) -> u32 {
                i = clampi(i, 0, len - 1);
                return horiz ? in[static_cast<std::size_t>(l) * img.w + i] : in[static_cast<std::size_t>(i) * img.w + l];
            };
            int sr = 0, sg = 0, sb = 0;
            for (int i = -radius; i <= radius; ++i) { const u32 c = at(i); sr += cr(c); sg += cg(c); sb += cb(c); }
            const int n = 2 * radius + 1;
            for (int i = 0; i < len; ++i) {
                const u32 v = rgba(sr / n, sg / n, sb / n);
                if (horiz) out[static_cast<std::size_t>(l) * img.w + i] = v; else out[static_cast<std::size_t>(i) * img.w + l] = v;
                const u32 add = at(i + radius + 1), sub = at(i - radius);
                sr += cr(add) - cr(sub); sg += cg(add) - cg(sub); sb += cb(add) - cb(sub);
            }
        }
    };
    for (int p = 0; p < passes; ++p) {
        pass(img.px, tmp, true);
        pass(tmp, img.px, false);
    }
}

void to_surface(const Image& img, Surface& s) {
    if (!img.ok()) return;
    if (s.w != img.w || s.h != img.h) s.alloc(img.w, img.h);
    std::memcpy(s.px, img.px.data(), img.px.size() * 4);
}

// -------------------------------------------------------------------- glass
void glass(Surface& s, const Image& blurred, Rect r, int radius, bool transparency) {
    radius = std::max(0, std::min(radius, std::min(r.w, r.h) / 2));
    const Rect vis = r.intersect(s.clip).intersect({0, 0, s.w, s.h});
    const u32 tint = transparency ? 0xA00F131Cu : 0xFF141925u;
    for (int y = vis.y; y < vis.bottom(); ++y)
        for (int x = vis.x; x < vis.right(); ++x) {
            const int cov = rr_cov(x, y, r, radius);
            if (cov == 0) continue;
            u32 base = (blurred.ok() && x < blurred.w && y < blurred.h) ? blurred.px[static_cast<std::size_t>(y) * blurred.w + x] : 0xFF0E1118u;
            const u32 col = transparency ? over(base, tint) : tint;
            u32& d = s.px[static_cast<std::size_t>(y) * s.stride + x];
            d = cov >= 255 ? col : mix(d, col, static_cast<float>(cov) / 255.0f) | 0xFF000000u;
        }
    rrect_outline(s, r, radius, 1, 0x30FFFFFFu);
    if (r.w > 2 * radius + 4)
        fill(s, {r.x + radius, r.y + 1, r.w - 2 * radius, 1}, 0x1CFFFFFFu);
}

// ------------------------------------------------------------------ helpers
u32 glow_color(float t) {
    t -= std::floor(t);
    static const u32 stops[4] = {0xFF4C8DFF, 0xFF2DD4BF, 0xFF8B5CF6, 0xFF4C8DFF};
    const float p = t * 3.0f;
    const int i = std::min(2, static_cast<int>(p));
    return mix(stops[i], stops[i + 1], p - static_cast<float>(i));
}

float ease_out(float t) {
    t = clampf(t, 0.0f, 1.0f);
    const float u = 1.0f - t;
    return 1.0f - u * u * u;
}

float ease_in_out(float t) {
    t = clampf(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

}  // namespace dg

namespace dg {

void rrect_outline_glow(Surface& s, Rect r, int radius, int thick, float phase, int alpha) {
    const float cx = static_cast<float>(r.x) + static_cast<float>(r.w) / 2.0f, cy = static_cast<float>(r.y) + static_cast<float>(r.h) / 2.0f;
    outline_walk(s, r, radius, thick, [&](int x, int y, int cov) {
        const float ang = std::atan2(static_cast<float>(y) + 0.5f - cy, static_cast<float>(x) + 0.5f - cx);
        put(s, x, y, with_alpha(glow_color(ang / 6.2831853f + 0.5f + phase), alpha), cov);
    });
}

}  // namespace dg
