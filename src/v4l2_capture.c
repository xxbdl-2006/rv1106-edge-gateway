#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "capture_signal.h"

#ifndef V4L2_PIX_FMT_NV12M
#define V4L2_PIX_FMT_NV12M v4l2_fourcc('N', 'M', '1', '2')
#endif

#define DEFAULT_DEVICE "/dev/video0"
#define DEFAULT_WIDTH 1280U
#define DEFAULT_HEIGHT 720U
#define DEFAULT_FORMAT "NV12"
#define DEFAULT_FRAMES 1U
#define DEFAULT_BUFFER_COUNT 4U
#define DEFAULT_TIMEOUT_MS 2000
#define MAX_PATH_LEN 1024

enum capture_kind {
    CAPTURE_KIND_AUTO = 0,
    CAPTURE_KIND_SINGLE,
    CAPTURE_KIND_MPLANE,
};

enum dequeue_result {
    DEQUEUE_OK = 0,
    DEQUEUE_ERROR = -1,
    DEQUEUE_TIMEOUT = -2,
    DEQUEUE_STOPPED = -3,
};

struct buffer_slot {
    void *start[VIDEO_MAX_PLANES];
    size_t length[VIDEO_MAX_PLANES];
    unsigned int plane_count;
};

struct config {
    const char *device;
    const char *output;
    uint32_t width;
    uint32_t height;
    uint32_t pixel_format;
    unsigned long frames;
    unsigned long warmup_frames;
    unsigned int buffer_count;
    unsigned int fps;
    int timeout_ms;
    enum capture_kind requested_kind;
    bool raw;
    bool discard;
    bool info;
    bool list_formats;
};

struct frame {
    struct v4l2_buffer buffer;
    struct v4l2_plane planes[VIDEO_MAX_PLANES];
};

/*
 * The capture thread shares this flag, so it has external linkage and lives
 * behind capture_signal.h. Keeping the definition here preserves the original
 * behaviour of the standalone v4l2_capture program, which is still built and
 * used for diagnostics.
 */
volatile sig_atomic_t g_stop;

static void on_signal(int signo)
{
    (void)signo;
    g_stop = 1;
}

static void usage(const char *program)
{
    fprintf(stderr,
            "Usage: %s [options]\n"
            "\n"
            "Capture raw frames from a V4L2 capture device.\n"
            "\n"
            "Options:\n"
            "  -d, --device DEV       capture device (default: %s)\n"
            "  -w, --width N          requested width (default: %u)\n"
            "  -H, --height N         requested height (default: %u)\n"
            "  -f, --format FOURCC    requested pixel format (default: %s)\n"
            "  -n, --frames N         number of frames, 0 means until SIGINT (default: %u)\n"
            "      --warmup N         discard N frames before saving or counting (default: 0)\n"
            "  -b, --buffers N        requested mmap buffer count (default: %u)\n"
            "  -o, --output PATH      output file or prefix (default: frame)\n"
            "      --type TYPE        auto, single, or mplane (default: auto)\n"
            "  -r, --fps N            optional requested frame rate\n"
            "  -t, --timeout MS       DQBUF timeout in milliseconds (default: %d)\n"
            "      --raw              save driver buffer bytes without repacking\n"
            "      --discard          capture and measure without saving frames\n"
            "  -i, --info             print device information and exit\n"
            "  -L, --list-formats     list formats and frame sizes, then exit\n"
            "  -h, --help             show this help\n"
            "\n"
            "For multiple frames, PATH is used as a prefix:\n"
            "  frame-0000.raw, frame-0001.raw, ...\n"
            "If PATH contains %%04u, it is replaced by the frame index.\n"
            "\n"
            "NV12 output is tightly repacked by default, removing row stride\n"
            "padding. Use --raw to inspect the exact driver layout.\n",
            program,
            DEFAULT_DEVICE,
            DEFAULT_WIDTH,
            DEFAULT_HEIGHT,
            DEFAULT_FORMAT,
            DEFAULT_FRAMES,
            DEFAULT_BUFFER_COUNT,
            DEFAULT_TIMEOUT_MS);
}

static int xioctl(int fd, unsigned long request, void *arg)
{
    int ret;

    do {
        ret = ioctl(fd, request, arg);
    } while (ret == -1 && errno == EINTR && !g_stop);

    return ret;
}

static void print_fourcc(uint32_t fourcc)
{
    char text[5];

    text[0] = (char)(fourcc & 0xffU);
    text[1] = (char)((fourcc >> 8) & 0xffU);
    text[2] = (char)((fourcc >> 16) & 0xffU);
    text[3] = (char)((fourcc >> 24) & 0xffU);
    text[4] = '\0';

    printf("%s (%08" PRIx32 ")", text, fourcc);
}

static int parse_fourcc(const char *text, uint32_t *out)
{
    if (text == NULL || out == NULL || strlen(text) != 4U) {
        return -1;
    }

    *out = v4l2_fourcc(text[0], text[1], text[2], text[3]);
    return 0;
}

static int parse_unsigned(const char *text, unsigned long *out)
{
    char *end = NULL;
    unsigned long value;

    if (text == NULL || out == NULL || text[0] == '\0') {
        return -1;
    }

    errno = 0;
    value = strtoul(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0') {
        return -1;
    }

    *out = value;
    return 0;
}

