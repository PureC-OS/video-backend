#pragma once
#include <stdint.h>

#define VB_SOCKET_PATH "/run/videod.sock"
#define VB_PROTO_VERSION 1
#define VB_TEXT_MAX 64
#define VB_MAX_SIZE 4096

enum vb_request {
    VB_HELLO = 1,
    VB_CREATE,
    VB_DAMAGE,
    VB_DESTROY,
    VB_MOVE,
    VB_TITLE,
    VB_RAISE,
};

enum vb_reply {
    VB_WELCOME = 64,
    VB_CREATED,
    VB_KEY,
    VB_POINTER,
    VB_ENTER,
    VB_LEAVE,
    VB_FOCUS,
    VB_CLOSE,
    VB_ERROR,
};

enum vb_error {
    VB_ERR_VERSION = 1,
    VB_ERR_SIZE,
    VB_ERR_SHM,
    VB_ERR_LIMIT,
    VB_ERR_WINDOW,
};

#define VB_BUTTON_LEFT 1u
#define VB_BUTTON_RIGHT 2u
#define VB_BUTTON_MIDDLE 4u

#define VB_MOD_SHIFT 0x11u
#define VB_MOD_CTRL 0x22u
#define VB_MOD_ALT 0x44u
#define VB_MOD_GUI 0x88u

struct vb_msg {
    uint32_t type;
    uint32_t window;
    int32_t a;
    int32_t b;
    int32_t c;
    int32_t d;
    uint32_t e;
    uint32_t f;
    char text[VB_TEXT_MAX];
};

/*
 * Requests (client -> videod)
 *   HELLO    a=version
 *   CREATE   a=width b=height c=x d=y (-1 = automatic) text=shared memory name
 *            The client creates the memory (width*height*4 bytes, XRGB) with shm_open.
 *   DAMAGE   window, a=x b=y c=width d=height (window coordinates)
 *   DESTROY  window
 *   MOVE     window: start moving the window with the pointer while the left button is held
 *   TITLE    window, text=title
 *   RAISE    window: raise and focus
 *
 * Replies and events (videod -> client)
 *   WELCOME  a=screen width b=screen height c=version
 *   CREATED  window=id a=x b=y c=width d=height, text=shared memory name of the request
 *   KEY      window, a=HID usage b=pressed c=repeat d=modifiers
 *   POINTER  window, a=x b=y (window coordinates) c=buttons d=wheel e=buttons that changed
 *   ENTER, LEAVE  window
 *   FOCUS    window, a=1 focused / 0 not
 *   CLOSE    window: the user asked to close it
 *   ERROR    a=vb_error, text=shared memory name of a failed CREATE
 */
