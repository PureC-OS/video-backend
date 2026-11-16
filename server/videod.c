#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>
#include "libc/include/purec.h"
#include "vb_proto.h"

#define MAX_CLIENTS 32
#define MAX_WINDOWS 64
#define BORDER 1
#define CURSOR_W 12
#define CURSOR_H 19

#define HID_TAB 0x2B
#define HID_BACKSPACE 0x2A
#define HID_F4 0x3D

#define COLOR_BORDER 0x3A3F4Bu
#define COLOR_BORDER_FOCUS 0x5B8DEFu
#define COLOR_PAD 0x1E1E2Eu
#define MIN_W 160
#define MIN_H 100
#define CONFIGURE_MS 33
#define HID_DOWN 0x51
#define HID_UP 0x52

struct client {
    int fd;
    char in[sizeof(struct vb_msg)];
    size_t have;
    bool hello;
    bool tasks;
    int version;
};

struct window {
    uint32_t id;
    struct client *owner;
    int x, y, w, h;
    const uint32_t *pixels;
    size_t bytes;
    int role;
    bool minimized;
    char title[VB_TEXT_MAX];
    int bw, bh;
    int state;
    int rx, ry, rw, rh;
    int cursor;
    bool pending;
    int pend_x, pend_y, pend_w, pend_h;
};

struct rect {
    int x0, y0, x1, y1;
};

static struct fb_map_info fb;
static uint32_t *back;
static int screen_w, screen_h;
static struct client clients[MAX_CLIENTS];
static struct window *stack[MAX_WINDOWS];
static int stack_n;
static uint32_t next_id = 1;
static struct window *focused, *hovered, *grab, *drag, *resize_w;
static int drag_dx, drag_dy;
static uint32_t resize_edges;
static int resize_x, resize_y, resize_ww, resize_hh, resize_mx, resize_my;
static uint64_t resize_sent;
static int mouse_x, mouse_y;
static uint32_t mouse_buttons;
static uint8_t mods;
static struct rect dirty;
static bool cursor_shown;
static int cursor_kind;
static volatile sig_atomic_t quit;
static struct rect workarea;

static const char *const cursor_shape[CURSOR_H] = {
    "X           ", "XX          ", "X.X         ", "X..X        ", "X...X       ", "X....X      ",
    "X.....X     ", "X......X    ", "X.......X   ", "X........X  ", "X.........X ", "X..........X",
    "X......XXXXX", "X...X..X    ", "X..XX..X    ", "X.X  X..X   ", "XX   X..X   ", "X     X..X  ",
    "      XXX   ",
};

static const char *const cursor_h[11] = {
    "                     ",
    "    XXX       XXX    ",
    "   XX.X       X.XX   ",
    "  XX..X       X..XX  ",
    " XX...XXXXXXXXX...XX ",
    " X.................X ",
    " XX...XXXXXXXXX...XX ",
    "  XX..X       X..XX  ",
    "   XX.X       X.XX   ",
    "    XXX       XXX    ",
    "                     ",
};

static const char *const cursor_v[21] = {
    "           ",
    "    XXX    ",
    "   XX.XX   ",
    "  XX...XX  ",
    " XX.....XX ",
    " X.......X ",
    " XXXX.XXXX ",
    "    X.X    ",
    "    X.X    ",
    "    X.X    ",
    "    X.X    ",
    "    X.X    ",
    "    X.X    ",
    "    X.X    ",
    " XXXX.XXXX ",
    " X.......X ",
    " XX.....XX ",
    "  XX...XX  ",
    "   XX.XX   ",
    "    XXX    ",
    "           ",
};

static const char *const cursor_nwse[17] = {
    "                 ",
    " XXXXXXX         ",
    " X.....X         ",
    " X....XX         ",
    " X...XX          ",
    " X..X.XX         ",
    " X.XXX.XX        ",
    " XXX XX.XX       ",
    "      XX.XX      ",
    "       XX.XX XXX ",
    "        XX.XXX.X ",
    "         XX.X..X ",
    "          XX...X ",
    "         XX....X ",
    "         X.....X ",
    "         XXXXXXX ",
    "                 ",
};

static const char *const cursor_nesw[17] = {
    "                 ",
    "         XXXXXXX ",
    "         X.....X ",
    "         XX....X ",
    "          XX...X ",
    "         XX.X..X ",
    "        XX.XXX.X ",
    "       XX.XX XXX ",
    "      XX.XX      ",
    " XXX XX.XX       ",
    " X.XXX.XX        ",
    " X..X.XX         ",
    " X...XX          ",
    " X....XX         ",
    " X.....X         ",
    " XXXXXXX         ",
    "                 ",
};

static const char *const cursor_move[21] = {
    "                     ",
    "    XXX              ",
    "   XX.XX             ",
    "  XX...XX            ",
    " XX.....XX           ",
    " X.......X           ",
    " XXXX.XXXX    XXX    ",
    "   XX.X       X.XX   ",
    "  XX..X       X..XX  ",
    " XX...XXXXXXXXX...XX ",
    " X.................X ",
    " XX...XXXXXXXXX...XX ",
    "  XX..X       X..XX  ",
    "   XX.X       X.XX   ",
    " XXXX.XXXX    XXX    ",
    " X.......X           ",
    " XX.....XX           ",
    "  XX...XX            ",
    "   XX.XX             ",
    "    XXX              ",
    "                     ",
};

struct cursor_def {
    int w, h, hx, hy;
    const char *const *rows;
};

static const struct cursor_def cursors[VB_CURSOR_COUNT] = {
    [VB_CURSOR_ARROW] = {CURSOR_W, CURSOR_H, 0, 0, cursor_shape},
    [VB_CURSOR_RESIZE_H] = {21, 11, 10, 5, cursor_h},
    [VB_CURSOR_RESIZE_V] = {11, 21, 5, 10, cursor_v},
    [VB_CURSOR_RESIZE_NWSE] = {17, 17, 8, 8, cursor_nwse},
    [VB_CURSOR_RESIZE_NESW] = {17, 17, 8, 8, cursor_nesw},
    [VB_CURSOR_MOVE] = {21, 21, 10, 10, cursor_move},
};

