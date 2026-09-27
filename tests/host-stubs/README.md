# host-stubs

MinGW does not ship `sys/socket.h`, `poll.h`, `netinet/in.h`, `arpa/inet.h` or
`sys/mman.h`, all of which the board sources need. These stubs provide just
enough of each to let `gcc -fsyntax-only` walk a file and report syntax errors
and warnings. They are **not** usable for linking or running anything.

Pair them with `extra.h`, which fills in the POSIX constants MinGW's own
headers leave out:

```
gcc -fsyntax-only -Isrc -Itests/host-stubs -include extra.h src/rtsp_server.c
```

`src/v4l2_capture.c` and `src/v4l2_mpp_encode.c` additionally need
`linux/videodev2.h`, which MinGW does not provide at all. Those two files can
therefore only be checked on Linux, or by a cross compiler. Their host-testable
logic lives in `src/frame_ring.c` and `src/capture_thread.c`, which are covered
by `make test-frame-ring` and `make test-capture-thread`.
