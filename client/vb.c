#include "vb.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define QUEUE_MAX 256

struct vb_window {
    uint32_t id;
    int x, y;
    int width, height;
    uint32_t *pixels;
    size_t bytes;
    char shm[VB_TEXT_MAX];
    void *user;
    struct vb_window *next;
};

static int sock = -1;
static int screen_w, screen_h;
static int work_x, work_y, work_w, work_h;
static struct vb_window *windows;
static struct vb_msg queue[QUEUE_MAX];
static int queue_head, queue_count;
static char inbuf[sizeof(struct vb_msg)];
static size_t inhave;
static unsigned shm_counter;

int vb_fd(void)
{
    return sock;
}

void vb_screen_size(int *width, int *height)
{
    if (width)
        *width = screen_w;
    if (height)
        *height = screen_h;
}

static int send_msg(const struct vb_msg *m)
{
    const char *p = (const char *)m;
    size_t left = sizeof(*m);
    while (left) {
        ssize_t n = send(sock, p, left, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        p += n;
        left -= (size_t)n;
    }
    return 0;
}

static int read_msg(struct vb_msg *out, int timeout_ms)
{
    for (;;) {
        if (inhave == sizeof(struct vb_msg)) {
            memcpy(out, inbuf, sizeof(*out));
            inhave = 0;
            return 1;
        }
        struct pollfd p = {.fd = sock, .events = POLLIN};
        int r = poll(&p, 1, timeout_ms);
        if (r < 0 && errno == EINTR)
            continue;
        if (r < 0)
            return -1;
        if (r == 0)
            return 0;
        ssize_t n = recv(sock, inbuf + inhave, sizeof(inbuf) - inhave, 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        inhave += (size_t)n;
    }
}

static void enqueue(const struct vb_msg *m)
{
    if (queue_count == QUEUE_MAX) {
        queue_head = (queue_head + 1) % QUEUE_MAX;
        queue_count--;
    }
    queue[(queue_head + queue_count) % QUEUE_MAX] = *m;
    queue_count++;
}

static int wait_for(uint32_t type, const char *text, struct vb_msg *out)
{
    for (;;) {
        struct vb_msg m;
        int r = read_msg(&m, 5000);
        if (r <= 0)
            return -1;
        if (m.type == type && (!text || !strncmp(m.text, text, sizeof(m.text)))) {
            *out = m;
            return 0;
        }
        if (m.type == VB_ERROR && (!text || !strncmp(m.text, text, sizeof(m.text)))) {
            *out = m;
            return -1;
        }
        enqueue(&m);
    }
}

int vb_connect(void)
{
    if (sock >= 0)
        return 0;
    sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0)
        return -1;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", VB_SOCKET_PATH);
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock);
        sock = -1;
        return -1;
    }
    struct vb_msg m;
    memset(&m, 0, sizeof(m));
    m.type = VB_HELLO;
    m.a = VB_PROTO_VERSION;
    struct vb_msg r;
    if (send_msg(&m) < 0 || wait_for(VB_WELCOME, NULL, &r) < 0) {
        close(sock);
        sock = -1;
        return -1;
    }
    screen_w = r.a;
    screen_h = r.b;
    work_x = 0;
    work_y = 0;
    work_w = r.a;
    work_h = r.b;
    return 0;
}

static void free_window(struct vb_window *w)
{
    struct vb_window **p = &windows;
    while (*p && *p != w)
        p = &(*p)->next;
    if (*p)
        *p = w->next;
    if (w->pixels)
        munmap(w->pixels, w->bytes);
    free(w);
}

void vb_disconnect(void)
{
    while (windows)
        free_window(windows);
    if (sock >= 0)
        close(sock);
    sock = -1;
    queue_count = 0;
    inhave = 0;
}

void vb_work_area(int *x, int *y, int *width, int *height)
{
    if (x)
        *x = work_x;
    if (y)
        *y = work_y;
    if (width)
        *width = work_w;
    if (height)
        *height = work_h;
}

uint32_t vb_window_id(const struct vb_window *w)
{
    return w ? w->id : 0;
}

void vb_window_position(const struct vb_window *w, int *x, int *y)
{
    if (x)
        *x = w ? w->x : 0;
    if (y)
        *y = w ? w->y : 0;
}

