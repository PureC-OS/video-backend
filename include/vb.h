#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "vb_proto.h"

struct vb_window;

enum vb_event_type {
    VB_EVENT_KEY = 1,
    VB_EVENT_POINTER,
    VB_EVENT_ENTER,
    VB_EVENT_LEAVE,
    VB_EVENT_FOCUS,
    VB_EVENT_CLOSE,
    VB_EVENT_DISCONNECT,
    VB_EVENT_WORKAREA,
    VB_EVENT_TASK,
    VB_EVENT_TASK_GONE,
    VB_EVENT_POPUP_DONE,
    VB_EVENT_CONFIGURE,
    VB_EVENT_SCREEN,
};

struct vb_event {
    int type;
    struct vb_window *window;
    int x, y;
    uint32_t buttons;
    uint32_t changed;
    int wheel;
    int key;
    bool pressed;
    bool repeat;
    uint8_t mods;
    bool focused;
    uint32_t task_id;
    uint32_t task_flags;
    char title[VB_TEXT_MAX];
    int width, height;
    int state;
};

int vb_connect(void);
void vb_disconnect(void);
int vb_fd(void);
void vb_screen_size(int *width, int *height);

struct vb_window *vb_window_create(int width, int height, const char *title);
struct vb_window *vb_window_create_at(int x, int y, int width, int height, const char *title);
struct vb_window *vb_window_create_role(int x, int y, int width, int height, const char *title, int role);
void vb_work_area(int *x, int *y, int *width, int *height);
void vb_tasks_subscribe(void);
void vb_task_activate(uint32_t task_id);
void vb_task_minimize(uint32_t task_id);
uint32_t vb_window_id(const struct vb_window *window);
void vb_window_position(const struct vb_window *window, int *x, int *y);
void vb_window_destroy(struct vb_window *window);
uint32_t *vb_window_pixels(struct vb_window *window);
int vb_window_width(const struct vb_window *window);
int vb_window_height(const struct vb_window *window);
void vb_window_damage(struct vb_window *window, int x, int y, int width, int height);
void vb_window_damage_all(struct vb_window *window);
void vb_window_title(struct vb_window *window, const char *title);
void vb_window_move(struct vb_window *window);
void vb_window_raise(struct vb_window *window);
int vb_server_version(void);
void vb_set_mode(int width, int height);
int vb_window_resize(struct vb_window *window, int width, int height);
void vb_window_set_state(struct vb_window *window, int state);
void vb_window_start_resize(struct vb_window *window, uint32_t edges);
void vb_window_set_cursor(struct vb_window *window, int cursor);
void *vb_window_user(struct vb_window *window);
void vb_window_set_user(struct vb_window *window, void *user);

int vb_next_event(struct vb_event *event, int timeout_ms);
char vb_key_char(const struct vb_event *event);