static int parse_capture_kind(const char *text, enum capture_kind *out)
{
    if (strcmp(text, "auto") == 0) {
        *out = CAPTURE_KIND_AUTO;
    } else if (strcmp(text, "single") == 0) {
        *out = CAPTURE_KIND_SINGLE;
    } else if (strcmp(text, "mplane") == 0) {
        *out = CAPTURE_KIND_MPLANE;
    } else {
        return -1;
    }

    return 0;
}

static const char *capture_kind_name(enum capture_kind kind)
{
    switch (kind) {
    case CAPTURE_KIND_SINGLE:
        return "V4L2_BUF_TYPE_VIDEO_CAPTURE";
    case CAPTURE_KIND_MPLANE:
        return "V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE";
    default:
        return "unknown";
    }
}

static enum v4l2_buf_type capture_buf_type(enum capture_kind kind)
{
    if (kind == CAPTURE_KIND_MPLANE) {
        return V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    }

    return V4L2_BUF_TYPE_VIDEO_CAPTURE;
}

static int select_capture_kind(const struct v4l2_capability *caps,
                               enum capture_kind requested,
                               enum capture_kind *selected)
{
    bool single = (caps->capabilities & V4L2_CAP_VIDEO_CAPTURE) != 0U;
    bool mplane = (caps->capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE) != 0U;

    if (requested == CAPTURE_KIND_SINGLE) {
        if (!single) {
            fprintf(stderr, "Device does not support single-planar capture\n");
            return -1;
        }
        *selected = CAPTURE_KIND_SINGLE;
        return 0;
    }

    if (requested == CAPTURE_KIND_MPLANE) {
        if (!mplane) {
            fprintf(stderr, "Device does not support multi-planar capture\n");
            return -1;
        }
        *selected = CAPTURE_KIND_MPLANE;
        return 0;
    }

    if (mplane) {
        *selected = CAPTURE_KIND_MPLANE;
    } else if (single) {
        *selected = CAPTURE_KIND_SINGLE;
    } else {
        fprintf(stderr, "Device has no video capture capability\n");
        return -1;
    }

    return 0;
}

static void print_device_info(const struct v4l2_capability *caps)
{
    printf("Driver       : %s\n", caps->driver);
    printf("Card         : %s\n", caps->card);
    printf("Bus          : %s\n", caps->bus_info);
    printf("Version      : %u.%u.%u\n",
           (caps->version >> 16) & 0xffU,
           (caps->version >> 8) & 0xffU,
           caps->version & 0xffU);
    printf("Capabilities :");
    if ((caps->capabilities & V4L2_CAP_VIDEO_CAPTURE) != 0U) {
        printf(" single-planar-capture");
    }
    if ((caps->capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE) != 0U) {
        printf(" multi-planar-capture");
    }
    if ((caps->capabilities & V4L2_CAP_STREAMING) != 0U) {
        printf(" streaming");
    }
    if ((caps->capabilities & V4L2_CAP_READWRITE) != 0U) {
        printf(" readwrite");
    }
    printf("\n");
}

static void print_frame_size(const struct v4l2_frmsizeenum *size)
{
    switch (size->type) {
    case V4L2_FRMSIZE_TYPE_DISCRETE:
        printf("      %ux%u\n", size->discrete.width, size->discrete.height);
        break;
    case V4L2_FRMSIZE_TYPE_STEPWISE:
    case V4L2_FRMSIZE_TYPE_CONTINUOUS:
        printf("      %ux%u..%ux%u step %ux%u\n",
               size->stepwise.min_width,
               size->stepwise.min_height,
               size->stepwise.max_width,
               size->stepwise.max_height,
               size->stepwise.step_width,
               size->stepwise.step_height);
        break;
    default:
        printf("      unknown size type %u\n", size->type);
        break;
    }
}

static int list_formats(int fd, enum capture_kind kind)
{
    struct v4l2_fmtdesc format;
    unsigned int format_index;

    for (format_index = 0;; format_index++) {
        memset(&format, 0, sizeof(format));
        format.index = format_index;
        format.type = capture_buf_type(kind);

        if (xioctl(fd, VIDIOC_ENUM_FMT, &format) == -1) {
            if (errno == EINVAL) {
                break;
            }
            perror("VIDIOC_ENUM_FMT");
            return -1;
        }

        printf("[%u] ", format.index);
        print_fourcc(format.pixelformat);
        printf("  %s%s\n",
               format.description,
               (format.flags & V4L2_FMT_FLAG_COMPRESSED) != 0U ? " [compressed]" : "");

        for (unsigned int size_index = 0;; size_index++) {
            struct v4l2_frmsizeenum frame_size;

            memset(&frame_size, 0, sizeof(frame_size));
            frame_size.index = size_index;
            frame_size.pixel_format = format.pixelformat;

            if (xioctl(fd, VIDIOC_ENUM_FRAMESIZES, &frame_size) == -1) {
                if (errno == EINVAL) {
                    break;
                }
                perror("VIDIOC_ENUM_FRAMESIZES");
                return -1;
            }

            print_frame_size(&frame_size);
        }
    }

    return 0;
}