static struct vb_window *create(int x, int y, int width, int height, const char *title, int role);

struct vb_window *vb_window_create_at(int x, int y, int width, int height, const char *title)
{
    return create(x, y, width, height, title, VB_ROLE_NORMAL);
}

struct vb_window *vb_window_create_role(int x, int y, int width, int height, const char *title, int role)
{
    return create(x, y, width, height, title, role);
}

static struct vb_window *create(int x, int y, int width, int height, const char *title, int role)
{
    if (sock < 0 && vb_connect() < 0)
        return NULL;
    if (width <= 0 || height <= 0 || width > VB_MAX_SIZE || height > VB_MAX_SIZE)
        return NULL;
    struct vb_window *w = calloc(1, sizeof(*w));
    if (!w)
        return NULL;
    w->width = width;
    w->height = height;
    w->bytes = (size_t)width * (size_t)height * 4;
    snprintf(w->shm, sizeof(w->shm), "/vb-%d-%u", (int)getpid(), shm_counter++);
    int fd = shm_open(w->shm, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) {
        free(w);
        return NULL;
    }
    if (ftruncate(fd, (off_t)w->bytes) < 0) {
        close(fd);
        shm_unlink(w->shm);
        free(w);
        return NULL;
    }
    void *map = mmap(NULL, w->bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (map == MAP_FAILED || !map) {
        shm_unlink(w->shm);
        free(w);
        return NULL;
    }
    w->pixels = map;
    memset(w->pixels, 0, w->bytes);
    struct vb_msg m;
    memset(&m, 0, sizeof(m));
    m.type = VB_CREATE;
    m.a = width;
    m.b = height;
    m.c = x;
    m.d = y;
    m.e = (uint32_t)role;
    memcpy(m.text, w->shm, sizeof(m.text));
    struct vb_msg r;
    int ok = send_msg(&m) == 0 && wait_for(VB_CREATED, w->shm, &r) == 0;
    shm_unlink(w->shm);
    if (!ok) {
        munmap(w->pixels, w->bytes);
        free(w);
        return NULL;
    }
    w->id = r.window;
    w->x = r.a;
    w->y = r.b;
    w->next = windows;
    windows = w;
    if (title)
        vb_window_title(w, title);
    return w;
}

struct vb_window *vb_window_create(int width, int height, const char *title)
{
    return vb_window_create_at(-1, -1, width, height, title);
}

static void simple(uint32_t type, const struct vb_window *w, int a, int b, int c, int d, const char *text)
{
    if (sock < 0 || !w)
        return;
    struct vb_msg m;
    memset(&m, 0, sizeof(m));
    m.type = type;
    m.window = w->id;
    m.a = a;
    m.b = b;
    m.c = c;
    m.d = d;
    if (text)
        snprintf(m.text, sizeof(m.text), "%s", text);
    send_msg(&m);
}

static void global(uint32_t type, uint32_t id)
{
    if (sock < 0)
        return;
    struct vb_msg m;
    memset(&m, 0, sizeof(m));
    m.type = type;
    m.window = id;
    send_msg(&m);
}

void vb_tasks_subscribe(void)
{
    global(VB_TASKS_SUBSCRIBE, 0);
}

void vb_task_activate(uint32_t id)
{
    global(VB_ACTIVATE, id);
}

void vb_task_minimize(uint32_t id)
{
    global(VB_MINIMIZE, id);
}

void vb_window_destroy(struct vb_window *w)
{
    if (!w)
        return;
    simple(VB_DESTROY, w, 0, 0, 0, 0, NULL);
    free_window(w);
}

uint32_t *vb_window_pixels(struct vb_window *w)
{
    return w ? w->pixels : NULL;
}

int vb_window_width(const struct vb_window *w)
{
    return w ? w->width : 0;
}

int vb_window_height(const struct vb_window *w)
{
    return w ? w->height : 0;
}

void vb_window_damage(struct vb_window *w, int x, int y, int width, int height)
{
    simple(VB_DAMAGE, w, x, y, width, height, NULL);
}

void vb_window_damage_all(struct vb_window *w)
{
    if (w)
        simple(VB_DAMAGE, w, 0, 0, w->width, w->height, NULL);
}

void vb_window_title(struct vb_window *w, const char *title)
{
    simple(VB_TITLE, w, 0, 0, 0, 0, title);
}

void vb_window_move(struct vb_window *w)
{
    simple(VB_MOVE, w, 0, 0, 0, 0, NULL);
}

void vb_window_raise(struct vb_window *w)
{
    simple(VB_RAISE, w, 0, 0, 0, 0, NULL);
}

void *vb_window_user(struct vb_window *w)
{
    return w ? w->user : NULL;
}

void vb_window_set_user(struct vb_window *w, void *user)
{
    if (w)
        w->user = user;
}

static struct vb_window *by_id(uint32_t id)
{
    for (struct vb_window *w = windows; w; w = w->next)
        if (w->id == id)
            return w;
    return NULL;
}

static bool convert(const struct vb_msg *m, struct vb_event *e)
{
    memset(e, 0, sizeof(*e));
    e->window = by_id(m->window);
    switch (m->type) {
    case VB_KEY:
        e->type = VB_EVENT_KEY;
        e->key = m->a;
        e->pressed = m->b != 0;
        e->repeat = m->c != 0;
        e->mods = (uint8_t)m->d;
        break;
    case VB_POINTER:
        e->type = VB_EVENT_POINTER;
        e->x = m->a;
        e->y = m->b;
        e->buttons = (uint32_t)m->c;
        e->wheel = m->d;
        e->changed = m->e;
        break;
    case VB_ENTER:
        e->type = VB_EVENT_ENTER;
        e->x = m->a;
        e->y = m->b;
        break;
    case VB_LEAVE:
        e->type = VB_EVENT_LEAVE;
        break;
    case VB_FOCUS:
        e->type = VB_EVENT_FOCUS;
        e->focused = m->a != 0;
        break;
    case VB_CLOSE:
        e->type = VB_EVENT_CLOSE;
        break;
    case VB_POPUP_DONE:
        e->type = VB_EVENT_POPUP_DONE;
        break;
    case VB_CONFIGURE:
        e->type = VB_EVENT_CONFIGURE;
        e->x = m->a;
        e->y = m->b;
        if (e->window) {
            e->window->x = m->a;
            e->window->y = m->b;
        }
        break;
    case VB_WORKAREA:
        work_x = m->a;
        work_y = m->b;
        work_w = m->c;
        work_h = m->d;
        e->type = VB_EVENT_WORKAREA;
        e->x = m->a;
        e->y = m->b;
        return true;
    case VB_TASK:
        e->type = VB_EVENT_TASK;
        e->task_id = m->window;
        e->task_flags = (uint32_t)m->a;
        memcpy(e->title, m->text, sizeof(e->title));
        e->title[sizeof(e->title) - 1] = 0;
        e->window = NULL;
        return true;
    case VB_TASK_GONE:
        e->type = VB_EVENT_TASK_GONE;
        e->task_id = m->window;
        e->window = NULL;
        return true;
    default:
        return false;
    }
    return e->window != NULL;
}

int vb_next_event(struct vb_event *event, int timeout_ms)
{
    if (sock < 0) {
        memset(event, 0, sizeof(*event));
        event->type = VB_EVENT_DISCONNECT;
        return -1;
    }
    while (queue_count) {
        struct vb_msg m = queue[queue_head];
        queue_head = (queue_head + 1) % QUEUE_MAX;
        queue_count--;
        if (convert(&m, event))
            return 1;
    }
    for (;;) {
        struct vb_msg m;
        int r = read_msg(&m, timeout_ms);
        if (r < 0) {
            memset(event, 0, sizeof(*event));
            event->type = VB_EVENT_DISCONNECT;
            return -1;
        }
        if (r == 0)
            return 0;
        if (convert(&m, event))
            return 1;
        timeout_ms = 0;
    }
}

char vb_key_char(const struct vb_event *e)
{
    static const char lower[] = "abcdefghijklmnopqrstuvwxyz1234567890\n\x1b\b\t -=[]\\#;'`,./";
    static const char upper[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ!@#$%^&*()\n\x1b\b\t _+{}|~:\"~<>?";
    if (!e || e->type != VB_EVENT_KEY || !e->pressed)
        return 0;
    int k = e->key;
    if (k < 0x04 || k > 0x38)
        return 0;
    bool shift = e->mods & VB_MOD_SHIFT;
    return (shift ? upper : lower)[k - 0x04];
}