static struct rect cursor_box;

static void on_signal(int sig)
{
    (void)sig;
    quit = 1;
}

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static void add_dirty(int x, int y, int w, int h)
{
    struct rect r = {clampi(x, 0, screen_w), clampi(y, 0, screen_h), clampi(x + w, 0, screen_w),
                     clampi(y + h, 0, screen_h)};
    if (r.x0 >= r.x1 || r.y0 >= r.y1)
        return;
    if (dirty.x0 >= dirty.x1) {
        dirty = r;
        return;
    }
    if (r.x0 < dirty.x0) dirty.x0 = r.x0;
    if (r.y0 < dirty.y0) dirty.y0 = r.y0;
    if (r.x1 > dirty.x1) dirty.x1 = r.x1;
    if (r.y1 > dirty.y1) dirty.y1 = r.y1;
}

static void damage_window_frame(const struct window *w)
{
    add_dirty(w->x - BORDER, w->y - BORDER, w->w + 2 * BORDER, w->h + 2 * BORDER);
}

static int layer_of(const struct window *w)
{
    if (w->role == VB_ROLE_NORMAL && w->state == VB_STATE_FULLSCREEN)
        return 2;
    switch (w->role) {
    case VB_ROLE_BACKGROUND: return 0;
    case VB_ROLE_PANEL: return 2;
    case VB_ROLE_POPUP: return 3;
    default: return 1;
    }
}

static bool focusable(const struct window *w)
{
    return w && !w->minimized && (w->role == VB_ROLE_NORMAL || w->role == VB_ROLE_BACKGROUND);
}

#define COLOR_BACKGROUND 0x11111Bu
#define HOLD_MS 1500u
#define DISPLAY_CONFIG "/config/display.ini"

static uint64_t hold_until;

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static bool has_content(void)
{
    for (int i = 0; i < stack_n; i++)
        if (!stack[i]->minimized && (stack[i]->role == VB_ROLE_BACKGROUND || stack[i]->role == VB_ROLE_NORMAL))
            return true;
    return false;
}

static void hold_frame(void)
{
    hold_until = now_ms() + HOLD_MS;
}

static bool holding(void)
{
    if (!hold_until)
        return false;
    if (now_ms() < hold_until)
        return true;
    hold_until = 0;
    return false;
}

static uint32_t background(int x, int y)
{
    (void)x;
    (void)y;
    return COLOR_BACKGROUND;
}

static uint32_t *fb_row(int y)
{
    return (uint32_t *)(uintptr_t)(fb.address + (uint64_t)y * fb.pitch_bytes);
}

static int border_of(const struct window *w)
{
    if (w->role != VB_ROLE_NORMAL || !w->owner || w->owner->version >= 3)
        return 0;
    return BORDER;
}

static void compose_rect(const struct rect *r)
{
    for (int y = r->y0; y < r->y1; y++) {
        uint32_t *line = back + (size_t)y * (size_t)screen_w;
        for (int x = r->x0; x < r->x1; x++)
            line[x] = background(x, y);
    }
    for (int i = 0; i < stack_n; i++) {
        const struct window *w = stack[i];
        if (w->minimized)
            continue;
        uint32_t border = w == focused ? COLOR_BORDER_FOCUS : COLOR_BORDER;
        int bw = border_of(w);
        int fx0 = w->x - bw, fy0 = w->y - bw, fx1 = w->x + w->w + bw, fy1 = w->y + w->h + bw;
        int y0 = fy0 > r->y0 ? fy0 : r->y0, y1 = fy1 < r->y1 ? fy1 : r->y1;
        int x0 = fx0 > r->x0 ? fx0 : r->x0, x1 = fx1 < r->x1 ? fx1 : r->x1;
        for (int y = y0; y < y1; y++) {
            uint32_t *line = back + (size_t)y * (size_t)screen_w;
            bool inner_row = y >= w->y && y < w->y + w->h;
            for (int x = x0; x < x1; x++) {
                bool inner = inner_row && x >= w->x && x < w->x + w->w;
                if (!inner) {
                    line[x] = border;
                    continue;
                }
                int ix0 = x, ix1 = x1 < w->x + w->w ? x1 : w->x + w->w;
                int cy = y - w->y, cx0 = ix0 - w->x, cx1 = ix1 - w->x;
                int end = cy < w->bh ? (cx1 < w->bw ? cx1 : w->bw) : cx0;
                if (end > cx0)
                    memcpy(line + ix0, w->pixels + (size_t)cy * (size_t)w->bw + (size_t)cx0, (size_t)(end - cx0) * 4);
                for (int px = end > cx0 ? end : cx0; px < cx1; px++)
                    line[w->x + px] = COLOR_PAD;
                x = ix1 - 1;
            }
        }
    }
}

static void copy_to_fb(const struct rect *r)
{
    for (int y = r->y0; y < r->y1; y++)
        memcpy(fb_row(y) + r->x0, back + (size_t)y * (size_t)screen_w + r->x0, (size_t)(r->x1 - r->x0) * 4);
}

static int wanted_cursor(void)
{
    if (resize_w) {
        bool h = resize_edges & (VB_EDGE_LEFT | VB_EDGE_RIGHT), v = resize_edges & (VB_EDGE_TOP | VB_EDGE_BOTTOM);
        if (h && v)
            return ((resize_edges & VB_EDGE_LEFT) != 0) == ((resize_edges & VB_EDGE_TOP) != 0) ? VB_CURSOR_RESIZE_NWSE
                                                                                            : VB_CURSOR_RESIZE_NESW;
        return h ? VB_CURSOR_RESIZE_H : VB_CURSOR_RESIZE_V;
    }
    if (drag)
        return VB_CURSOR_MOVE;
    const struct window *w = grab ? grab : hovered;
    return w ? w->cursor : VB_CURSOR_ARROW;
}