static int set_capture_format(int fd,
                              enum capture_kind kind,
                              const struct config *config,
                              struct v4l2_format *actual,
                              unsigned int *plane_count)
{
    struct v4l2_format requested;

    memset(&requested, 0, sizeof(requested));
    requested.type = capture_buf_type(kind);

    if (xioctl(fd, VIDIOC_G_FMT, &requested) == -1) {
        perror("VIDIOC_G_FMT");
        return -1;
    }

    if (kind == CAPTURE_KIND_SINGLE) {
        requested.fmt.pix.width = config->width;
        requested.fmt.pix.height = config->height;
        requested.fmt.pix.pixelformat = config->pixel_format;
        requested.fmt.pix.field = V4L2_FIELD_ANY;
        requested.fmt.pix.bytesperline = 0U;
        requested.fmt.pix.sizeimage = 0U;
    } else {
        requested.fmt.pix_mp.width = config->width;
        requested.fmt.pix_mp.height = config->height;
        requested.fmt.pix_mp.pixelformat = config->pixel_format;
        requested.fmt.pix_mp.field = V4L2_FIELD_ANY;
        if (requested.fmt.pix_mp.num_planes == 0U) {
            requested.fmt.pix_mp.num_planes = 1U;
        }
        for (unsigned int plane = 0; plane < VIDEO_MAX_PLANES; plane++) {
            requested.fmt.pix_mp.plane_fmt[plane].bytesperline = 0U;
            requested.fmt.pix_mp.plane_fmt[plane].sizeimage = 0U;
        }
    }

    if (xioctl(fd, VIDIOC_S_FMT, &requested) == -1) {
        perror("VIDIOC_S_FMT");
        return -1;
    }

    *actual = requested;

    if (kind == CAPTURE_KIND_SINGLE) {
        *plane_count = 1U;
        printf("Format       : ");
        print_fourcc(requested.fmt.pix.pixelformat);
        printf("\n");
        printf("Resolution   : %ux%u\n",
               requested.fmt.pix.width,
               requested.fmt.pix.height);
        printf("Stride       : %u\n", requested.fmt.pix.bytesperline);
        printf("Size image   : %u\n", requested.fmt.pix.sizeimage);
    } else {
        *plane_count = requested.fmt.pix_mp.num_planes;
        if (*plane_count == 0U || *plane_count > VIDEO_MAX_PLANES) {
            fprintf(stderr, "Invalid multi-planar plane count: %u\n", *plane_count);
            return -1;
        }

        printf("Format       : ");
        print_fourcc(requested.fmt.pix_mp.pixelformat);
        printf("\n");
        printf("Resolution   : %ux%u\n",
               requested.fmt.pix_mp.width,
               requested.fmt.pix_mp.height);
        printf("Planes       : %u\n", *plane_count);
        for (unsigned int index = 0; index < *plane_count; index++) {
            printf("  plane[%u]   : stride=%u, size=%u\n",
                   index,
                   requested.fmt.pix_mp.plane_fmt[index].bytesperline,
                   requested.fmt.pix_mp.plane_fmt[index].sizeimage);
        }
    }

    if (requested.type == V4L2_BUF_TYPE_VIDEO_CAPTURE &&
        (requested.fmt.pix.width != config->width ||
         requested.fmt.pix.height != config->height ||
         requested.fmt.pix.pixelformat != config->pixel_format)) {
        fprintf(stderr,
                "Warning: driver adjusted the requested format to %ux%u fourcc=%08" PRIx32 "\n",
                requested.fmt.pix.width,
                requested.fmt.pix.height,
                requested.fmt.pix.pixelformat);
    } else if (requested.type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE &&
               (requested.fmt.pix_mp.width != config->width ||
                requested.fmt.pix_mp.height != config->height ||
                requested.fmt.pix_mp.pixelformat != config->pixel_format)) {
        fprintf(stderr,
                "Warning: driver adjusted the requested format to %ux%u fourcc=%08" PRIx32 "\n",
                requested.fmt.pix_mp.width,
                requested.fmt.pix_mp.height,
                requested.fmt.pix_mp.pixelformat);
    }

    return 0;
}

static int set_frame_rate(int fd, enum capture_kind kind, unsigned int fps)
{
    struct v4l2_streamparm parameters;

    if (fps == 0U) {
        return 0;
    }

    memset(&parameters, 0, sizeof(parameters));
    parameters.type = capture_buf_type(kind);

    if (xioctl(fd, VIDIOC_G_PARM, &parameters) == -1) {
        perror("VIDIOC_G_PARM");
        return -1;
    }

    if ((parameters.parm.capture.capability & V4L2_CAP_TIMEPERFRAME) == 0U) {
        fprintf(stderr, "Device does not support frame interval control\n");
        return -1;
    }

    parameters.parm.capture.timeperframe.numerator = 1U;
    parameters.parm.capture.timeperframe.denominator = fps;

    if (xioctl(fd, VIDIOC_S_PARM, &parameters) == -1) {
        perror("VIDIOC_S_PARM");
        return -1;
    }

    printf("Requested FPS: %u, actual: %u/%u\n",
           fps,
           parameters.parm.capture.timeperframe.denominator,
           parameters.parm.capture.timeperframe.numerator);

    return 0;
}

