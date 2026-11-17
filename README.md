# video-backend

Display server of PureC OS. `videod` owns the screen through the kernel
system calls (`DISPLAY_CLAIM`, `FB_MAP`, `INPUT_OPEN`, `FB_DAMAGE`,
`DISPLAY_RELEASE`) and the standard libc; programs draw into their own
windows and talk to it over `/run/videod.sock` with `libvb`.

## Build

From the PureC-OS tree (this repository is its `video-backend` submodule):

    make -C video-backend            # bin/video-backend/{videod,libvb.a}
    make -C video-backend package    # video-backend-VERSION-1-x86_64.pkg.tar.gz (+ .sig)

Publish the package with `mk/pkg/publish.py --registry Trusted` (or attach it
and its `.sig` to a GitHub release), then on PureC OS:

    pkg -Sy && pkg -S video-backend
    systemctl start videod           # or: systemctl set-default graphical.target

## How it works

- Window contents live in shared memory (`shm_open`, XRGB, created by the
  client); the client sends `DAMAGE` with the changed rectangle.
- videod composes the windows (z-order; protocol v1/v2 clients get a 1 px
  border, the focused one in blue) in a back buffer. Only finished frames
  reach the framebuffer: the changed rows are copied from the back buffer
  with the pointer already laid over them, so the screen never shows a frame
  without the pointer or a half-drawn update.
- Input: keys go to the focused window, pointer events to the window under
  the pointer (or the one that got the button press). Click raises and
  focuses.
- Keys handled by videod: Alt+drag moves a window, Alt+Tab cycles windows,
  Alt+F4 asks the focused window to close, Ctrl+Alt+Backspace stops videod
  and gives the screen back to the text consoles.

Protocol: `include/vb_proto.h`. Client API: `include/vb.h`.