static void draw_cursor(void)
{
    const struct cursor_def *d = &cursors[cursor_kind];
    for (int cy = 0; cy < d->h; cy++) {
        int y = cursor_box.y0 + cy;
        if (y < 0 || y >= screen_h)
            continue;
        uint32_t *line = fb_row(y);
        for (int cx = 0; cx < d->w; cx++) {
            int x = cursor_box.x0 + cx;
            char c = d->rows[cy][cx];
            if (x < 0 || x >= screen_w || c == ' ')
                continue;
            line[x] = c == 'X' ? 0x000000u : 0xFFFFFFu;
        }
    }
}

static void damage_box(const struct rect *b)
{
    struct rect r = {clampi(b->x0, 0, screen_w), clampi(b->y0, 0, screen_h), clampi(b->x1, 0, screen_w),
                     clampi(b->y1, 0, screen_h)};
    if (r.x0 < r.x1 && r.y0 < r.y1) {
        copy_to_fb(&r);
        pc_fb_damage((uint32_t)r.x0, (uint32_t)r.y0, (uint32_t)(r.x1 - r.x0), (uint32_t)(r.y1 - r.y0));
    }
}

static void flush(void)
{
    if (holding())
        return;
    int kind = wanted_cursor();
    const struct cursor_def *d = &cursors[kind];
    struct rect box = {mouse_x - d->hx, mouse_y - d->hy, mouse_x - d->hx + d->w, mouse_y - d->hy + d->h};
    bool moved = !cursor_shown || kind != cursor_kind || memcmp(&box, &cursor_box, sizeof(box)) != 0;
    struct rect old = cursor_box;
    bool had_dirty = dirty.x0 < dirty.x1;
    if (had_dirty) {
        compose_rect(&dirty);
        copy_to_fb(&dirty);
        pc_fb_damage((uint32_t)dirty.x0, (uint32_t)dirty.y0, (uint32_t)(dirty.x1 - dirty.x0),
                     (uint32_t)(dirty.y1 - dirty.y0));
    }
    bool overlap = had_dirty && old.x0 < dirty.x1 && old.x1 > dirty.x0 && old.y0 < dirty.y1 && old.y1 > dirty.y0;
    dirty.x0 = dirty.x1 = 0;
    if (!moved && !overlap)
        return;
    if (cursor_shown && moved)
        damage_box(&old);
    cursor_box = box;
    cursor_kind = kind;
    cursor_shown = true;
    draw_cursor();
    struct rect r = {clampi(box.x0, 0, screen_w), clampi(box.y0, 0, screen_h), clampi(box.x1, 0, screen_w),
                     clampi(box.y1, 0, screen_h)};
    if (r.x0 < r.x1 && r.y0 < r.y1)
        pc_fb_damage((uint32_t)r.x0, (uint32_t)r.y0, (uint32_t)(r.x1 - r.x0), (uint32_t)(r.y1 - r.y0));
}

static void send_msg(struct client *c, const struct vb_msg *m)
{
    if (!c || c->fd < 0)
        return;
    (void)send(c->fd, m, sizeof(*m), MSG_NOSIGNAL | MSG_DONTWAIT);
}

static void send_event(struct window *w, uint32_t type, int a, int b, int cc, int d, uint32_t e)
{
    if (!w)
        return;
    struct vb_msg m;
    memset(&m, 0, sizeof(m));
    m.type = type;
    m.window = w->id;
    m.a = a;
    m.b = b;
    m.c = cc;
    m.d = d;
    m.e = e;
    send_msg(w->owner, &m);
}

static void send_task(const struct window *w)
{
    if (w->role != VB_ROLE_NORMAL)
        return;
    struct vb_msg m;
    memset(&m, 0, sizeof(m));
    m.type = VB_TASK;
    m.window = w->id;
    m.a = (int)((w == focused ? VB_TASK_FOCUSED : 0) | (w->minimized ? VB_TASK_MINIMIZED : 0));
    memcpy(m.text, w->title, sizeof(m.text));
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].fd >= 0 && clients[i].tasks)
            send_msg(&clients[i], &m);
}

static void send_task_gone(uint32_t id)
{
    struct vb_msg m;
    memset(&m, 0, sizeof(m));
    m.type = VB_TASK_GONE;
    m.window = id;
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].fd >= 0 && clients[i].tasks)
            send_msg(&clients[i], &m);
}

static void send_workarea(struct client *only)
{
    struct vb_msg m;
    memset(&m, 0, sizeof(m));
    m.type = VB_WORKAREA;
    m.a = workarea.x0;
    m.b = workarea.y0;
    m.c = workarea.x1 - workarea.x0;
    m.d = workarea.y1 - workarea.y0;
    if (only) {
        send_msg(only, &m);
        return;
    }
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].fd >= 0 && clients[i].hello)
            send_msg(&clients[i], &m);
}

static void apply_state_geometry(struct window *w);

static void update_workarea(void)
{
    struct rect r = {0, 0, screen_w, screen_h};
    for (int i = 0; i < stack_n; i++) {
        const struct window *w = stack[i];
        if (w->role != VB_ROLE_PANEL || w->minimized || w->x > 0 || w->x + w->w < screen_w)
            continue;
        if (w->y <= 0 && w->y + w->h > r.y0)
            r.y0 = w->y + w->h;
        else if (w->y + w->h >= screen_h && w->y < r.y1)
            r.y1 = w->y;
    }
    if (r.y1 <= r.y0)
        r = (struct rect){0, 0, screen_w, screen_h};
    if (memcmp(&r, &workarea, sizeof(r)) != 0) {
        workarea = r;
        send_workarea(NULL);
        for (int i = 0; i < stack_n; i++)
            if (stack[i]->role == VB_ROLE_NORMAL && stack[i]->state == VB_STATE_MAXIMIZED)
                apply_state_geometry(stack[i]);
    }
}

