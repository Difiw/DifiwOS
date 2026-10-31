// Host tool for the Netline page engine: web_host <page.html> <base-url> <font.psf> <out.ppm> [width] [height]
// Lays a page out with the same code the browser uses and writes the picture, so the engine can be checked without a VM.
// SPDX-License-Identifier: MIT
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "../src/difiw-desktop/gfx.hpp"

using namespace dg;

namespace {
#include "../src/difiw-desktop/web.hpp"
}

int main(int argc, char** argv) {
    if (argc < 5) { std::fprintf(stderr, "usage: web_host page.html base-url font.psf out.ppm [w] [h]\n"); return 2; }
    if (!load_font(argv[3])) { std::fprintf(stderr, "no font\n"); return 3; }
    const int W = argc > 5 ? std::atoi(argv[5]) : 900, H = argc > 6 ? std::atoi(argv[6]) : 700;
    std::ifstream f(argv[1], std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    Doc d;
    d.base = argv[2];
    Sheet sheet;
    if (const char* cf = std::getenv("CSS")) { std::stringstream cl(cf); std::string one; while (std::getline(cl, one, ',')) { std::ifstream c(one); std::stringstream cs; cs << c.rdbuf(); css_parse(cs.str(), sheet); } }
    Layouter(d, W, nullptr, &sheet).run(to_utf8(ss.str(), "utf-8"));
    for (const auto& u : d.css_urls) std::printf("css: %s\n", u.c_str());
    std::printf("fields=%zu forms=%zu\n", d.fields.size(), d.forms.size());
    std::printf("title=%s items=%zu links=%zu imgs=%zu height=%d\n", d.title.c_str(), d.items.size(), d.links.size(), d.imgs.size(), d.height);
    Surface s;
    s.alloc(W, H);
    std::map<std::string, Pic> pics;
    draw_doc(s, {0, 0, W, H}, d, 0, pics, -1);
    FILE* o = std::fopen(argv[4], "wb");
    std::fprintf(o, "P6\n%d %d\n255\n", W, H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const u32 c = s.px[static_cast<std::size_t>(y) * W + x];
            std::fputc((c >> 16) & 255, o); std::fputc((c >> 8) & 255, o); std::fputc(c & 255, o);
        }
    std::fclose(o);
    return 0;
}
