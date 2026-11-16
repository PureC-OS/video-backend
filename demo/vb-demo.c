#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vb.h"

#define TITLE_H 28
#define CLOSE_W 28
#define MAX_DEMO 8

struct demo {
    struct vb_window *win;
    int w, h;
    uint32_t accent;
    bool focused;
    int px, py;
    bool inside;
    int keys;
    char typed[64];
    int typed_n;
};

static const uint32_t accents[] = {0x5B8DEF, 0x3FB27F, 0xE0A040, 0xD45D79, 0x9C6ADE, 0x2BB3C0, 0xC0C040, 0x8090A0};

static void fill(struct demo *d, int x, int y, int w, int h, uint32_t color)
{
    uint32_t *p = vb_window_pixels(d->win);
    for (int yy = y < 0 ? 0 : y; yy < y + h && yy < d->h; yy++)
        for (int xx = x < 0 ? 0 : x; xx < x + w && xx < d->w; xx++)
            p[(size_t)yy * (size_t)d->w + (size_t)xx] = color;
}

static uint32_t dim(uint32_t c, int percent)
{
    uint32_t r = ((c >> 16) & 0xFF) * (uint32_t)percent / 100, g = ((c >> 8) & 0xFF) * (uint32_t)percent / 100,
             b = (c & 0xFF) * (uint32_t)percent / 100;
    return (r << 16) | (g << 8) | b;
}

static void draw(struct demo *d)
{
    uint32_t *p = vb_window_pixels(d->win);
    for (int y = TITLE_H; y < d->h; y++)
        for (int x = 0; x < d->w; x++) {
            uint32_t r = (uint32_t)(x * 80 / d->w) + 20, g = (uint32_t)(y * 80 / d->h) + 24;
            p[(size_t)y * (size_t)d->w + (size_t)x] = (r << 16) | (g << 8) | 0x40;
        }
    fill(d, 0, 0, d->w, TITLE_H, d->focused ? d->accent : dim(d->accent, 45));
    fill(d, d->w - CLOSE_W, 0, CLOSE_W, TITLE_H, d->focused ? 0xC84040 : 0x703030);
    for (int i = 0; i < 12; i++) {
        fill(d, d->w - CLOSE_W + 8 + i, 8 + i, 2, 2, 0xFFFFFF);
        fill(d, d->w - CLOSE_W + 19 - i, 8 + i, 2, 2, 0xFFFFFF);
    }
    for (int i = 0; i < d->typed_n; i++) {
        unsigned char c = (unsigned char)d->typed[i];
        uint32_t color = ((c * 97u) & 0xFF) << 16 | ((c * 57u) & 0xFF) << 8 | ((c * 31u) & 0xFF) | 0x404040;
        fill(d, 12 + (i % 24) * 18, TITLE_H + 12 + (i / 24) * 22, 14, 18, color);
    }
    if (d->inside && d->py >= TITLE_H)
        fill(d, d->px - 4, d->py - 4, 9, 9, 0xFFFFFF);
    vb_window_damage_all(d->win);
}

static struct demo *find(struct demo *demos, int n, struct vb_window *w)
{
    for (int i = 0; i < n; i++)
        if (demos[i].win == w)
            return &demos[i];
    return NULL;
}

int main(int argc, char **argv)
{
    int count = argc > 1 ? atoi(argv[1]) : 2;
    if (count < 1)
        count = 1;
    if (count > MAX_DEMO)
        count = MAX_DEMO;
    if (vb_connect() < 0) {
        fprintf(stderr, "vb-demo: cannot connect to videod (%s)\n", VB_SOCKET_PATH);
        return 1;
    }
    int sw, sh;
    vb_screen_size(&sw, &sh);
    printf("vb-demo: screen %dx%d, %d window(s); drag the title bar, Alt+drag anywhere, q or the red box closes\n",
           sw, sh, count);
    struct demo demos[MAX_DEMO];
    memset(demos, 0, sizeof(demos));
    int open = 0;
    for (int i = 0; i < count; i++) {
        struct demo *d = &demos[i];
        d->w = 420 + i * 20;
        d->h = 280 + i * 10;
        d->accent = accents[i % 8];
        char title[32];
        snprintf(title, sizeof(title), "vb-demo %d", i + 1);
        d->win = vb_window_create(d->w, d->h, title);
        if (!d->win) {
            fprintf(stderr, "vb-demo: cannot create window %d\n", i + 1);
            continue;
        }
        open++;
        d->focused = i == count - 1;
        draw(d);
    }
    while (open > 0) {
        struct vb_event e;
        int r = vb_next_event(&e, 1000);
        if (r < 0) {
            fprintf(stderr, "vb-demo: videod went away\n");
            return 1;
        }
        if (r == 0)
            continue;
        struct demo *d = find(demos, count, e.window);
        if (!d || !d->win)
            continue;
        bool close_it = false;
        switch (e.type) {
        case VB_EVENT_FOCUS:
            d->focused = e.focused;
            break;
        case VB_EVENT_ENTER:
        case VB_EVENT_POINTER:
            d->inside = true;
            d->px = e.x;
            d->py = e.y;
            if (e.type == VB_EVENT_POINTER && (e.changed & e.buttons & VB_BUTTON_LEFT) && e.y < TITLE_H) {
                if (e.x >= d->w - CLOSE_W)
                    close_it = true;
                else
                    vb_window_move(d->win);
            }
            break;
        case VB_EVENT_LEAVE:
            d->inside = false;
            break;
        case VB_EVENT_KEY: {
            char c = vb_key_char(&e);
            if (c == 'q' || c == 0x1b)
                close_it = true;
            else if (c == '\b' && d->typed_n)
                d->typed_n--;
            else if (c >= ' ' && d->typed_n < (int)sizeof(d->typed))
                d->typed[d->typed_n++] = c;
            if (e.pressed)
                d->keys++;
            break;
        }
        case VB_EVENT_CLOSE:
            close_it = true;
            break;
        default:
            break;
        }
        if (close_it) {
            vb_window_destroy(d->win);
            d->win = NULL;
            open--;
            continue;
        }
        draw(d);
    }
    vb_disconnect();
    return 0;
}
