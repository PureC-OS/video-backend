#pragma once
#include <stdint.h>

#define VB_SOCKET_PATH "/run/videod.sock"
#define VB_PROTO_VERSION 3
#define VB_PROTO_MIN_VERSION 1
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
    VB_TASKS_SUBSCRIBE,
    VB_ACTIVATE,
    VB_MINIMIZE,
    VB_RESIZE,
    VB_SET_STATE,
    VB_RESIZE_START,
    VB_CURSOR,
    VB_SET_MODE,
};

enum vb_state {
    VB_STATE_NORMAL = 0,
    VB_STATE_MAXIMIZED = 1,
    VB_STATE_FULLSCREEN = 2,
    VB_STATE_MINIMIZED = 3,
};

#define VB_EDGE_LEFT 1u
#define VB_EDGE_RIGHT 2u
#define VB_EDGE_TOP 4u
#define VB_EDGE_BOTTOM 8u

enum vb_cursor {
    VB_CURSOR_ARROW = 0,
    VB_CURSOR_RESIZE_H,
    VB_CURSOR_RESIZE_V,
    VB_CURSOR_RESIZE_NWSE,
    VB_CURSOR_RESIZE_NESW,
    VB_CURSOR_MOVE,
    VB_CURSOR_COUNT,
};

enum vb_role {
    VB_ROLE_NORMAL = 0,
    VB_ROLE_BACKGROUND = 1,
    VB_ROLE_PANEL = 2,
    VB_ROLE_POPUP = 3,
};

#define VB_TASK_FOCUSED 1u
#define VB_TASK_MINIMIZED 2u

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
    VB_WORKAREA,
    VB_TASK,
    VB_TASK_GONE,
    VB_POPUP_DONE,
    VB_CONFIGURE,
    VB_RESIZED,
    VB_SCREEN,
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
 *   CREATE   a=width b=height c=x d=y (-1 = automatic) e=vb_role text=shared memory name
 *            The client creates the memory (width*height*4 bytes, XRGB) with shm_open.
 *            Roles: NORMAL windows are tasks; BACKGROUND stays below them and may take
 *            the keyboard; PANEL stays above them, never takes the keyboard, and a
 *            full-width panel at the top or bottom edge shrinks the work area; POPUP
 *            stays above panels and gets POPUP_DONE when the user clicks elsewhere.
 *   DAMAGE   window, a=x b=y c=width d=height (window coordinates)
 *   DESTROY  window
 *   MOVE     window: start moving the window with the pointer while the left button is held
 *   TITLE    window, text=title
 *   RAISE    window: raise and focus
 *   TASKS_SUBSCRIBE  receive TASK / TASK_GONE for every normal window (also the current ones)
 *   ACTIVATE window=any task id: show, raise and focus it
 *   MINIMIZE window=any task id: hide it
 *   RESIZE   window, a=width b=height text=shared memory name: replace the buffer (version 3)
 *   SET_STATE window, a=vb_state: maximize, restore, fullscreen or minimize (version 3)
 *   RESIZE_START window, a=VB_EDGE_* bits: resize with the pointer while the left button is held (version 3)
 *   CURSOR   window, a=vb_cursor: pointer shape over this window (version 3)
 *   SET_MODE a=width b=height: change the display mode and remember it (version 3)
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
 *   WORKAREA a=x b=y c=width d=height: screen without the panels (sent after WELCOME and on change)
 *   TASK     window=task id, a=VB_TASK_* flags, text=title (new or changed)
 *   TASK_GONE window=task id
 *   POPUP_DONE window: a click landed outside this popup
 *   CONFIGURE window, a=x b=y: the window was moved (sent when a move ends);
 *            version 3 also c=width d=height (0 = keep) e=vb_state: the size the window should take
 *   RESIZED  window, a=width b=height text=shared memory name: the new buffer of RESIZE is in use
 *   SCREEN   a=width b=height: the display mode changed (to every client); ERROR a=VB_ERR_SIZE text="mode"
 *            answers a SET_MODE that failed
 */