static int request_buffers(int fd,
                           enum capture_kind kind,
                           unsigned int requested_count,
                           unsigned int *actual_count)
{
    struct v4l2_requestbuffers request;

    memset(&request, 0, sizeof(request));
    request.count = requested_count;
    request.type = capture_buf_type(kind);
    request.memory = V4L2_MEMORY_MMAP;

    if (xioctl(fd, VIDIOC_REQBUFS, &request) == -1) {
        perror("VIDIOC_REQBUFS");
        return -1;
    }

    if (request.count == 0U) {
        fprintf(stderr, "Driver returned zero mmap buffers\n");
        return -1;
    }

    *actual_count = request.count;
    return 0;
}

static int map_buffers(int fd,
                       enum capture_kind kind,
                       unsigned int plane_count,
                       unsigned int buffer_count,
                       struct buffer_slot *slots)
{
    for (unsigned int index = 0; index < buffer_count; index++) {
        if (kind == CAPTURE_KIND_SINGLE) {
            struct v4l2_buffer buffer;

            memset(&buffer, 0, sizeof(buffer));
            buffer.type = capture_buf_type(kind);
            buffer.memory = V4L2_MEMORY_MMAP;
            buffer.index = index;

            if (xioctl(fd, VIDIOC_QUERYBUF, &buffer) == -1) {
                perror("VIDIOC_QUERYBUF");
                return -1;
            }

            slots[index].plane_count = 1U;
            slots[index].length[0] = buffer.length;
            slots[index].start[0] = mmap(NULL,
                                         buffer.length,
                                         PROT_READ | PROT_WRITE,
                                         MAP_SHARED,
                                         fd,
                                         buffer.m.offset);
            if (slots[index].start[0] == MAP_FAILED) {
                slots[index].start[0] = NULL;
                perror("mmap");
                return -1;
            }
        } else {
            struct v4l2_buffer buffer;
            struct v4l2_plane planes[VIDEO_MAX_PLANES];

            memset(&buffer, 0, sizeof(buffer));
            memset(planes, 0, sizeof(planes));
            buffer.type = capture_buf_type(kind);
            buffer.memory = V4L2_MEMORY_MMAP;
            buffer.index = index;
            buffer.m.planes = planes;
            buffer.length = plane_count;

            if (xioctl(fd, VIDIOC_QUERYBUF, &buffer) == -1) {
                perror("VIDIOC_QUERYBUF");
                return -1;
            }

            slots[index].plane_count = plane_count;
            for (unsigned int plane = 0; plane < plane_count; plane++) {
                slots[index].length[plane] = planes[plane].length;
                slots[index].start[plane] = mmap(NULL,
                                                 planes[plane].length,
                                                 PROT_READ | PROT_WRITE,
                                                 MAP_SHARED,
                                                 fd,
                                                 planes[plane].m.mem_offset);
                if (slots[index].start[plane] == MAP_FAILED) {
                    slots[index].start[plane] = NULL;
                    perror("mmap");
                    return -1;
                }
            }
        }
    }

    return 0;
}

static void unmap_buffers(struct buffer_slot *slots, unsigned int buffer_count)
{
    if (slots == NULL) {
        return;
    }

    for (unsigned int index = 0; index < buffer_count; index++) {
        for (unsigned int plane = 0; plane < slots[index].plane_count; plane++) {
            if (slots[index].start[plane] != NULL) {
                munmap(slots[index].start[plane], slots[index].length[plane]);
                slots[index].start[plane] = NULL;
            }
        }
    }
}

static int queue_buffer(int fd,
                        enum capture_kind kind,
                        struct buffer_slot *slot,
                        unsigned int index)
{
    if (kind == CAPTURE_KIND_SINGLE) {
        struct v4l2_buffer buffer;

        memset(&buffer, 0, sizeof(buffer));
        buffer.type = capture_buf_type(kind);
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = index;

        if (xioctl(fd, VIDIOC_QBUF, &buffer) == -1) {
            perror("VIDIOC_QBUF");
            return -1;
        }
    } else {
        struct v4l2_buffer buffer;
        struct v4l2_plane planes[VIDEO_MAX_PLANES];

        memset(&buffer, 0, sizeof(buffer));
        memset(planes, 0, sizeof(planes));
        buffer.type = capture_buf_type(kind);
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = index;
        buffer.m.planes = planes;
        buffer.length = slot->plane_count;

        for (unsigned int plane = 0; plane < slot->plane_count; plane++) {
            planes[plane].length = slot->length[plane];
        }

        if (xioctl(fd, VIDIOC_QBUF, &buffer) == -1) {
            perror("VIDIOC_QBUF");
            return -1;
        }
    }

    return 0;
}

static int dequeue_buffer(int fd,
                          enum capture_kind kind,
                          unsigned int plane_count,
                          int timeout_ms,
                          struct frame *frame)
{
    struct pollfd poll_fd;

    memset(&poll_fd, 0, sizeof(poll_fd));
    poll_fd.fd = fd;
    poll_fd.events = POLLIN;

