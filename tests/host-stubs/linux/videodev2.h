/*
 * A minimal stand-in for <linux/videodev2.h>, for host-side syntax checking
 * only.
 *
 * This exists for one reason: MinGW ships no Linux kernel headers at all (there
 * is no system <linux/...> anywhere under its include tree), so
 *
 *     gcc -fsyntax-only src/v4l2_mpp_encode.c
 *
 * fails before it ever gets to look at our code. src/v4l2_mpp_encode.c pulls in
 * src/v4l2_capture.c, which is where the V4L2 dependency enters. Without this
 * header the main encoder program -- by far the largest integration surface we
 * have -- could never be checked on the host, and every edit to it would be a
 * blind edit.
 *
 * Scope: the declarations below are exactly the set that src/v4l2_capture.c
 * actually names. Nothing more. That is deliberate -- a stub that guesses at
 * the real header's full surface is a stub that will silently accept code the
 * real header would reject, and we would rather it fail loudly than lie.
 *
 * What it does NOT do: it is not ABI-compatible with the kernel, the ioctl
 * numbers are not the real ones, and _IOWR is deliberately borrowed from
 * sys/ioctl.h rather than redefined. This file must never be used to build
 * anything that runs. It is a parser input.
 */

#ifndef HOST_STUB_LINUX_VIDEODEV2_H
#define HOST_STUB_LINUX_VIDEODEV2_H

#include <stdint.h>
#include <sys/ioctl.h>   /* _IOWR, _IOR, _IOW */

#define VIDEO_MAX_PLANES 8U

/* --- buffer types ------------------------------------------------------- */

enum v4l2_buf_type {
    V4L2_BUF_TYPE_VIDEO_CAPTURE        = 1,
    V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE = 9,
};

enum v4l2_memory {
    V4L2_MEMORY_MMAP  = 1,
    V4L2_MEMORY_USERPTR = 2,
    V4L2_MEMORY_OVERLAY = 3,
    V4L2_MEMORY_DMABUF = 4,
};

enum v4l2_field {
    V4L2_FIELD_ANY = 0,
};

/* --- device capabilities ------------------------------------------------ */

#define V4L2_CAP_VIDEO_CAPTURE        0x00000001U
#define V4L2_CAP_VIDEO_CAPTURE_MPLANE 0x00001000U
#define V4L2_CAP_STREAMING            0x04000000U
#define V4L2_CAP_READWRITE            0x01000000U
#define V4L2_CAP_TIMEPERFRAME         0x00001000U

/* --- pixel formats (fourcc) --------------------------------------------- */

#define V4L2_PIX_FMT_NV12  v4l2_fourcc('N', 'V', '1', '2')
#define V4L2_PIX_FMT_NV12M v4l2_fourcc('N', 'M', '1', '2')

