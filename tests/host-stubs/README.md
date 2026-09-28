# host-stubs

MinGW does not ship `sys/socket.h`, `poll.h`, `netinet/in.h`, `arpa/inet.h`,
`sys/mman.h` or `linux/videodev2.h`, all of which the board sources need. These
stubs provide just enough of each to let `gcc -fsyntax-only` walk a file and
report syntax errors and warnings. They are **not** usable for linking or
running anything.

Pair them with `extra.h`, which fills in the POSIX constants MinGW's own
headers leave out:

```
gcc -fsyntax-only -Isrc -Itests/host-stubs -include extra.h src/rtsp_server.c
```

## linux/videodev2.h

`src/v4l2_mpp_encode.c` is the integration point for nearly everything we add
(sinks, RTSP, the OSD feed), and it reaches `linux/videodev2.h` through
`src/v4l2_capture.c`. MinGW has no Linux kernel headers anywhere in its tree, so
for a long time that file could not be parsed on the host at all and every edit
to it was a blind edit.

`linux/videodev2.h` here declares **only** the names `src/v4l2_capture.c`
actually uses. That is a deliberate limit: a stub that guesses at the real
header's full surface would accept code the real header rejects, and a check
that cannot fail is worse than no check. What it does not model is the ABI --
the ioctl numbers are not the kernel's and this must never be compiled into
something that runs.

`sys/ioctl.h` carries the `_IO`/`_IOR`/`_IOW`/`_IOWR` request-number builders
for the same reason: the real ones live in Linux's `<sys/ioctl.h>` and MinGW has
no equivalent, so the stub above cannot encode a request without them.

So these two are now checkable on Windows, via `make host-syntax` and
`make board-flags`:

```
make host-syntax        # the file, with test flags
make board-flags        # the file, with the board's flags
make host-syntax-can-fail   # proves the stub rejects a broken encoder
```

`host-syntax-can-fail` exists because a stub we wrote ourselves can rot: if it
drifts from the source, the other two targets keep printing success while
checking nothing. It copies the encoder, misspells a struct member, and asserts
the compiler rejects it. If that target ever fails, do not trust the other two
until the stub is fixed.
