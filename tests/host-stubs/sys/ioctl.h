#ifndef HOST_STUB_SYS_IOCTL_H
#define HOST_STUB_SYS_IOCTL_H

/*
 * Stub of <sys/ioctl.h> for `make host-syntax` on Windows.
 *
 * MinGW ships ioctl() in <io.h> with a different signature, so include the
 * real one and then declare the POSIX-shaped wrapper the driver calls.
 * Nothing here runs; the file is only ever compiled with -fsyntax-only.
 */

#include <io.h>

extern int ioctl(int fd, unsigned long request, ...);

#endif
