/*
 * libnsfb surface for the Difiw desktop: the page is drawn into a shared-memory window buffer and
 * input arrives from the desktop socket. Replaces libnsfb's "ram" surface at build time.
 * Wire format: see src/difiw-desktop/proto.hpp (fixed 148-byte messages, SOCK_SEQPACKET, memfd by SCM_RIGHTS).
 * SPDX-License-Identifier: MIT
 */
#define _GNU_SOURCE
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "libnsfb.h"
#include "libnsfb_event.h"
#include "libnsfb_plot.h"

#include "nsfb.h"
#include "plot.h"
#include "surface.h"

enum { C_HELLO = 1, C_CREATE = 2, C_ATTACH = 3, C_COMMIT = 4, C_TITLE = 5, C_DESTROY = 6,
       S_WELCOME = 100, S_CREATED = 101, S_CONFIGURE = 102, S_MOUSE = 103, S_KEY = 104, S_FOCUS = 105, S_CLOSE = 106 };

struct dmsg {
    uint32_t type;
    int32_t i[6];
    char s[120];
};

#define QMAX 64
#define RAW_UCS4 0x100000 /* keycode offset that carries a plain Unicode character (patched into fbtk_keycode_to_ucs4) */

struct dstate {
    int sock, memfd, win;
    size_t len;
    uint32_t *map;
    nsfb_event_t q[QMAX];
    int qh, qn;
    int buttons;
    int mods_held[3]; /* shift ctrl alt */
};

static struct dstate G = {.sock = -1, .memfd = -1};

static bool send_msg(const struct dmsg *m, int fd)
{
    struct iovec iov = {(void *)m, sizeof *m};
    char ctl[CMSG_SPACE(sizeof(int))];
    struct msghdr mh;
    memset(&mh, 0, sizeof mh);
    memset(ctl, 0, sizeof ctl);
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    if (fd >= 0) {
        mh.msg_control = ctl;
        mh.msg_controllen = sizeof ctl;
        struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(c), &fd, sizeof(int));
    }
    return sendmsg(G.sock, &mh, MSG_NOSIGNAL) == (ssize_t)sizeof *m;
}

static int recv_msg(struct dmsg *m)
{
    struct iovec iov = {m, sizeof *m};
    struct msghdr mh;
    memset(&mh, 0, sizeof mh);
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    ssize_t n = recvmsg(G.sock, &mh, 0);
    if (n == 0) return 0;
    if (n != (ssize_t)sizeof *m) return -1;
    m->s[sizeof m->s - 1] = 0;
    return 1;
}

static bool make_buffer(int w, int h)
{
    size_t len = (size_t)w * (size_t)h * 4;
    int fd = memfd_create("difiw-window", MFD_CLOEXEC);
    if (fd < 0) return false;
    if (ftruncate(fd, (off_t)len) != 0) { close(fd); return false; }
    void *m = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) { close(fd); return false; }
    uint32_t *p = m;
    for (size_t k = 0; k < (size_t)w * (size_t)h; ++k) p[k] = 0xFFFFFFFFu;
    if (G.map) munmap(G.map, G.len);
    if (G.memfd >= 0) close(G.memfd);
    G.map = m;
    G.memfd = fd;
    G.len = len;
    return true;
}

static void push(nsfb_event_t e)
{
    if (G.qn < QMAX) { G.q[(G.qh + G.qn) % QMAX] = e; ++G.qn; }
}

static void push_key(int code, bool down)
{
    nsfb_event_t e;
    memset(&e, 0, sizeof e);
    e.type = down ? NSFB_EVENT_KEY_DOWN : NSFB_EVENT_KEY_UP;
    e.value.keycode = (enum nsfb_key_code_e)code;
    push(e);
}

static int utf8_cp(const char *s)
{
    const unsigned char *u = (const unsigned char *)s;
    if (u[0] < 0x80) return u[0];
    if ((u[0] & 0xE0) == 0xC0 && (u[1] & 0xC0) == 0x80) return ((u[0] & 0x1F) << 6) | (u[1] & 0x3F);
    if ((u[0] & 0xF0) == 0xE0 && (u[1] & 0xC0) == 0x80 && (u[2] & 0xC0) == 0x80) return ((u[0] & 0x0F) << 12) | ((u[1] & 0x3F) << 6) | (u[2] & 0x3F);
    return 0;
}