static int stack_index(const struct window *w)
{
    for (int i = 0; i < stack_n; i++)
        if (stack[i] == w)
            return i;
    return -1;
}

static void set_focus(struct window *w)
{
    if (w == focused || (w && !focusable(w)))
        return;
    struct window *old = focused;
    focused = w;
    if (old) {
        send_event(old, VB_FOCUS, 0, 0, 0, 0, 0);
        damage_window_frame(old);
        send_task(old);
    }
    if (w) {
        send_event(w, VB_FOCUS, 1, 0, 0, 0, 0);
        damage_window_frame(w);
        send_task(w);
    }
}

static struct window *next_focus(const struct window *skip)
{
    for (int i = stack_n - 1; i >= 0; i--)
        if (stack[i] != skip && stack[i]->role == VB_ROLE_NORMAL && !stack[i]->minimized)
            return stack[i];
    for (int i = stack_n - 1; i >= 0; i--)
        if (stack[i] != skip && stack[i]->role == VB_ROLE_BACKGROUND)
            return stack[i];
    return NULL;
}

static void insert_window(struct window *w)
{
    int pos = stack_n;
    while (pos > 0 && layer_of(stack[pos - 1]) > layer_of(w))
        pos--;
    memmove(stack + pos + 1, stack + pos, (size_t)(stack_n - pos) * sizeof(stack[0]));
    stack[pos] = w;
    stack_n++;
}

static void raise_window(struct window *w)
{
    int i = stack_index(w);
    if (i < 0)
        return;
    memmove(stack + i, stack + i + 1, (size_t)(stack_n - 1 - i) * sizeof(stack[0]));
    stack_n--;
    insert_window(w);
    if (stack_index(w) != i)
        damage_window_frame(w);
}

static struct window *window_at(int x, int y)
{
    for (int i = stack_n - 1; i >= 0; i--) {
        struct window *w = stack[i];
        if (!w->minimized && x >= w->x && x < w->x + w->w && y >= w->y && y < w->y + w->h)
            return w;
    }
    return NULL;
}

static struct window *find_any(uint32_t id)
{
    for (int i = 0; i < stack_n; i++)
        if (stack[i]->id == id)
            return stack[i];
    return NULL;
}

static struct window *find_window(struct client *c, uint32_t id)
{
    for (int i = 0; i < stack_n; i++)
        if (stack[i]->id == id && stack[i]->owner == c)
            return stack[i];
    return NULL;
}

static void destroy_window(struct window *w)
{
    int i = stack_index(w);
    if (i < 0)
        return;
    damage_window_frame(w);
    memmove(stack + i, stack + i + 1, (size_t)(stack_n - 1 - i) * sizeof(stack[0]));
    stack_n--;
    if (hovered == w)
        hovered = NULL;
    if (grab == w)
        grab = NULL;
    if (drag == w)
        drag = NULL;
    if (resize_w == w)
        resize_w = NULL;
    if (focused == w) {
        focused = NULL;
        set_focus(next_focus(w));
    }
    if (w->role == VB_ROLE_NORMAL)
        send_task_gone(w->id);
    int role = w->role;
    munmap((void *)w->pixels, w->bytes);
    free(w);
    if (role == VB_ROLE_PANEL)
        update_workarea();
    if ((role == VB_ROLE_BACKGROUND || role == VB_ROLE_NORMAL) && !has_content())
        hold_frame();
}

static void send_error(struct client *c, int code, const char *text)
{
    struct vb_msg m;
    memset(&m, 0, sizeof(m));
    m.type = VB_ERROR;
    m.a = code;
    snprintf(m.text, sizeof(m.text), "%s", text ? text : "");
    send_msg(c, &m);
}