    for (;;) {
        int poll_result;

        if (g_stop) {
            return DEQUEUE_STOPPED;
        }

        poll_result = poll(&poll_fd, 1, timeout_ms);
        if (poll_result == -1) {
            if (errno == EINTR) {
                continue;
            }
            perror("poll");
            return DEQUEUE_ERROR;
        }

        if (poll_result == 0) {
            fprintf(stderr, "VIDIOC_DQBUF timed out after %d ms\n", timeout_ms);
            return DEQUEUE_TIMEOUT;
        }

        if ((poll_fd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            fprintf(stderr, "Capture device reported poll error: 0x%x\n", poll_fd.revents);
            return DEQUEUE_ERROR;
        }

        memset(frame, 0, sizeof(*frame));
        frame->buffer.type = capture_buf_type(kind);
        frame->buffer.memory = V4L2_MEMORY_MMAP;

        if (kind == CAPTURE_KIND_MPLANE) {
            frame->buffer.m.planes = frame->planes;
            frame->buffer.length = plane_count;
        }

        if (xioctl(fd, VIDIOC_DQBUF, &frame->buffer) == 0) {
            return DEQUEUE_OK;
        }

        if (errno != EAGAIN) {
            perror("VIDIOC_DQBUF");
            return DEQUEUE_ERROR;
        }
    }
}

static int write_bytes(FILE *output, const void *data, size_t length)
{
    const uint8_t *cursor = data;
    size_t remaining = length;

    while (remaining > 0U) {
        size_t written = fwrite(cursor, 1U, remaining, output);

        if (written == 0U) {
            if (ferror(output)) {
                perror("fwrite");
            }
            return -1;
        }

        cursor += written;
        remaining -= written;
    }

    return 0;
}

static int write_rows(FILE *output,
                      const uint8_t *source,
                      size_t source_length,
                      size_t stride,
                      uint32_t width,
                      uint32_t rows)
{
    size_t required;

    if (stride < width) {
        fprintf(stderr, "Invalid stride %zu for width %u\n", stride, width);
        return -1;
    }

    required = stride * rows;
    if (source_length < required) {
        fprintf(stderr,
                "Buffer is too small: need %zu bytes, got %zu bytes\n",
                required,
                source_length);
        return -1;
    }

    for (uint32_t row = 0; row < rows; row++) {
        if (write_bytes(output, source + ((size_t)row * stride), width) == -1) {
            return -1;
        }
    }

    return 0;
}

static bool is_nv12(uint32_t format)
{
    return format == V4L2_PIX_FMT_NV12 || format == V4L2_PIX_FMT_NV12M;
}

static const uint8_t *plane_data(const struct buffer_slot *slot,
                                 const struct frame *frame,
                                 unsigned int plane)
{
    const uint8_t *data = slot->start[plane];

    return data + frame->planes[plane].data_offset;
}

static int save_raw_frame(enum capture_kind kind,
                          FILE *output,
                          const struct buffer_slot *slot,
                          const struct frame *frame)
{
    if (kind == CAPTURE_KIND_SINGLE) {
        return write_bytes(output, slot->start[0], frame->buffer.bytesused);
    }

    for (unsigned int plane = 0; plane < slot->plane_count; plane++) {
        if (write_bytes(output,
                        plane_data(slot, frame, plane),
                        frame->planes[plane].bytesused) == -1) {
            return -1;
        }
    }

    return 0;
}

static int save_packed_nv12(FILE *output,
                            enum capture_kind kind,
                            uint32_t width,
                            uint32_t height,
                            const struct v4l2_format *format,
                            const struct buffer_slot *slot,
                            const struct frame *frame)
{
    if ((height & 1U) != 0U) {
        fprintf(stderr, "NV12 requires an even height, got %u\n", height);
        return -1;
    }

    if (kind == CAPTURE_KIND_MPLANE && slot->plane_count >= 2U) {
        uint32_t y_stride = format->fmt.pix_mp.plane_fmt[0].bytesperline;
        uint32_t uv_stride = format->fmt.pix_mp.plane_fmt[1].bytesperline;
        size_t uv_rows = height / 2U;

        if (y_stride == 0U) {
            y_stride = width;
        }
        if (uv_stride == 0U) {
            if (uv_rows != 0U &&
                frame->planes[1].bytesused % uv_rows == 0U) {
                size_t inferred_stride = frame->planes[1].bytesused / uv_rows;

                if (inferred_stride >= width && inferred_stride <= UINT32_MAX) {
                    uv_stride = (uint32_t)inferred_stride;
                }
            }
            if (uv_stride == 0U) {
                uv_stride = width;
            }
        }

        if (write_rows(output,
                       plane_data(slot, frame, 0),
                       frame->planes[0].bytesused,
                       y_stride,
                       width,
                       height) == -1) {
            return -1;
        }

        return write_rows(output,
                          plane_data(slot, frame, 1),
                          frame->planes[1].bytesused,
                          uv_stride,
                          width,
                          height / 2U);
    }

    {
        uint32_t y_stride;
        uint32_t uv_stride;
        size_t source_length;
        const uint8_t *source = kind == CAPTURE_KIND_MPLANE
                                    ? plane_data(slot, frame, 0)
                                    : slot->start[0];
        size_t uv_rows = height / 2U;
        size_t y_size;
        size_t uv_size;

        if (kind == CAPTURE_KIND_SINGLE) {
            y_stride = format->fmt.pix.bytesperline;
            source_length = frame->buffer.bytesused;
        } else {
            y_stride = format->fmt.pix_mp.plane_fmt[0].bytesperline;
            source_length = frame->planes[0].bytesused;
        }

        if (y_stride == 0U) {
            y_stride = width;
        }
        if (y_stride < width) {
            fprintf(stderr, "Invalid NV12 Y stride %u for width %u\n", y_stride, width);
            return -1;
        }

        y_size = (size_t)y_stride * height;
        if (source_length < y_size) {
            fprintf(stderr,
                    "NV12 Y plane is too small: need %zu bytes, got %zu bytes\n",
                    y_size,
                    source_length);
            return -1;
        }

        uv_stride = 0U;
        if (uv_rows != 0U) {
            size_t chroma_bytes = source_length - y_size;

            if (chroma_bytes % uv_rows == 0U) {
                size_t inferred_stride = chroma_bytes / uv_rows;

                if (inferred_stride >= width && inferred_stride <= UINT32_MAX) {
                    uv_stride = (uint32_t)inferred_stride;
                }
            }
        }
        if (uv_stride == 0U) {
            uv_stride = width;
        }

        uv_size = (size_t)uv_stride * uv_rows;
        if (source_length < y_size + uv_size) {
            fprintf(stderr,
                    "NV12 buffer is too small: need %zu bytes, got %zu bytes\n",
                    y_size + uv_size,
                    source_length);
            return -1;
        }

        if (write_rows(output, source, y_size, y_stride, width, height) == -1) {
            return -1;
        }

        return write_rows(output,
                          source + y_size,
                          uv_size,
                          uv_stride,
                          width,
                          uv_rows);
    }
}

static int make_output_path(const struct config *config,
                            unsigned long frame_index,
                            char *path,
                            size_t path_size)
{
    const char *marker = strstr(config->output, "%04u");

    if (marker != NULL) {
        int written = snprintf(path,
                               path_size,
                               "%.*s%04lu%s",
                               (int)(marker - config->output),
                               config->output,
                               frame_index,
                               marker + 4);
        if (written < 0 || (size_t)written >= path_size) {
            return -1;
        }
        return 0;
    }

    if (config->frames == 1U) {
        int written = snprintf(path, path_size, "%s", config->output);
        return (written < 0 || (size_t)written >= path_size) ? -1 : 0;
    }

    {
        int written = snprintf(path,
                               path_size,
                               "%s-%04lu.raw",
                               config->output,
                               frame_index);
        return (written < 0 || (size_t)written >= path_size) ? -1 : 0;
    }
}

static ssize_t save_frame(const struct config *config,
                          enum capture_kind kind,
                          uint32_t width,
                          uint32_t height,
                          const struct v4l2_format *format,
                          const struct buffer_slot *slot,
                          const struct frame *frame,
                          unsigned long frame_index)
{
    char path[MAX_PATH_LEN];
    FILE *output;
    int result;
    long file_size;
    uint32_t pixel_format;

    if (make_output_path(config, frame_index, path, sizeof(path)) == -1) {
        fprintf(stderr, "Output path is too long\n");
        return -1;
    }

    output = fopen(path, "wb");
    if (output == NULL) {
        perror(path);
        return -1;
    }

    if (kind == CAPTURE_KIND_SINGLE) {
        pixel_format = format->fmt.pix.pixelformat;
    } else {
        pixel_format = format->fmt.pix_mp.pixelformat;
    }

    if (!config->raw && is_nv12(pixel_format)) {
        result = save_packed_nv12(output,
                                  kind,
                                  width,
                                  height,
                                  format,
                                  slot,
                                  frame);
    } else {
        result = save_raw_frame(kind, output, slot, frame);
    }

    if (result == -1) {
        fclose(output);
        return -1;
    }

    if (fflush(output) == EOF) {
        perror("fflush");
        fclose(output);
        return -1;
    }

    if (fseek(output, 0L, SEEK_END) == 0) {
        file_size = ftell(output);
    } else {
        file_size = -1;
    }

    if (fclose(output) == EOF) {
        perror("fclose");
        return -1;
    }

    printf("Saved        : %s (%ld bytes)\n", path, file_size);
    return file_size;
}

static uint64_t monotonic_ns(void)
{
    struct timespec timestamp;

    if (clock_gettime(CLOCK_MONOTONIC, &timestamp) == -1) {
        return 0U;
    }

    return ((uint64_t)timestamp.tv_sec * 1000000000ULL) +
           (uint64_t)timestamp.tv_nsec;
}

#ifndef V4L2_CAPTURE_NO_MAIN
int main(int argc, char **argv)
{
    struct config config;
    struct v4l2_capability capabilities;
    enum capture_kind selected_kind = CAPTURE_KIND_AUTO;
    struct v4l2_format actual_format;
    unsigned int plane_count = 0U;
    unsigned int buffer_count = 0U;
    struct buffer_slot *buffers = NULL;
    enum v4l2_buf_type buf_type;
    int fd = -1;
    int exit_code = EXIT_FAILURE;
    bool streaming = false;
    unsigned long captured = 0U;
    unsigned long warmup_done = 0U;
    uint64_t saved_bytes = 0U;
    uint64_t first_timestamp = 0U;
    uint64_t last_timestamp = 0U;
    uint32_t actual_width;
    uint32_t actual_height;
    char buffer_count_text[32];
    char fps_text[32];

    memset(&config, 0, sizeof(config));
    config.device = DEFAULT_DEVICE;
    config.output = "frame";
    config.width = DEFAULT_WIDTH;
    config.height = DEFAULT_HEIGHT;
    config.frames = DEFAULT_FRAMES;
    config.buffer_count = DEFAULT_BUFFER_COUNT;
    config.timeout_ms = DEFAULT_TIMEOUT_MS;
    config.requested_kind = CAPTURE_KIND_AUTO;

    if (parse_fourcc(DEFAULT_FORMAT, &config.pixel_format) == -1) {
        fprintf(stderr, "Internal pixel format error\n");
        return EXIT_FAILURE;
    }

    for (;;) {
        int option;
        int option_index = 0;
        enum {
            OPTION_TYPE = 1000,
            OPTION_RAW,
            OPTION_DISCARD,
            OPTION_WARMUP,
        };
        static const struct option long_options[] = {
            {"device", required_argument, NULL, 'd'},
            {"width", required_argument, NULL, 'w'},
            {"height", required_argument, NULL, 'H'},
            {"format", required_argument, NULL, 'f'},
            {"frames", required_argument, NULL, 'n'},
            {"buffers", required_argument, NULL, 'b'},
            {"output", required_argument, NULL, 'o'},
            {"fps", required_argument, NULL, 'r'},
            {"timeout", required_argument, NULL, 't'},
            {"type", required_argument, NULL, OPTION_TYPE},
            {"warmup", required_argument, NULL, OPTION_WARMUP},
            {"raw", no_argument, NULL, OPTION_RAW},
            {"discard", no_argument, NULL, OPTION_DISCARD},
            {"info", no_argument, NULL, 'i'},
            {"list-formats", no_argument, NULL, 'L'},
            {"help", no_argument, NULL, 'h'},
            {NULL, 0, NULL, 0},
        };

        option = getopt_long(argc, argv, "d:w:H:f:n:b:o:r:t:iLh", long_options, &option_index);
        if (option == -1) {
            break;
        }

        switch (option) {
        case 'd':
            config.device = optarg;
            break;
        case 'w': {
            unsigned long value;
            if (parse_unsigned(optarg, &value) == -1 || value == 0U || value > UINT32_MAX) {
                fprintf(stderr, "Invalid width: %s\n", optarg);
                return EXIT_FAILURE;
            }
            config.width = (uint32_t)value;
            break;
        }
        case 'H': {
            unsigned long value;
            if (parse_unsigned(optarg, &value) == -1 || value == 0U || value > UINT32_MAX) {
                fprintf(stderr, "Invalid height: %s\n", optarg);
                return EXIT_FAILURE;
            }
            config.height = (uint32_t)value;
            break;
        }
        case 'f':
            if (parse_fourcc(optarg, &config.pixel_format) == -1) {
                fprintf(stderr, "Pixel format must contain exactly four characters\n");
                return EXIT_FAILURE;
            }
            break;
        case 'n': {
            unsigned long value;
            if (parse_unsigned(optarg, &value) == -1) {
                fprintf(stderr, "Invalid frame count: %s\n", optarg);
                return EXIT_FAILURE;
            }
            config.frames = value;
            break;
        }
        case 'b': {
            unsigned long value;
            if (parse_unsigned(optarg, &value) == -1 || value == 0U || value > UINT32_MAX) {
                fprintf(stderr, "Invalid buffer count: %s\n", optarg);
                return EXIT_FAILURE;
            }
            config.buffer_count = (unsigned int)value;
            break;
        }
        case 'o':
            config.output = optarg;
            break;
        case 'r': {
            unsigned long value;
            if (parse_unsigned(optarg, &value) == -1 || value == 0U || value > UINT32_MAX) {
                fprintf(stderr, "Invalid FPS: %s\n", optarg);
                return EXIT_FAILURE;
            }
            config.fps = (unsigned int)value;
            break;
        }
        case 't': {
            unsigned long value;
            if (parse_unsigned(optarg, &value) == -1 || value == 0U || value > INT32_MAX) {
                fprintf(stderr, "Invalid timeout: %s\n", optarg);
                return EXIT_FAILURE;
            }
            config.timeout_ms = (int)value;
            break;
        }
        case OPTION_TYPE:
            if (parse_capture_kind(optarg, &config.requested_kind) == -1) {
                fprintf(stderr, "Invalid capture type: %s\n", optarg);
                return EXIT_FAILURE;
            }
            break;
        case OPTION_WARMUP: {
            unsigned long value;
            if (parse_unsigned(optarg, &value) == -1) {
                fprintf(stderr, "Invalid warmup frame count: %s\n", optarg);
                return EXIT_FAILURE;
            }
            config.warmup_frames = value;
            break;
        }
        case OPTION_RAW:
            config.raw = true;
            break;
        case OPTION_DISCARD:
            config.discard = true;
            break;
        case 'i':
            config.info = true;
            break;
        case 'L':
            config.list_formats = true;
            break;
        case 'h':
            usage(argv[0]);
            return EXIT_SUCCESS;
        default:
            usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    if (optind != argc) {
        fprintf(stderr, "Unexpected positional argument: %s\n", argv[optind]);
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    fd = open(config.device, O_RDWR | O_NONBLOCK);
    if (fd == -1) {
        perror(config.device);
        return EXIT_FAILURE;
    }

    memset(&capabilities, 0, sizeof(capabilities));
    if (xioctl(fd, VIDIOC_QUERYCAP, &capabilities) == -1) {
        perror("VIDIOC_QUERYCAP");
        goto cleanup_fd;
    }

    if (config.info) {
        print_device_info(&capabilities);
    }

    if (select_capture_kind(&capabilities,
                            config.requested_kind,
                            &selected_kind) == -1) {
        goto cleanup_fd;
    }

    printf("Device       : %s\n", config.device);
    printf("Capture type : %s\n", capture_kind_name(selected_kind));

    if (config.list_formats) {
        if (list_formats(fd, selected_kind) == -1) {
            goto cleanup_fd;
        }
        exit_code = EXIT_SUCCESS;
        goto cleanup_fd;
    }

    if (config.info) {
        exit_code = EXIT_SUCCESS;
        goto cleanup_fd;
    }

    if (set_capture_format(fd,
                           selected_kind,
                           &config,
                           &actual_format,
                           &plane_count) == -1) {
        goto cleanup_fd;
    }

    if (set_frame_rate(fd, selected_kind, config.fps) == -1) {
        goto cleanup_fd;
    }

    if (request_buffers(fd,
                        selected_kind,
                        config.buffer_count,
                        &buffer_count) == -1) {
        goto cleanup_fd;
    }

    if (buffer_count < 2U) {
        fprintf(stderr, "Warning: driver returned only %u buffer\n", buffer_count);
    }

    buffers = calloc(buffer_count, sizeof(*buffers));
    if (buffers == NULL) {
        perror("calloc");
        goto cleanup_buffers;
    }

    if (map_buffers(fd,
                    selected_kind,
                    plane_count,
                    buffer_count,
                    buffers) == -1) {
        goto cleanup_buffers;
    }

    for (unsigned int index = 0; index < buffer_count; index++) {
        if (queue_buffer(fd, selected_kind, &buffers[index], index) == -1) {
            goto cleanup_buffers;
        }
    }

    buf_type = capture_buf_type(selected_kind);
    if (xioctl(fd, VIDIOC_STREAMON, &buf_type) == -1) {
        perror("VIDIOC_STREAMON");
        goto cleanup_buffers;
    }
    streaming = true;

    if (selected_kind == CAPTURE_KIND_SINGLE) {
        actual_width = actual_format.fmt.pix.width;
        actual_height = actual_format.fmt.pix.height;
    } else {
        actual_width = actual_format.fmt.pix_mp.width;
        actual_height = actual_format.fmt.pix_mp.height;
    }

    printf("Streaming... press Ctrl+C to stop\n");

    while (config.frames == 0U || captured < config.frames) {
        struct frame frame_data;
        int result;
        uint64_t timestamp;

        result = dequeue_buffer(fd,
                                selected_kind,
                                plane_count,
                                config.timeout_ms,
                                &frame_data);
        if (result == DEQUEUE_STOPPED) {
            break;
        }
        if (result != DEQUEUE_OK) {
            goto cleanup_buffers;
        }

        if (frame_data.buffer.index >= buffer_count) {
            fprintf(stderr, "Driver returned invalid buffer index %u\n",
                    frame_data.buffer.index);
            goto cleanup_buffers;
        }

        if (warmup_done < config.warmup_frames) {
            if (queue_buffer(fd,
                             selected_kind,
                             &buffers[frame_data.buffer.index],
                             frame_data.buffer.index) == -1) {
                goto cleanup_buffers;
            }
            warmup_done++;
            continue;
        }

        timestamp = monotonic_ns();
        if (captured == 0U) {
            first_timestamp = timestamp;
        }
        last_timestamp = timestamp;

        if (!config.discard) {
            ssize_t bytes = save_frame(&config,
                                       selected_kind,
                                       actual_width,
                                       actual_height,
                                       &actual_format,
                                       &buffers[frame_data.buffer.index],
                                       &frame_data,
                                       captured);
            if (bytes < 0) {
                goto cleanup_buffers;
            }
            saved_bytes += (uint64_t)bytes;
        }

        if (queue_buffer(fd,
                         selected_kind,
                         &buffers[frame_data.buffer.index],
                         frame_data.buffer.index) == -1) {
            goto cleanup_buffers;
        }

        captured++;
    }

    if (captured > 0U) {
        uint64_t elapsed = last_timestamp - first_timestamp;
        double fps = elapsed == 0U
                         ? 0.0
                         : (double)(captured - 1U) * 1000000000.0 / (double)elapsed;

        snprintf(buffer_count_text, sizeof(buffer_count_text), "%u", buffer_count);
        snprintf(fps_text, sizeof(fps_text), "%.3f", fps);

        printf("Captured     : %lu frames\n", captured);
        printf("Buffers      : %s\n", buffer_count_text);
        printf("Average FPS  : %s\n", fps_text);
        printf("Saved bytes  : %" PRIu64 "\n", saved_bytes);
    }

    exit_code = EXIT_SUCCESS;

cleanup_buffers:
    if (streaming) {
        buf_type = capture_buf_type(selected_kind);
        if (xioctl(fd, VIDIOC_STREAMOFF, &buf_type) == -1) {
            perror("VIDIOC_STREAMOFF");
            exit_code = EXIT_FAILURE;
        }
    }

    unmap_buffers(buffers, buffer_count);
    free(buffers);

cleanup_fd:
    if (fd != -1) {
        close(fd);
    }

    return exit_code;
}
#endif
