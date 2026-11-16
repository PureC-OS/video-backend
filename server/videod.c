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

struct client {
    int fd;
    char in[sizeof(struct vb_msg)];
    size_t have;
    bool hello;
};

struct window {
    uint32_t id;
    struct client *owner;
    int x, y, w, h;
    const uint32_t *pixels;
    size_t bytes;
    char title[VB_TEXT_MAX];
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
static struct window *focused, *hovered, *grab, *drag;
static int drag_dx, drag_dy;
static int mouse_x, mouse_y;
static uint32_t mouse_buttons;
static uint8_t mods;
static struct rect dirty;
static bool cursor_shown;
static int cursor_x, cursor_y;
static volatile sig_atomic_t quit;

static const char *const cursor_shape[CURSOR_H] = {
    "X           ", "XX          ", "X.X         ", "X..X        ", "X...X       ", "X....X      ",
    "X.....X     ", "X......X    ", "X.......X   ", "X........X  ", "X.........X ", "X..........X",
    "X......XXXXX", "X...X..X    ", "X..XX..X    ", "X.X  X..X   ", "XX   X..X   ", "X     X..X  ",
    "      XXX   ",
};

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

static uint32_t background(int x, int y)
{
    (void)x;
    uint32_t t = screen_h ? (uint32_t)y * 64u / (uint32_t)screen_h : 0;
    return ((0x10u + t / 4) << 16) | ((0x18u + t / 3) << 8) | (0x2Au + t);
}

static uint32_t *fb_row(int y)
{
    return (uint32_t *)(uintptr_t)(fb.address + (uint64_t)y * fb.pitch_bytes);
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
        uint32_t border = w == focused ? COLOR_BORDER_FOCUS : COLOR_BORDER;
        int fx0 = w->x - BORDER, fy0 = w->y - BORDER, fx1 = w->x + w->w + BORDER, fy1 = w->y + w->h + BORDER;
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
                memcpy(line + ix0, w->pixels + (size_t)(y - w->y) * (size_t)w->w + (size_t)(ix0 - w->x),
                       (size_t)(ix1 - ix0) * 4);
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

static void draw_cursor(void)
{
    for (int cy = 0; cy < CURSOR_H; cy++) {
        int y = cursor_y + cy;
        if (y < 0 || y >= screen_h)
            continue;
        uint32_t *line = fb_row(y);
        for (int cx = 0; cx < CURSOR_W; cx++) {
            int x = cursor_x + cx;
            char c = cursor_shape[cy][cx];
            if (x < 0 || x >= screen_w || c == ' ')
                continue;
            line[x] = c == 'X' ? 0x000000u : 0xFFFFFFu;
        }
    }
}

static void flush(void)
{
    bool moved = !cursor_shown || cursor_x != mouse_x || cursor_y != mouse_y;
    struct rect old_cursor = {cursor_x, cursor_y, cursor_x + CURSOR_W, cursor_y + CURSOR_H};
    if (dirty.x0 < dirty.x1) {
        compose_rect(&dirty);
        copy_to_fb(&dirty);
        pc_fb_damage((uint32_t)dirty.x0, (uint32_t)dirty.y0, (uint32_t)(dirty.x1 - dirty.x0),
                     (uint32_t)(dirty.y1 - dirty.y0));
    }
    bool overlap = dirty.x0 < dirty.x1 && old_cursor.x0 < dirty.x1 && old_cursor.x1 > dirty.x0 &&
                   old_cursor.y0 < dirty.y1 && old_cursor.y1 > dirty.y0;
    if (!moved && !overlap) {
        dirty.x0 = dirty.x1 = 0;
        return;
    }
    if (cursor_shown && moved) {
        struct rect r = {clampi(old_cursor.x0, 0, screen_w), clampi(old_cursor.y0, 0, screen_h),
                         clampi(old_cursor.x1, 0, screen_w), clampi(old_cursor.y1, 0, screen_h)};
        if (r.x0 < r.x1 && r.y0 < r.y1) {
            copy_to_fb(&r);
            pc_fb_damage((uint32_t)r.x0, (uint32_t)r.y0, (uint32_t)(r.x1 - r.x0), (uint32_t)(r.y1 - r.y0));
        }
    }
    cursor_x = mouse_x;
    cursor_y = mouse_y;
    cursor_shown = true;
    draw_cursor();
    int cx0 = clampi(cursor_x, 0, screen_w), cy0 = clampi(cursor_y, 0, screen_h);
    int cx1 = clampi(cursor_x + CURSOR_W, 0, screen_w), cy1 = clampi(cursor_y + CURSOR_H, 0, screen_h);
    if (cx0 < cx1 && cy0 < cy1)
        pc_fb_damage((uint32_t)cx0, (uint32_t)cy0, (uint32_t)(cx1 - cx0), (uint32_t)(cy1 - cy0));
    dirty.x0 = dirty.x1 = 0;
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

static int stack_index(const struct window *w)
{
    for (int i = 0; i < stack_n; i++)
        if (stack[i] == w)
            return i;
    return -1;
}

static void set_focus(struct window *w)
{
    if (w == focused)
        return;
    struct window *old = focused;
    focused = w;
    if (old) {
        send_event(old, VB_FOCUS, 0, 0, 0, 0, 0);
        damage_window_frame(old);
    }
    if (w) {
        send_event(w, VB_FOCUS, 1, 0, 0, 0, 0);
        damage_window_frame(w);
    }
}

static void raise_window(struct window *w)
{
    int i = stack_index(w);
    if (i < 0 || i == stack_n - 1)
        return;
    memmove(stack + i, stack + i + 1, (size_t)(stack_n - 1 - i) * sizeof(stack[0]));
    stack[stack_n - 1] = w;
    damage_window_frame(w);
}

static struct window *window_at(int x, int y)
{
    for (int i = stack_n - 1; i >= 0; i--) {
        struct window *w = stack[i];
        if (x >= w->x && x < w->x + w->w && y >= w->y && y < w->y + w->h)
            return w;
    }
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
    if (focused == w) {
        focused = NULL;
        if (stack_n)
            set_focus(stack[stack_n - 1]);
    }
    munmap((void *)w->pixels, w->bytes);
    free(w);
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
    size_t bytes = (size_t)m->a * (size_t)m->b * 4;
    int fd = shm_open(name, O_RDWR, 0);
    if (fd < 0) {
        send_error(c, VB_ERR_SHM, name);
        return;
    }
    struct stat st;
    if (fstat(fd, &st) == 0 && st.st_size > 0 && (size_t)st.st_size < bytes) {
        close(fd);
        send_error(c, VB_ERR_SHM, name);
        return;
    }
    void *map = mmap(NULL, bytes, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (map == MAP_FAILED || !map) {
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
    w->pixels = map;
    w->bytes = bytes;
    if (m->c >= 0 && m->d >= 0) {
        w->x = m->c;
        w->y = m->d;
    } else {
        int span = screen_w - w->w > 0 ? screen_w - w->w : 1;
        w->x = clampi((screen_w - w->w) / 2 - 120 + cascade * 32 % span, 0, span);
        w->y = clampi((screen_h - w->h) / 2 - 90 + cascade * 32 % (screen_h - w->h > 0 ? screen_h - w->h : 1), 0,
                      screen_h > w->h ? screen_h - w->h : 0);
        cascade = (cascade + 1) % 8;
    }
    stack[stack_n++] = w;
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
    set_focus(w);
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
        c->hello = m->a == VB_PROTO_VERSION;
        if (!c->hello)
            r.type = VB_ERROR, r.a = VB_ERR_VERSION;
        send_msg(c, &r);
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
        }
        break;
    case VB_DESTROY:
        if ((w = find_window(c, m->window)))
            destroy_window(w);
        break;
    case VB_MOVE:
        if ((w = find_window(c, m->window)) && (mouse_buttons & VB_BUTTON_LEFT))
            start_drag(w);
        break;
    case VB_TITLE:
        if ((w = find_window(c, m->window))) {
            memcpy(w->title, m->text, sizeof(w->title));
            w->title[sizeof(w->title) - 1] = 0;
        }
        break;
    case VB_RAISE:
        if ((w = find_window(c, m->window))) {
            raise_window(w);
            set_focus(w);
        }
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
    if (drag) {
        damage_window_frame(drag);
        drag->x = mouse_x - drag_dx;
        drag->y = mouse_y - drag_dy;
        damage_window_frame(drag);
        if (!(mouse_buttons & VB_BUTTON_LEFT)) {
            pointer_to(drag, changed, 0);
            drag = NULL;
        }
        return;
    }
    struct window *under = window_at(mouse_x, mouse_y);
    if (pressed) {
        if (under) {
            raise_window(under);
            set_focus(under);
            if ((mods & VB_MOD_ALT) && (pressed & VB_BUTTON_LEFT)) {
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
    if (stack_n < 2)
        return;
    struct window *bottom = stack[0];
    raise_window(bottom);
    set_focus(bottom);
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
        if (focused)
            send_event(focused, VB_CLOSE, 0, 0, 0, 0, 0);
        return;
    }
    if (r->pressed && alt && !ctrl && r->scancode == HID_TAB) {
        cycle_focus();
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
    add_dirty(0, 0, screen_w, screen_h);
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