static void *map_shm(const char *name, int width, int height, size_t *bytes_out)
{
    size_t bytes = (size_t)width * (size_t)height * 4;
    int fd = shm_open(name, O_RDWR, 0);
    if (fd < 0)
        return NULL;
    struct stat st;
    if (fstat(fd, &st) == 0 && st.st_size > 0 && (size_t)st.st_size < bytes) {
        close(fd);
        return NULL;
    }
    void *map = mmap(NULL, bytes, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (map == MAP_FAILED || !map)
        return NULL;
    *bytes_out = bytes;
    return map;
}

static void create_window(struct client *c, const struct vb_msg *m)
{
    char name[VB_TEXT_MAX];
    memcpy(name, m->text, sizeof(name));
    name[sizeof(name) - 1] = 0;
    if (m->a <= 0 || m->b <= 0 || m->a > VB_MAX_SIZE || m->b > VB_MAX_SIZE) {
        send_error(c, VB_ERR_SIZE, name);
        return;
    }
    if (stack_n >= MAX_WINDOWS) {
        send_error(c, VB_ERR_LIMIT, name);
        return;
    }
    size_t bytes = 0;
    void *map = map_shm(name, m->a, m->b, &bytes);
    if (!map) {
        send_error(c, VB_ERR_SHM, name);
        return;
    }
    struct window *w = calloc(1, sizeof(*w));
    if (!w) {
        munmap(map, bytes);
        send_error(c, VB_ERR_LIMIT, name);
        return;
    }
    static int cascade;
    w->id = next_id++;
    w->owner = c;
    w->w = m->a;
    w->h = m->b;
    w->bw = m->a;
    w->bh = m->b;
    w->pixels = map;
    w->bytes = bytes;
    w->role = m->e <= VB_ROLE_POPUP ? (int)m->e : VB_ROLE_NORMAL;
    if (m->c >= 0 && m->d >= 0) {
        w->x = m->c;
        w->y = m->d;
    } else {
        int aw = workarea.x1 - workarea.x0, ah = workarea.y1 - workarea.y0;
        int spanx = aw - w->w > 0 ? aw - w->w : 0, spany = ah - w->h > 0 ? ah - w->h : 0;
        w->x = workarea.x0 + clampi(spanx / 2 - 120 + cascade * 32, 0, spanx);
        w->y = workarea.y0 + clampi(spany / 2 - 90 + cascade * 32, 0, spany);
        cascade = (cascade + 1) % 8;
    }
    if (w->role == VB_ROLE_NORMAL && w->y < workarea.y0)
        w->y = workarea.y0;
    insert_window(w);
    damage_window_frame(w);
    struct vb_msg r;
    memset(&r, 0, sizeof(r));
    r.type = VB_CREATED;
    r.window = w->id;
    r.a = w->x;
    r.b = w->y;
    r.c = w->w;
    r.d = w->h;
    memcpy(r.text, name, sizeof(r.text));
    send_msg(c, &r);
    if (w->role == VB_ROLE_PANEL)
        update_workarea();
    if (w->role == VB_ROLE_NORMAL) {
        send_task(w);
        set_focus(w);
    } else if (w->role == VB_ROLE_BACKGROUND && !focused) {
        set_focus(w);
    }
}

static void send_configure(struct window *w, int x, int y, int width, int height)
{
    w->pending = true;
    w->pend_x = x;
    w->pend_y = y;
    w->pend_w = width;
    w->pend_h = height;
    if (w->owner && w->owner->version >= 3)
        send_event(w, VB_CONFIGURE, x, y, width, height, (uint32_t)w->state);
    else
        send_event(w, VB_CONFIGURE, x, y, 0, 0, 0);
}

static void minimize_window(struct window *w)
{
    if (w->role != VB_ROLE_NORMAL || w->minimized)
        return;
    damage_window_frame(w);
    w->minimized = true;
    if (hovered == w)
        hovered = NULL;
    if (grab == w)
        grab = NULL;
    if (drag == w)
        drag = NULL;
    if (resize_w == w)
        resize_w = NULL;
    if (focused == w)
        set_focus(next_focus(w));
    send_task(w);
}

static void apply_state_geometry(struct window *w)
{
    if (w->state == VB_STATE_MAXIMIZED)
        send_configure(w, workarea.x0, workarea.y0, workarea.x1 - workarea.x0, workarea.y1 - workarea.y0);
    else if (w->state == VB_STATE_FULLSCREEN)
        send_configure(w, 0, 0, screen_w, screen_h);
    else
        send_configure(w, w->rx, w->ry, w->rw, w->rh);
}

static void set_state(struct window *w, int state)
{
    if (w->role != VB_ROLE_NORMAL)
        return;
    if (state == VB_STATE_MINIMIZED) {
        minimize_window(w);
        return;
    }
    if (state < VB_STATE_NORMAL || state > VB_STATE_FULLSCREEN || state == w->state)
        return;
    if (w->state == VB_STATE_NORMAL) {
        w->rx = w->x;
        w->ry = w->y;
        w->rw = w->w;
        w->rh = w->h;
    }
    if (resize_w == w)
        resize_w = NULL;
    if (drag == w)
        drag = NULL;
    int old = w->state;
    w->state = state;
    if (state == VB_STATE_FULLSCREEN || old == VB_STATE_FULLSCREEN)
        raise_window(w);
    damage_window_frame(w);
    apply_state_geometry(w);
}

static void start_resize(struct window *w, uint32_t edges)
{
    resize_w = w;
    resize_edges = edges;
    resize_x = w->x;
    resize_y = w->y;
    resize_ww = w->w;
    resize_hh = w->h;
    resize_mx = mouse_x;
    resize_my = mouse_y;
    resize_sent = 0;
    drag = NULL;
    grab = NULL;
}

static void attach_buffer(struct client *c, struct window *w, const struct vb_msg *m)
{
    char name[VB_TEXT_MAX];
    memcpy(name, m->text, sizeof(name));
    name[sizeof(name) - 1] = 0;
    if (m->a <= 0 || m->b <= 0 || m->a > VB_MAX_SIZE || m->b > VB_MAX_SIZE) {
        send_error(c, VB_ERR_SIZE, name);
        return;
    }
    size_t bytes = 0;
    void *map = map_shm(name, m->a, m->b, &bytes);
    if (!map) {
        send_error(c, VB_ERR_SHM, name);
        return;
    }
    damage_window_frame(w);
    munmap((void *)w->pixels, w->bytes);
    w->pixels = map;
    w->bytes = bytes;
    w->bw = m->a;
    w->bh = m->b;
    if (resize_w == w) {
        if (resize_edges & VB_EDGE_LEFT)
            w->x = resize_x + resize_ww - m->a;
        if (resize_edges & VB_EDGE_TOP)
            w->y = resize_y + resize_hh - m->b;
    } else if (w->pending && (w->state != VB_STATE_NORMAL || (w->pend_w == m->a && w->pend_h == m->b))) {
        w->x = w->pend_x;
        w->y = w->pend_y;
        w->pending = false;
    }
    w->w = m->a;
    w->h = m->b;
    damage_window_frame(w);
    struct vb_msg r;
    memset(&r, 0, sizeof(r));
    r.type = VB_RESIZED;
    r.window = w->id;
    r.a = m->a;
    r.b = m->b;
    memcpy(r.text, name, sizeof(r.text));
    send_msg(c, &r);
}

static int apply_mode(int width, int height)
{
    if (width == screen_w && height == screen_h)
        return 0;
    int32_t rc = pc_display_set_mode((uint32_t)width, (uint32_t)height, 32);
    if (rc < 0)
        return rc;
    struct fb_map_info next;
    rc = pc_fb_map(&next);
    if (rc < 0 || next.bpp != 32 || !next.width || !next.height)
        return rc < 0 ? rc : -1;
    uint32_t *next_back = malloc((size_t)next.width * (size_t)next.height * 4);
    if (!next_back)
        return -1;
    free(back);
    back = next_back;
    fb = next;
    screen_w = (int)fb.width;
    screen_h = (int)fb.height;
    mouse_x = clampi(mouse_x, 0, screen_w - 1);
    mouse_y = clampi(mouse_y, 0, screen_h - 1);
    cursor_shown = false;
    dirty.x0 = dirty.x1 = 0;
    resize_w = NULL;
    drag = NULL;
    workarea = (struct rect){0, 0, 0, 0};
    update_workarea();
    for (int i = 0; i < stack_n; i++) {
        struct window *w = stack[i];
        if (w->role != VB_ROLE_NORMAL)
            continue;
        if (w->state != VB_STATE_NORMAL) {
            apply_state_geometry(w);
        } else {
            int nx = clampi(w->x, 0, screen_w > w->w ? screen_w - w->w : 0);
            int ny = clampi(w->y, workarea.y0, screen_h > w->h ? screen_h - w->h : workarea.y0);
            if (nx != w->x || ny != w->y) {
                w->x = nx;
                w->y = ny;
                send_event(w, VB_CONFIGURE, w->x, w->y, 0, 0, (uint32_t)w->state);
            }
        }
    }
    struct vb_msg m;
    memset(&m, 0, sizeof(m));
    m.type = VB_SCREEN;
    m.a = screen_w;
    m.b = screen_h;
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].fd >= 0 && clients[i].hello)
            send_msg(&clients[i], &m);
    add_dirty(0, 0, screen_w, screen_h);
    syslog(LOG_INFO, "display mode %dx%d", screen_w, screen_h);
    return 0;
}