static int evdev_special(int code)
{
    switch (code) {
    case 28: case 96: return NSFB_KEY_RETURN;
    case 14: return NSFB_KEY_BACKSPACE;
    case 15: return NSFB_KEY_TAB;
    case 1: return NSFB_KEY_ESCAPE;
    case 111: return NSFB_KEY_DELETE;
    case 103: return NSFB_KEY_UP;
    case 108: return NSFB_KEY_DOWN;
    case 106: return NSFB_KEY_RIGHT;
    case 105: return NSFB_KEY_LEFT;
    case 102: return NSFB_KEY_HOME;
    case 107: return NSFB_KEY_END;
    case 104: return NSFB_KEY_PAGEUP;
    case 109: return NSFB_KEY_PAGEDOWN;
    case 42: return NSFB_KEY_LSHIFT;
    case 54: return NSFB_KEY_RSHIFT;
    case 29: return NSFB_KEY_LCTRL;
    case 97: return NSFB_KEY_RCTRL;
    case 56: return NSFB_KEY_LALT;
    case 100: return NSFB_KEY_RALT;
    default: return 0;
    }
}

static int evdev_letter(int code)
{
    static const char row1[] = "qwertyuiop", row2[] = "asdfghjkl", row3[] = "zxcvbnm";
    if (code >= 16 && code <= 25) return row1[code - 16];
    if (code >= 30 && code <= 38) return row2[code - 30];
    if (code >= 44 && code <= 50) return row3[code - 44];
    return 0;
}

static void handle(const struct dmsg *m, nsfb_t *nsfb)
{
    nsfb_event_t e;
    memset(&e, 0, sizeof e);
    switch (m->type) {
    case S_MOUSE: {
        int kind = m->i[4], b = m->i[3];
        e.type = NSFB_EVENT_MOVE_ABSOLUTE;
        e.value.vector.x = m->i[1];
        e.value.vector.y = m->i[2];
        push(e);
        if (kind == 1 || kind == 2) {
            int diff = b ^ G.buttons;
            /* desktop: bit 1 = left, bit 2 = right; NetSurf: MOUSE_1 left, MOUSE_3 right */
            if (diff & 1) push_key(NSFB_KEY_MOUSE_1, kind == 1);
            if (diff & 2) push_key(NSFB_KEY_MOUSE_3, kind == 1);
            if (!diff) push_key(NSFB_KEY_MOUSE_1, kind == 1);
        } else if (kind == 3 && m->i[5] != 0) {
            int k = m->i[5] > 0 ? NSFB_KEY_MOUSE_4 : NSFB_KEY_MOUSE_5;
            push_key(k, true);
            push_key(k, false);
        }
        G.buttons = b;
        break;
    }
    case S_KEY: {
        int code = m->i[1], state = m->i[2], mods = m->i[3];
        int sp = evdev_special(code);
        bool down = state != 0;
        if (sp) { push_key(sp, down); break; }
        if (!down) break;
        if (mods & 6) { /* ctrl or alt: shortcuts use plain letter codes */
            int l = evdev_letter(code);
            if (l) { push_key(l, true); push_key(l, false); }
            break;
        }
        if (m->s[0]) {
            int cp = utf8_cp(m->s);
            if (cp >= 32 && cp != 127) { push_key(RAW_UCS4 + cp, true); push_key(RAW_UCS4 + cp, false); }
        }
        break;
    }
    case S_CLOSE:
        e.type = NSFB_EVENT_CONTROL;
        e.value.controlcode = NSFB_CONTROL_QUIT;
        push(e);
        break;
    case S_CONFIGURE: {
        int w = m->i[1], h = m->i[2];
        if (w < 64 || h < 64 || w > 4096 || h > 4096 || (w == nsfb->width && h == nsfb->height)) break;
        if (!make_buffer(w, h)) break;
        struct dmsg a;
        memset(&a, 0, sizeof a);
        a.type = C_ATTACH;
        a.i[0] = G.win; a.i[1] = w; a.i[2] = h;
        send_msg(&a, G.memfd);
        nsfb->width = w;
        nsfb->height = h;
        nsfb->ptr = (uint8_t *)G.map;
        nsfb->linelen = w * 4;
        e.type = NSFB_EVENT_RESIZE;
        e.value.resize.w = w;
        e.value.resize.h = h;
        push(e);
        break;
    }
    default: break;
    }
}

static int difiw_defaults(nsfb_t *nsfb)
{
    nsfb->width = 1000;
    nsfb->height = 640;
    nsfb->format = NSFB_FMT_XRGB8888;
    select_plotters(nsfb);
    return 0;
}

