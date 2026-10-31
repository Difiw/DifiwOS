// SDL2 backend for Netline: used for development and tests on a normal Linux desktop (WSL/WSLg).
// SPDX-License-Identifier: MIT
#include <SDL.h>
#include <unistd.h>
#include <fstream>
#include <vector>
#include <cstdio>
#include <cstdlib>

#include "platform.hpp"

namespace nl {
namespace {
int sdl_to_vk(SDL_Keycode k) {
    if (k >= SDLK_a && k <= SDLK_z) return 'A' + (k - SDLK_a);
    if (k >= SDLK_0 && k <= SDLK_9) return k;
    if (k >= SDLK_F1 && k <= SDLK_F12) return 0x70 + (k - SDLK_F1);
    switch (k) {
        case SDLK_BACKSPACE: return 8; case SDLK_TAB: return 9; case SDLK_RETURN: case SDLK_KP_ENTER: return 13; case SDLK_ESCAPE: return 27;
        case SDLK_SPACE: return 32; case SDLK_PAGEUP: return 33; case SDLK_PAGEDOWN: return 34; case SDLK_END: return 35;
        case SDLK_HOME: return 36; case SDLK_LEFT: return 37; case SDLK_UP: return 38; case SDLK_RIGHT: return 39;
        case SDLK_DOWN: return 40; case SDLK_DELETE: return 46; case SDLK_INSERT: return 45;
        case SDLK_COMMA: return 0xBC; case SDLK_PERIOD: return 0xBE; case SDLK_MINUS: case SDLK_KP_MINUS: return 0xBD;
        case SDLK_EQUALS: case SDLK_PLUS: case SDLK_KP_PLUS: return 0xBB; case SDLK_SLASH: return 0xBF;
        case SDLK_SEMICOLON: return 0xBA; case SDLK_QUOTE: return 0xDE;
        default: return 0;
    }
}
int mods_now() {
    const Uint16 m = SDL_GetModState();
    return ((m & KMOD_SHIFT) ? MOD_SHIFT : 0) | ((m & KMOD_CTRL) ? MOD_CTRL : 0) | ((m & KMOD_ALT) ? MOD_ALT : 0);
}
SDL_SystemCursor cursor_for(int t) {  // cef_cursor_type_t values
    switch (t) {
        case 2: return SDL_SYSTEM_CURSOR_HAND;
        case 3: case 41: return SDL_SYSTEM_CURSOR_IBEAM;
        case 4: return SDL_SYSTEM_CURSOR_WAIT;
        case 1: return SDL_SYSTEM_CURSOR_CROSSHAIR;
        case 31: return SDL_SYSTEM_CURSOR_NO;
        default: return SDL_SYSTEM_CURSOR_ARROW;
    }
}

class SdlPlatform : public Platform {
public:
    bool init(int w, int h, const std::string& title) override {
        if (SDL_Init(SDL_INIT_VIDEO) != 0) return false;
        win_ = SDL_CreateWindow(title.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h, SDL_WINDOW_RESIZABLE);
        if (!win_) return false;
        ren_ = SDL_CreateRenderer(win_, -1, SDL_RENDERER_ACCELERATED);
        if (!ren_) ren_ = SDL_CreateRenderer(win_, -1, SDL_RENDERER_SOFTWARE);
        if (!ren_) return false;
        {  // Netline's own icon (res/netline.dicon: u32 w, u32 h, RGBA) instead of the window system's default
            char exe[4096] = {0};
            ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
            std::string path = std::string(exe, n > 0 ? n : 0);
            path = path.substr(0, path.rfind('/')) + "/res/netline.dicon";
            std::ifstream f(path, std::ios::binary);
            icon_.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            if (icon_.size() >= 8 + 128 * 128 * 4) {
                SDL_Surface* sf = SDL_CreateRGBSurfaceWithFormatFrom(icon_.data() + 8, 128, 128, 32, 128 * 4, SDL_PIXELFORMAT_ABGR8888);
                if (sf) { SDL_SetWindowIcon(win_, sf); SDL_FreeSurface(sf); }
            }
        }
        SDL_StartTextInput();
        resize(w, h);
        return true;
    }
    dg::Surface& surface() override { return surf_; }
    bool wait(PEvent& out, int timeout_ms) override {
        SDL_Event e;
        if (!SDL_WaitEventTimeout(&e, timeout_ms)) return false;
        switch (e.type) {
            case SDL_QUIT: out.kind = PEvent::Quit; return true;
            case SDL_WINDOWEVENT:
                if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                    int w, h;
                    SDL_GetWindowSize(win_, &w, &h);
                    if (w != surf_.w || h != surf_.h) resize(w, h);
                    out.kind = PEvent::Resize;
                    return true;
                }
                if (e.window.event == SDL_WINDOWEVENT_EXPOSED) { out.kind = PEvent::Expose; return true; }
                return false;
            case SDL_KEYDOWN: case SDL_KEYUP: {
                const int vk = sdl_to_vk(e.key.keysym.sym);
                if (!vk) return false;
                out.kind = PEvent::Key; out.vk = vk; out.mods = mods_now(); out.down = e.type == SDL_KEYDOWN;
                return true;
            }
            case SDL_TEXTINPUT: out.kind = PEvent::Text; out.text = e.text.text; out.mods = mods_now(); return true;
            case SDL_MOUSEMOTION: out.kind = PEvent::MouseMove; out.x = e.motion.x; out.y = e.motion.y; out.mods = mods_now(); return true;
            case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP:
                out.kind = e.type == SDL_MOUSEBUTTONDOWN ? PEvent::MouseDown : PEvent::MouseUp;
                out.x = e.button.x; out.y = e.button.y; out.clicks = e.button.clicks; out.mods = mods_now();
                out.button = e.button.button == SDL_BUTTON_LEFT ? 1 : e.button.button == SDL_BUTTON_MIDDLE ? 2 : 3;
                return true;
            case SDL_MOUSEWHEEL: {
                int x, y;
                SDL_GetMouseState(&x, &y);
                out.kind = PEvent::Wheel; out.x = x; out.y = y; out.wheel = e.wheel.y; out.mods = mods_now();
                return true;
            }
            default: return false;
        }
    }
    void present() override {
        SDL_UpdateTexture(tex_, nullptr, surf_.px, surf_.stride * 4);
        SDL_RenderCopy(ren_, tex_, nullptr, nullptr);
        SDL_RenderPresent(ren_);
    }
    void set_cursor(int t) override {
        SDL_Cursor* c = SDL_CreateSystemCursor(cursor_for(t));
        SDL_SetCursor(c);
        if (cur_) SDL_FreeCursor(cur_);
        cur_ = c;
    }
    std::string clipboard() override {
        char* c = SDL_GetClipboardText();
        std::string s = c ? c : "";
        if (c) SDL_free(c);
        return s;
    }
    void set_clipboard(const std::string& s) override { SDL_SetClipboardText(s.c_str()); }
    uint32_t ticks() override { return SDL_GetTicks(); }
    void set_title(const std::string& t) override { SDL_SetWindowTitle(win_, t.c_str()); }

private:
    void resize(int w, int h) {
        surf_.alloc(w, h);
        surf_.reset_clip();
        if (tex_) SDL_DestroyTexture(tex_);
        tex_ = SDL_CreateTexture(ren_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
    }
    SDL_Window* win_ = nullptr;
    SDL_Renderer* ren_ = nullptr;
    SDL_Texture* tex_ = nullptr;
    SDL_Cursor* cur_ = nullptr;
    std::vector<char> icon_;
    dg::Surface surf_;
};
}  // namespace

Platform* make_platform() { return new SdlPlatform; }
}  // namespace nl