static void save_mode(void)
{
    mkdir("/config", 0755);
    FILE *f = fopen(DISPLAY_CONFIG, "w");
    if (!f)
        return;
    fprintf(f, "width=%d\nheight=%d\nbpp=32\nscale=100\n", screen_w, screen_h);
    fclose(f);
}

static void load_mode(void)
{
    FILE *f = fopen(DISPLAY_CONFIG, "r");
    if (!f)
        return;
    char line[64];
    int width = 0, height = 0;
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "width=", 6))
            width = atoi(line + 6);
        else if (!strncmp(line, "height=", 7))
            height = atoi(line + 7);
    }
    fclose(f);
    if (width >= 640 && height >= 400 && apply_mode(width, height) < 0)
        syslog(LOG_WARNING, "cannot restore the display mode %dx%d", width, height);
}

static void start_drag(struct window *w)
{
    drag = w;
    drag_dx = mouse_x - w->x;
    drag_dy = mouse_y - w->y;
    grab = NULL;
}

static void handle_request(struct client *c, const struct vb_msg *m)
{
    if (!c->hello && m->type != VB_HELLO) {
        send_error(c, VB_ERR_VERSION, "");
        return;
    }
    struct window *w;
    switch (m->type) {
    case VB_HELLO: {
        struct vb_msg r;
        memset(&r, 0, sizeof(r));
        r.type = VB_WELCOME;
        r.a = screen_w;
        r.b = screen_h;
        r.c = VB_PROTO_VERSION;
        c->hello = m->a >= VB_PROTO_MIN_VERSION && m->a <= VB_PROTO_VERSION;
        c->version = c->hello ? m->a : 0;
        if (!c->hello)
            r.type = VB_ERROR, r.a = VB_ERR_VERSION;
        send_msg(c, &r);
        if (c->hello)
            send_workarea(c);
        break;
    }
    case VB_CREATE:
        create_window(c, m);
        break;
    case VB_DAMAGE:
        if ((w = find_window(c, m->window))) {
            int x0 = clampi(m->a, 0, w->w), y0 = clampi(m->b, 0, w->h);
            int x1 = clampi(m->a + m->c, 0, w->w), y1 = clampi(m->b + m->d, 0, w->h);
            if (x0 < x1 && y0 < y1)
                add_dirty(w->x + x0, w->y + y0, x1 - x0, y1 - y0);
            if (w->role == VB_ROLE_BACKGROUND || w->role == VB_ROLE_NORMAL)
                hold_until = 0;
        }
        break;
    case VB_DESTROY:
        if ((w = find_window(c, m->window)))
            destroy_window(w);
        break;
    case VB_MOVE:
        if ((w = find_window(c, m->window)) && w->role == VB_ROLE_NORMAL && w->state == VB_STATE_NORMAL &&
            (mouse_buttons & VB_BUTTON_LEFT)) {
            w->pending = false;
            start_drag(w);
        }
        break;
    case VB_RESIZE:
        if ((w = find_window(c, m->window)))
            attach_buffer(c, w, m);
        break;
    case VB_SET_STATE:
        if ((w = find_window(c, m->window)))
            set_state(w, m->a);
        break;
    case VB_RESIZE_START:
        if ((w = find_window(c, m->window)) && w->role == VB_ROLE_NORMAL && w->state == VB_STATE_NORMAL &&
            (mouse_buttons & VB_BUTTON_LEFT) && (m->a & 15))
            start_resize(w, (uint32_t)m->a & 15u);
        break;
    case VB_SET_MODE:
        if (apply_mode(m->a, m->b) < 0)
            send_error(c, VB_ERR_SIZE, "mode");
        else
            save_mode();
        break;
    case VB_CURSOR:
        if ((w = find_window(c, m->window)))
            w->cursor = m->a >= 0 && m->a < VB_CURSOR_COUNT ? m->a : VB_CURSOR_ARROW;
        break;
    case VB_TITLE:
        if ((w = find_window(c, m->window))) {
            memcpy(w->title, m->text, sizeof(w->title));
            w->title[sizeof(w->title) - 1] = 0;
            send_task(w);
        }
        break;
    case VB_RAISE:
        if ((w = find_window(c, m->window))) {
            raise_window(w);
            set_focus(w);
        }
        break;
    case VB_TASKS_SUBSCRIBE:
        c->tasks = true;
        for (int i = 0; i < stack_n; i++)
            if (stack[i]->role == VB_ROLE_NORMAL) {
                struct vb_msg t;
                memset(&t, 0, sizeof(t));
                t.type = VB_TASK;
                t.window = stack[i]->id;
                t.a = (int)((stack[i] == focused ? VB_TASK_FOCUSED : 0) |
                            (stack[i]->minimized ? VB_TASK_MINIMIZED : 0));
                memcpy(t.text, stack[i]->title, sizeof(t.text));
                send_msg(c, &t);
            }
        break;
    case VB_ACTIVATE:
        if ((w = find_any(m->window)) && w->role == VB_ROLE_NORMAL) {
            w->minimized = false;
            raise_window(w);
            damage_window_frame(w);
            set_focus(w);
            send_task(w);
        }
        break;
    case VB_MINIMIZE:
        if ((w = find_any(m->window)))
            minimize_window(w);
        break;
    default:
        break;
    }
}