#define v4l2_fourcc(a, b, c, d) \
    ((uint32_t)(a) | ((uint32_t)(b) << 8) | \
     ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

/* --- format description flags ------------------------------------------- */

#define V4L2_FMT_FLAG_COMPRESSED 0x0001U

/* --- frame size enumeration types --------------------------------------- */

enum v4l2_frmsizetypes {
    V4L2_FRMSIZE_TYPE_DISCRETE   = 1,
    V4L2_FRMSIZE_TYPE_CONTINUOUS = 2,
    V4L2_FRMSIZE_TYPE_STEPWISE   = 3,
};

/* --- ioctl request numbers ---------------------------------------------- */
/*
 * Not the real numbers. Nothing here is ever passed to a kernel, and the only
 * thing that matters for a syntax check is that they are distinct integer
 * constants usable in a switch/if. Kept in a separate number space (0x56xx)
 * purely so a stray real-looking constant would be obvious.
 */
#define VIDIOC_QUERYCAP       _IOWR('V', 0, struct v4l2_capability)
#define VIDIOC_ENUM_FMT       _IOWR('V', 2, struct v4l2_fmtdesc)
#define VIDIOC_G_FMT          _IOWR('V', 4, struct v4l2_format)
#define VIDIOC_S_FMT          _IOWR('V', 5, struct v4l2_format)
#define VIDIOC_REQBUFS        _IOWR('V', 8, struct v4l2_requestbuffers)
#define VIDIOC_QUERYBUF       _IOWR('V', 9, struct v4l2_buffer)
#define VIDIOC_QBUF           _IOWR('V', 15, struct v4l2_buffer)
#define VIDIOC_DQBUF          _IOWR('V', 17, struct v4l2_buffer)
#define VIDIOC_STREAMON       _IOW('V', 18, int)
#define VIDIOC_STREAMOFF      _IOW('V', 19, int)
#define VIDIOC_G_PARM         _IOWR('V', 21, struct v4l2_streamparm)
#define VIDIOC_S_PARM         _IOWR('V', 22, struct v4l2_streamparm)
#define VIDIOC_ENUM_FRAMESIZES _IOWR('V', 74, struct v4l2_frmsizeenum)

/* --- structures --------------------------------------------------------- */

struct v4l2_capability {
    uint8_t  driver[16];
    uint8_t  card[32];
    uint8_t  bus_info[32];
    uint32_t version;
    uint32_t capabilities;
    uint32_t device_caps;
    uint32_t reserved[3];
};

struct v4l2_fmtdesc {
    uint32_t index;
    uint32_t type;
    uint32_t flags;
    uint8_t  description[32];
    uint32_t pixelformat;
    uint32_t reserved[4];
};

struct v4l2_frmsize_discrete {
    uint32_t width;
    uint32_t height;
};

struct v4l2_frmsize_stepwise {
    uint32_t min_width;
    uint32_t min_height;
    uint32_t max_width;
    uint32_t max_height;
    uint32_t step_width;
    uint32_t step_height;
};

struct v4l2_frmsizeenum {
    uint32_t index;
    uint32_t pixel_format;
    uint32_t type;
    union {
        struct v4l2_frmsize_discrete discrete;
        struct v4l2_frmsize_stepwise stepwise;
    };
    uint32_t reserved[2];
};

struct v4l2_pix_format {
    uint32_t width;
    uint32_t height;
    uint32_t pixelformat;
    uint32_t field;
    uint32_t bytesperline;
    uint32_t sizeimage;
    uint32_t colorspace;
    uint32_t priv;
    uint32_t flags;
    uint32_t ycbcr_enc;
    uint32_t quantization;
    uint32_t xfer_func;
};

struct v4l2_plane_pix_format {
    uint32_t sizeimage;
    uint32_t bytesperline;
    uint16_t reserved[6];
};

struct v4l2_pix_format_mplane {
    uint32_t width;
    uint32_t height;
    uint32_t pixelformat;
    uint32_t field;
    uint32_t colorspace;
    struct v4l2_plane_pix_format plane_fmt[VIDEO_MAX_PLANES];
    uint8_t  num_planes;
    uint8_t  flags;
    uint8_t  ycbcr_enc;
    uint8_t  quantization;
    uint8_t  xfer_func;
    uint8_t  reserved[7];
};

struct v4l2_format {
    uint32_t type;
    union {
        struct v4l2_pix_format        pix;
        struct v4l2_pix_format_mplane pix_mp;
        uint8_t reserved[200];
    } fmt;
};

struct v4l2_requestbuffers {
    uint32_t count;
    uint32_t type;
    uint32_t memory;
    uint32_t capabilities;
    uint32_t reserved[1];
};

struct v4l2_plane {
    uint32_t bytesused;
    uint32_t length;
    union {
        uint32_t mem_offset;
        unsigned long userptr;
        int      fd;
    } m;
    uint32_t data_offset;
    uint32_t reserved[11];
};

struct v4l2_buffer {
    uint32_t index;
    uint32_t type;
    uint32_t bytesused;
    uint32_t flags;
    uint32_t field;
    struct { int32_t tv_sec; int32_t tv_usec; } timestamp;
    struct { int32_t tv_sec; int32_t tv_usec; } timecode;
    uint32_t sequence;
    uint32_t memory;
    union {
        uint32_t offset;
        unsigned long userptr;
        struct v4l2_plane *planes;
        int fd;
    } m;
    uint32_t length;
    uint32_t reserved2;
    union { int32_t request_fd; uint32_t request; };
    uint32_t reserved;
};

struct v4l2_captureparm {
    uint32_t capability;
    uint32_t capturemode;
    struct { uint32_t numerator; uint32_t denominator; } timeperframe;
    uint32_t extendedmode;
    uint32_t readbuffers;
    uint32_t reserved[4];
};

struct v4l2_streamparm {
    uint32_t type;
    union {
        struct v4l2_captureparm capture;
        uint8_t reserved[200];
    } parm;
};

#endif /* HOST_STUB_LINUX_VIDEODEV2_H */
