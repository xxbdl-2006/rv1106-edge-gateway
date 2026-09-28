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

/*
 * The _IO/_IOR/_IOW/_IOWR request-number builders live in the real
 * <sys/ioctl.h> on Linux. MinGW has no equivalent, so any header that encodes
 * an ioctl number -- our <linux/videodev2.h> stub, for instance -- needs these
 * declared before it can be included.
 *
 * The encodings below mirror the Linux _IOC layout (direction, size, type, nr)
 * closely enough that distinct requests stay distinct, which is all a syntax
 * check cares about. They are not the real numbers and are never passed to a
 * kernel. _IOC_TYPECHECK is Linux's compile-time "the third argument must be a
 * type that fits in 14 bits" guard; here it is an identity macro, because
 * reproducing sizeof() in a preprocessor-adjacent macro buys nothing when the
 * output is only parsed.
 */
#define _IOC_NRBITS   8
#define _IOC_TYPEBITS 8
#define _IOC_SIZEBITS 14

#define _IOC_NRSHIFT   0
#define _IOC_TYPESHIFT (_IOC_NRSHIFT + _IOC_NRBITS)
#define _IOC_SIZESHIFT (_IOC_TYPESHIFT + _IOC_TYPEBITS)
#define _IOC_DIRSHIFT  (_IOC_SIZESHIFT + _IOC_SIZEBITS)

#define _IOC_NONE  0U
#define _IOC_WRITE 1U
#define _IOC_READ  2U

#define _IOC_TYPECHECK(t) (sizeof(t))

#define _IOC(dir, type, nr, size) \
    (((dir) << _IOC_DIRSHIFT) | ((type) << _IOC_TYPESHIFT) | \
     ((nr) << _IOC_NRSHIFT) | ((size) << _IOC_SIZESHIFT))

#define _IO(type, nr)        _IOC(_IOC_NONE, (type), (nr), 0U)
#define _IOR(type, nr, size) _IOC(_IOC_READ, (type), (nr), _IOC_TYPECHECK(size))
#define _IOW(type, nr, size) _IOC(_IOC_WRITE, (type), (nr), _IOC_TYPECHECK(size))
#define _IOWR(type, nr, size) \
    _IOC(_IOC_READ | _IOC_WRITE, (type), (nr), _IOC_TYPECHECK(size))

#endif