static void drop_client(struct client *c)
{
    for (int i = stack_n - 1; i >= 0; i--)
        if (stack[i]->owner == c)
            destroy_window(stack[i]);
    close(c->fd);
    c->fd = -1;
    c->have = 0;
    c->hello = false;
}

static void read_client(struct client *c)
{
    char buf[sizeof(struct vb_msg) * 16];
    ssize_t n = recv(c->fd, buf, sizeof(buf), MSG_DONTWAIT);
    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
        drop_client(c);
        return;
    }
    for (ssize_t i = 0; i < n; i++) {
        c->in[c->have++] = buf[i];
        if (c->have == sizeof(struct vb_msg)) {
            struct vb_msg m;
            memcpy(&m, c->in, sizeof(m));
            c->have = 0;
            handle_request(c, &m);
            if (c->fd < 0)
                return;
        }
    }
}

static void accept_client(int listen_fd)
{
    int fd = accept(listen_fd, NULL, NULL);
    if (fd < 0)
        return;
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].fd < 0) {
            memset(&clients[i], 0, sizeof(clients[i]));
            clients[i].fd = fd;
            return;
        }
    close(fd);
}

static void pointer_to(struct window *w, uint32_t changed, int wheel)
{
    send_event(w, VB_POINTER, mouse_x - w->x, mouse_y - w->y, (int)mouse_buttons, wheel, changed);
}

static void handle_mouse(const struct input_record *r)
{
    int nx = clampi(r->x, 0, screen_w - 1), ny = clampi(r->y, 0, screen_h - 1);
    uint32_t changed = r->buttons ^ mouse_buttons;
    uint32_t pressed = changed & r->buttons;
    mouse_x = nx;
    mouse_y = ny;
    mouse_buttons = r->buttons;
    if (resize_w) {
        struct window *w = resize_w;
        int dx = mouse_x - resize_mx, dy = mouse_y - resize_my;
        int x0 = resize_x, y0 = resize_y, x1 = resize_x + resize_ww, y1 = resize_y + resize_hh;
        if (resize_edges & VB_EDGE_LEFT)
            x0 = clampi(x0 + dx, x1 - VB_MAX_SIZE, x1 - MIN_W);
        if (resize_edges & VB_EDGE_RIGHT)
            x1 = clampi(x1 + dx, x0 + MIN_W, x0 + VB_MAX_SIZE);
        if (resize_edges & VB_EDGE_TOP) {
            int lo = y1 - VB_MAX_SIZE > workarea.y0 ? y1 - VB_MAX_SIZE : workarea.y0;
            y0 = clampi(y0 + dy, lo, y1 - MIN_H);
        }
        if (resize_edges & VB_EDGE_BOTTOM)
            y1 = clampi(y1 + dy, y0 + MIN_H, y0 + VB_MAX_SIZE);
        bool done = !(mouse_buttons & VB_BUTTON_LEFT);
        uint64_t now = now_ms();
        bool changed_size = !w->pending || w->pend_x != x0 || w->pend_y != y0 || w->pend_w != x1 - x0 ||
                            w->pend_h != y1 - y0;
        if (changed_size && (done || now - resize_sent >= CONFIGURE_MS)) {
            send_configure(w, x0, y0, x1 - x0, y1 - y0);
            resize_sent = now;
        }
        if (done) {
            resize_w = NULL;
            pointer_to(w, changed, 0);
        }
        return;
    }
    if (drag) {
        damage_window_frame(drag);
        drag->x = mouse_x - drag_dx;
        drag->y = mouse_y - drag_dy;
        if (drag->y < workarea.y0)
            drag->y = workarea.y0;
        damage_window_frame(drag);
        if (!(mouse_buttons & VB_BUTTON_LEFT)) {
            pointer_to(drag, changed, 0);
            send_event(drag, VB_CONFIGURE, drag->x, drag->y, 0, 0, (uint32_t)drag->state);
            drag = NULL;
        }
        return;
    }
    struct window *under = window_at(mouse_x, mouse_y);
    if (pressed) {
        for (int i = 0; i < stack_n; i++)
            if (stack[i]->role == VB_ROLE_POPUP && stack[i] != under)
                send_event(stack[i], VB_POPUP_DONE, 0, 0, 0, 0, 0);
        if (under) {
            if (under->role == VB_ROLE_NORMAL)
                raise_window(under);
            set_focus(under);
            if (under->role == VB_ROLE_NORMAL && (mods & VB_MOD_ALT) && (pressed & VB_BUTTON_LEFT)) {
                start_drag(under);
                return;
            }
            grab = under;
        }
    }
    struct window *target = grab ? grab : under;
    if (!grab && target != hovered) {
        if (hovered)
            send_event(hovered, VB_LEAVE, 0, 0, 0, 0, 0);
        hovered = target;
        if (hovered)
            send_event(hovered, VB_ENTER, mouse_x - hovered->x, mouse_y - hovered->y, 0, 0, 0);
    }
    if (target)
        pointer_to(target, changed, r->wheel);
    if (!mouse_buttons)
        grab = NULL;
}