static int difiw_initialise(nsfb_t *nsfb)
{
    const char *token = getenv("DIFIW_TOKEN"), *path = getenv("DIFIW_SOCK");
    if (!token) return -1;
    G.sock = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (G.sock < 0) return -1;
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    strncpy(sa.sun_path, path ? path : "/run/difiw/desktop.sock", sizeof sa.sun_path - 1);
    if (connect(G.sock, (struct sockaddr *)&sa, sizeof sa) != 0) return -1;
    struct dmsg m, r;
    memset(&m, 0, sizeof m);
    m.type = C_HELLO;
    strncpy(m.s, token, sizeof m.s - 1);
    if (!send_msg(&m, -1) || recv_msg(&r) != 1 || r.type != S_WELCOME) return -1;
    int w = nsfb->width, h = nsfb->height;
    if (!make_buffer(w, h)) return -1;
    memset(&m, 0, sizeof m);
    m.type = C_CREATE;
    m.i[0] = w; m.i[1] = h;
    strcpy(m.s, "Netline");
    if (!send_msg(&m, G.memfd)) return -1;
    for (;;) {
        if (recv_msg(&r) != 1) return -1;
        if (r.type == S_CREATED) break;
    }
    if (r.i[0] <= 0) return -1;
    G.win = r.i[0];
    if (r.i[1] != w || r.i[2] != h) {
        w = r.i[1]; h = r.i[2];
        if (!make_buffer(w, h)) return -1;
        memset(&m, 0, sizeof m);
        m.type = C_ATTACH;
        m.i[0] = G.win; m.i[1] = w; m.i[2] = h;
        send_msg(&m, G.memfd);
    }
    nsfb->width = w;
    nsfb->height = h;
    nsfb->ptr = (uint8_t *)G.map;
    nsfb->linelen = w * 4;
    return 0;
}

static int difiw_set_geometry(nsfb_t *nsfb, int width, int height, enum nsfb_format_e format)
{
    if (width > 0) nsfb->width = width;
    if (height > 0) nsfb->height = height;
    if (format != NSFB_FMT_ANY) nsfb->format = NSFB_FMT_XRGB8888;
    select_plotters(nsfb);
    nsfb->linelen = nsfb->width * 4;
    return 0;
}

static int difiw_finalise(nsfb_t *nsfb)
{
    (void)nsfb;
    if (G.sock >= 0) {
        struct dmsg m;
        memset(&m, 0, sizeof m);
        m.type = C_DESTROY;
        m.i[0] = G.win;
        send_msg(&m, -1);
        close(G.sock);
        G.sock = -1;
    }
    return 0;
}

static bool difiw_input(nsfb_t *nsfb, nsfb_event_t *event, int timeout)
{
    for (;;) {
        if (G.qn > 0) {
            *event = G.q[G.qh];
            G.qh = (G.qh + 1) % QMAX;
            --G.qn;
            return true;
        }
        struct pollfd pf = {G.sock, POLLIN, 0};
        int pr = poll(&pf, 1, timeout);
        if (pr <= 0) return false;
        struct dmsg m;
        int r = recv_msg(&m);
        if (r <= 0) {
            nsfb_event_t e;
            memset(&e, 0, sizeof e);
            e.type = NSFB_EVENT_CONTROL;
            e.value.controlcode = NSFB_CONTROL_QUIT;
            *event = e;
            return true;
        }
        handle(&m, nsfb);
        timeout = 0; /* drain what is queued, then report "no event" */
    }
}

static int difiw_claim(nsfb_t *nsfb, nsfb_bbox_t *box)
{
    (void)nsfb; (void)box;
    return 0;
}

static int difiw_update(nsfb_t *nsfb, nsfb_bbox_t *box)
{
    /* the plotters leave the unused byte as is; the desktop reads 0xAARRGGBB */
    int x0 = 0, y0 = 0, x1 = nsfb->width, y1 = nsfb->height;
    if (box) {
        if (box->x0 > x0) x0 = box->x0;
        if (box->y0 > y0) y0 = box->y0;
        if (box->x1 < x1) x1 = box->x1;
        if (box->y1 < y1) y1 = box->y1;
    }
    for (int y = y0; y < y1; ++y) {
        uint32_t *row = (uint32_t *)(nsfb->ptr + (size_t)y * (size_t)nsfb->linelen);
        for (int x = x0; x < x1; ++x) row[x] |= 0xFF000000u;
    }
    struct dmsg m;
    memset(&m, 0, sizeof m);
    m.type = C_COMMIT;
    m.i[0] = G.win;
    send_msg(&m, -1);
    return 0;
}

static int difiw_cursor(nsfb_t *nsfb, struct nsfb_cursor_s *cursor)
{
    (void)nsfb; (void)cursor; /* the desktop draws the pointer */
    return 0;
}

const nsfb_surface_rtns_t ram_rtns = {
    .defaults = difiw_defaults,
    .initialise = difiw_initialise,
    .finalise = difiw_finalise,
    .geometry = difiw_set_geometry,
    .input = difiw_input,
    .claim = difiw_claim,
    .update = difiw_update,
    .cursor = difiw_cursor,
};

NSFB_SURFACE_DEF(ram, NSFB_SURFACE_RAM, &ram_rtns)
