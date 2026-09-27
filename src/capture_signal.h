#ifndef CAPTURE_SIGNAL_H
#define CAPTURE_SIGNAL_H

#include <signal.h>

/*
 * Process wide stop flag, raised by the SIGINT/SIGTERM handler that
 * v4l2_capture.c installs (see on_signal()).
 *
 * It lives in its own header because three pieces of code now share it: the
 * signal handler, the V4L2 capture loop, and the capture thread that has to
 * break out of a blocking DQBUF poll as soon as a signal arrives. The
 * alternative was for every one of them to declare its own copy of the flag,
 * which is exactly the kind of bug that looks like a hang.
 */
extern volatile sig_atomic_t g_stop;

#endif