static void cycle_focus(void)
{
    for (int i = 0; i < stack_n; i++) {
        struct window *w = stack[i];
        if (w->role == VB_ROLE_NORMAL && !w->minimized) {
            if (w == focused && w == next_focus(NULL))
                return;
            raise_window(w);
            set_focus(w);
            return;
        }
    }
}

static void handle_key(const struct input_record *r)
{
    mods = r->mods;
    bool ctrl = mods & VB_MOD_CTRL, alt = mods & VB_MOD_ALT;
    if (r->pressed && ctrl && alt && r->scancode == HID_BACKSPACE) {
        quit = 1;
        return;
    }
    if (r->pressed && alt && !ctrl && r->scancode == HID_F4) {
        if (focused && focused->role == VB_ROLE_NORMAL)
            send_event(focused, VB_CLOSE, 0, 0, 0, 0, 0);
        return;
    }
    if (r->pressed && alt && !ctrl && r->scancode == HID_TAB) {
        cycle_focus();
        return;
    }
    bool gui = mods & VB_MOD_GUI;
    if (r->pressed && gui && focused && focused->role == VB_ROLE_NORMAL && focused->owner &&
        focused->owner->version >= 3 && (r->scancode == HID_UP || r->scancode == HID_DOWN)) {
        if (r->scancode == HID_UP)
            set_state(focused, VB_STATE_MAXIMIZED);
        else if (focused->state != VB_STATE_NORMAL)
            set_state(focused, VB_STATE_NORMAL);
        else
            set_state(focused, VB_STATE_MINIMIZED);
        return;
    }
    if (focused)
        send_event(focused, VB_KEY, r->scancode, r->pressed, r->repeat, r->mods, 0);
}

static void read_input(int fd)
{
    struct input_record recs[32];
    ssize_t got = read(fd, recs, sizeof(recs));
    if (got <= 0)
        return;
    for (int i = 0; i < (int)(got / (ssize_t)sizeof(recs[0])); i++) {
        if (recs[i].type == INPUT_RECORD_MOUSE)
            handle_mouse(&recs[i]);
        else if (recs[i].type == INPUT_RECORD_KEY)
            handle_key(&recs[i]);
    }
}

static int open_socket(void)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", VB_SOCKET_PATH);
    unlink(VB_SOCKET_PATH);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(fd, 8) < 0) {
        close(fd);
        return -1;
    }
    chmod(VB_SOCKET_PATH, 0666);
    return fd;
}

static int fail(const char *what, int code)
{
    fprintf(stderr, "videod: %s (%d)\n", what, code);
    syslog(LOG_ERR, "%s (%d)", what, code);
    return 1;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    openlog("videod", 0, LOG_DAEMON);
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGPIPE, SIG_IGN);
    for (int i = 0; i < MAX_CLIENTS; i++)
        clients[i].fd = -1;
    int32_t rc = pc_display_claim();
    if (rc < 0)
        return fail("cannot claim the display", rc);
    rc = pc_fb_map(&fb);
    if (rc < 0 || fb.bpp != 32 || !fb.width || !fb.height) {
        pc_display_release();
        return fail("cannot map the framebuffer", rc);
    }
    screen_w = (int)fb.width;
    screen_h = (int)fb.height;
    back = malloc((size_t)screen_w * (size_t)screen_h * 4);
    if (!back) {
        pc_display_release();
        return fail("out of memory for the back buffer", 0);
    }
    int input = pc_input_open();
    if (input < 0) {
        pc_display_release();
        return fail("cannot open the input stream", input);
    }
    mkdir("/run", 0755);
    int listen_fd = open_socket();
    if (listen_fd < 0) {
        close(input);
        pc_display_release();
        return fail("cannot listen on " VB_SOCKET_PATH, errno);
    }
    syslog(LOG_INFO, "display %dx%d pitch %u, listening on %s", screen_w, screen_h, fb.pitch_bytes, VB_SOCKET_PATH);
    mouse_x = screen_w / 2;
    mouse_y = screen_h / 2;
    workarea = (struct rect){0, 0, screen_w, screen_h};
    load_mode();
    add_dirty(0, 0, screen_w, screen_h);
    hold_frame();
    flush();
    struct pollfd fds[2 + MAX_CLIENTS];
    int owners[2 + MAX_CLIENTS];
    while (!quit) {
        int n = 0;
        fds[n] = (struct pollfd){.fd = listen_fd, .events = POLLIN};
        owners[n++] = -1;
        fds[n] = (struct pollfd){.fd = input, .events = POLLIN};
        owners[n++] = -2;
        for (int i = 0; i < MAX_CLIENTS; i++)
            if (clients[i].fd >= 0) {
                fds[n] = (struct pollfd){.fd = clients[i].fd, .events = POLLIN};
                owners[n++] = i;
            }
        int ready = poll(fds, (nfds_t)n, 50);
        if (ready < 0 && errno != EINTR)
            break;
        for (int i = 0; i < n && ready > 0; i++) {
            if (!fds[i].revents)
                continue;
            if (owners[i] == -1)
                accept_client(listen_fd);
            else if (owners[i] == -2)
                read_input(input);
            else if (clients[owners[i]].fd >= 0)
                read_client(&clients[owners[i]]);
        }
        flush();
    }
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].fd >= 0)
            drop_client(&clients[i]);
    close(listen_fd);
    unlink(VB_SOCKET_PATH);
    close(input);
    pc_display_release();
    syslog(LOG_INFO, "display released");
    return 0;
}
