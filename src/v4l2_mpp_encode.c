#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "capture_thread.h"
#include "frame_ring.h"
#include "mpp_encoder.h"
#include "packet_queue.h"
#include "rtsp_server.h"
#include "sink_file.h"
#include "sink_queue.h"

/*
 * Reuse the already verified V4L2 capture implementation. The standalone
 * program keeps its main(), while this program provides its own main loop.
 */
#define V4L2_CAPTURE_NO_MAIN
#include "v4l2_capture.c"
#undef V4L2_CAPTURE_NO_MAIN

#define ENC_DEFAULT_DEVICE "/dev/video11"
#define ENC_DEFAULT_WIDTH 1280U
#define ENC_DEFAULT_HEIGHT 720U
#define ENC_DEFAULT_FPS 30U
#define ENC_DEFAULT_BITRATE 2000000
#define ENC_DEFAULT_GOP 30U
#define ENC_DEFAULT_WARMUP 30U
#define ENC_DEFAULT_RTSP_PORT 8554U
#define ENC_DEFAULT_RTSP_PATH "/live/0"
#define ENC_DEFAULT_RING_SLOTS 4U
#define ENC_BUILD_TAG "rockit-venc-v23"

struct encode_options {
    const char *device;
    const char *output;
    const char *sink_type;
    uint16_t rtsp_port;
    int quiet;
    int threads;
    uint32_t width;
    uint32_t height;
    uint32_t fps;
    uint32_t gop;
    uint32_t warmup;
    uint32_t ring_slots;
    int32_t bitrate;
    unsigned long max_frames;
};

static void print_usage(const char *program)
{
    fprintf(stderr,
            "Usage: %s [options]\n"
            "\n"
            "Options:\n"
            "  -d, --device DEV      V4L2 device (default: %s)\n"
            "  -w, --width N         width (default: %u)\n"
            "  -H, --height N        height (default: %u)\n"
            "  -n, --frames N        frames to encode, 0 means until SIGINT\n"
            "  -o, --output FILE     H.264 output (default: live.h264)\n"
            "      --sink TYPE       file, queue or rtsp (default: %s)\n"
            "      --rtsp-port N     RTSP TCP port when --sink rtsp (default: %u)\n"
            "  -r, --fps N           frame rate (default: %u)\n"
            "  -b, --bitrate N       CBR bitrate (default: %d)\n"
            "  -g, --gop N           GOP length (default: %u)\n"
            "      --warmup N        discard initial frames (default: %u)\n"
            "      --threads         capture on its own thread via a frame ring\n"
            "      --ring-slots N    frame ring depth when --threads (default: %u)\n"
            "  -q, --quiet           suppress the per frame encoder trace\n"
            "  -h, --help            show this help\n",
            program,
            ENC_DEFAULT_DEVICE,
            ENC_DEFAULT_WIDTH,
            ENC_DEFAULT_HEIGHT,
            "file",
            (unsigned)ENC_DEFAULT_RTSP_PORT,
            ENC_DEFAULT_FPS,
            ENC_DEFAULT_BITRATE,
            ENC_DEFAULT_GOP,
            ENC_DEFAULT_WARMUP,
            (unsigned)ENC_DEFAULT_RING_SLOTS);
}

static int parse_u32(const char *text, uint32_t *value)
{
    char *end = NULL;
    unsigned long result;

    errno = 0;
    result = strtoul(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0' || result > UINT32_MAX) {
        return -1;
    }

    *value = (uint32_t)result;
    return 0;
}

static int parse_u64(const char *text, unsigned long *value)
{
    char *end = NULL;
    unsigned long result;

    errno = 0;
    result = strtoul(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0') {
        return -1;
    }

    *value = result;
    return 0;
}

/*
 * Tightly repack one V4L2 NV12 frame, removing the row stride padding.
 *
 * Split into two layers because two callers need different halves:
 *
 *   nv12_source_*      pulls the plane pointers and geometry out of a dequeued
 *                      V4L2 buffer
 *   pack_nv12_rows     copies Y then UV, honouring the source stride
 *
 * The synchronous main loop calls both back to back; the capture thread calls
 * the source half and then hands the result to pack_nv12_rows on the consumer
 * side, which is what keeps the V4L2 details out of capture_thread.c.
 */
struct nv12_source {
    const uint8_t *data;
    size_t length;
    uint32_t y_stride;
    uint32_t uv_stride;
    uint32_t width;
    uint32_t height;
};

static int nv12_source_from_buffer(struct nv12_source *source,
                                   const struct buffer_slot *slot,
                                   const struct frame *frame,
                                   enum capture_kind kind,
                                   const struct v4l2_format *format,
                                   uint32_t width,
                                   uint32_t height)
{
    size_t uv_rows = height / 2U;
    size_t y_size;
    size_t chroma_bytes;

    if (width == 0U || height == 0U || (height & 1U) != 0U) {
        return -1;
    }

    memset(source, 0, sizeof(*source));

    if (kind == CAPTURE_KIND_SINGLE) {
        source->data = slot->start[0];
        source->length = frame->buffer.bytesused;
        source->y_stride = format->fmt.pix.bytesperline;
    } else {
        source->data = plane_data(slot, frame, 0);
        source->length = frame->planes[0].bytesused;
        source->y_stride = format->fmt.pix_mp.plane_fmt[0].bytesperline;
    }

    if (source->data == NULL || source->y_stride < width) {
        return -1;
    }

    y_size = (size_t)source->y_stride * height;
    if (source->length < y_size) {
        return -1;
    }

    /*
     * The chroma stride is not always reported, so infer it from what is left
     * in the buffer. The handoff notes a stride of 2304 leaking in from a stale
     * G_FMT, which is exactly the case this guards against.
     */
    chroma_bytes = source->length - y_size;
    source->uv_stride = width;
    if (uv_rows != 0U && chroma_bytes % uv_rows == 0U) {
        size_t inferred = chroma_bytes / uv_rows;

        if (inferred >= width && inferred <= UINT32_MAX) {
            source->uv_stride = (uint32_t)inferred;
        }
    }

    if (source->length <
        y_size + (size_t)source->uv_stride * uv_rows) {
        return -1;
    }

    source->width = width;
    source->height = height;

    return 0;
}

static int pack_nv12_rows(uint8_t *destination,
                          size_t destination_size,
                          const struct nv12_source *source)
{
    size_t uv_rows = source->height / 2U;
    size_t y_size = (size_t)source->y_stride * source->height;

    if (destination_size <
        (size_t)source->width * source->height +
            (size_t)source->width * uv_rows) {
        return -1;
    }

    for (uint32_t row = 0; row < source->height; row++) {
        memcpy(destination + (size_t)row * source->width,
               source->data + (size_t)row * source->y_stride,
               source->width);
    }

    for (uint32_t row = 0; row < uv_rows; row++) {
        memcpy(destination + (size_t)source->width * source->height +
                   (size_t)row * source->width,
               source->data + y_size + (size_t)row * source->uv_stride,
               source->width);
    }

    return 0;
}

static int copy_nv12_to_tight(uint8_t *destination,
                              size_t destination_size,
                              const struct buffer_slot *slot,
                              const struct frame *frame,
                              enum capture_kind kind,
                              const struct v4l2_format *format,
                              uint32_t width,
                              uint32_t height)
{
    struct nv12_source source;

    if (nv12_source_from_buffer(&source, slot, frame, kind, format,
                                width, height) == -1) {
        return -1;
    }

    return pack_nv12_rows(destination, destination_size, &source);
}

static uint64_t encode_monotonic_ns(void)
{
    struct timespec timestamp;

    if (clock_gettime(CLOCK_MONOTONIC, &timestamp) == -1) {
        return 0U;
    }

    return (uint64_t)timestamp.tv_sec * 1000000000ULL +
           (uint64_t)timestamp.tv_nsec;
}

/* ------------------------------------------------ threaded capture glue */

/*
 * Everything the capture thread's callbacks need to reach the V4L2 device.
 * The synchronous loop below keeps using its own locals; this struct exists
 * only for the --threads path.
 */
struct threaded_capture {
    int fd;
    enum capture_kind kind;
    unsigned int plane_count;
    int timeout_ms;
    uint32_t width;
    uint32_t height;
    const struct v4l2_format *format;
    struct buffer_slot *buffers;
    unsigned int buffer_count;
    uint8_t *tight_nv12;
    size_t tight_size;
};

/*
 * dequeue_buffer() reports a timeout separately from an error, but the capture
 * thread only distinguishes "got one" from "did not". A timeout is normal while
 * the sensor settles, so it must not be counted as a camera failure; genuine
 * errors must be, or the thread would pointlessly retry a dead device.
 *
 * Rather than return false for both, a hard error latches this flag and the
 * callback keeps reporting empty until the thread's retry budget runs out.
 * That way the caller gets one clear failure instead of a silent busy loop.
 */
static bool g_capture_hard_error;

static bool threaded_capture_source(void *context,
                                    struct capture_frame_view *view)
{
    struct threaded_capture *capture = context;
    struct frame frame_data;
    int result;

    if (g_capture_hard_error) {
        return false;
    }

    result = dequeue_buffer(capture->fd,
                            capture->kind,
                            capture->plane_count,
                            capture->timeout_ms,
                            &frame_data);
    if (result == DEQUEUE_STOPPED) {
        return false;
    }
    if (result != DEQUEUE_OK) {
        if (result != DEQUEUE_TIMEOUT) {
            g_capture_hard_error = true;
        }
        return false;
    }

    if (frame_data.buffer.index >= capture->buffer_count) {
        fprintf(stderr, "Invalid V4L2 buffer index: %u\n",
                frame_data.buffer.index);
        g_capture_hard_error = true;
        return false;
    }

    if (copy_nv12_to_tight(capture->tight_nv12,
                           capture->tight_size,
                           &capture->buffers[frame_data.buffer.index],
                           &frame_data,
                           capture->kind,
                           capture->format,
                           capture->width,
                           capture->height) == -1) {
        fprintf(stderr, "Failed to normalize V4L2 NV12 frame\n");
        g_capture_hard_error = true;
        return false;
    }

    view->data = capture->tight_nv12;
    view->length = capture->tight_size;
    view->index = frame_data.buffer.index;
    view->pts_us = encode_monotonic_ns() / 1000ULL;

    return true;
}

static int threaded_capture_release(void *context, unsigned int buffer_index)
{
    struct threaded_capture *capture = context;

    if (buffer_index >= capture->buffer_count) {
        return -1;
    }

    return queue_buffer(capture->fd,
                        capture->kind,
                        &capture->buffers[buffer_index],
                        buffer_index);
}

int main(int argc, char **argv)
{
    struct encode_options options;
    struct config capture_config;
    struct v4l2_capability capabilities;
    struct v4l2_format actual_format;
    enum capture_kind selected_kind = CAPTURE_KIND_AUTO;
    struct buffer_slot *buffers = NULL;
    struct mpp_encoder *encoder = NULL;
    struct mpp_encoder_config encoder_config;
    struct packet_sink *sink = NULL;
    struct packet_queue *queue = NULL;
    struct queue_file_drain *drain = NULL;
    struct rtsp_server *rtsp = NULL;
    uint8_t *tight_nv12 = NULL;
    size_t tight_size;
    unsigned int plane_count = 0U;
    unsigned int buffer_count = 0U;
    enum v4l2_buf_type buf_type;
    int fd = -1;
    int exit_code = EXIT_FAILURE;
    bool streaming = false;
    unsigned long encoded = 0U;
    unsigned long warmup_done = 0U;
    uint64_t first_timestamp = 0U;
    uint64_t last_timestamp = 0U;
    uint32_t actual_width;
    uint32_t actual_height;

    memset(&options, 0, sizeof(options));
    options.device = ENC_DEFAULT_DEVICE;
    options.output = "live.h264";
    options.sink_type = "file";
    options.quiet = 0;
    options.rtsp_port = (uint16_t)ENC_DEFAULT_RTSP_PORT;
    options.width = ENC_DEFAULT_WIDTH;
    options.height = ENC_DEFAULT_HEIGHT;
    options.fps = ENC_DEFAULT_FPS;
    options.gop = ENC_DEFAULT_GOP;
    options.warmup = ENC_DEFAULT_WARMUP;
    options.ring_slots = ENC_DEFAULT_RING_SLOTS;
    options.bitrate = ENC_DEFAULT_BITRATE;
    options.max_frames = 0U;

    for (;;) {
        int option;
        int option_index = 0;
        static const struct option long_options[] = {
            {"device", required_argument, NULL, 'd'},
            {"width", required_argument, NULL, 'w'},
            {"height", required_argument, NULL, 'H'},
            {"frames", required_argument, NULL, 'n'},
            {"output", required_argument, NULL, 'o'},
            {"fps", required_argument, NULL, 'r'},
            {"bitrate", required_argument, NULL, 'b'},
            {"gop", required_argument, NULL, 'g'},
            {"warmup", required_argument, NULL, 1000},
            {"sink", required_argument, NULL, 1001},
            {"rtsp-port", required_argument, NULL, 1002},
            {"ring-slots", required_argument, NULL, 1003},
            {"threads", no_argument, NULL, 1004},
            {"quiet", no_argument, NULL, 'q'},
            {"help", no_argument, NULL, 'h'},
            {NULL, 0, NULL, 0},
        };

        option = getopt_long(argc,
                             argv,
                             "d:w:H:n:o:r:b:g:qh",
                             long_options,
                             &option_index);
        if (option == -1) {
            break;
        }

        switch (option) {
        case 'q':
            options.quiet = 1;
            break;
        case 'd':
            options.device = optarg;
            break;
        case 'w':
            if (parse_u32(optarg, &options.width) == -1 || options.width == 0U) {
                fprintf(stderr, "Invalid width: %s\n", optarg);
                return EXIT_FAILURE;
            }
            break;
        case 'H':
            if (parse_u32(optarg, &options.height) == -1 || options.height == 0U) {
                fprintf(stderr, "Invalid height: %s\n", optarg);
                return EXIT_FAILURE;
            }
            break;
        case 'n':
            if (parse_u64(optarg, &options.max_frames) == -1) {
                fprintf(stderr, "Invalid frame count: %s\n", optarg);
                return EXIT_FAILURE;
            }
            break;
        case 'o':
            options.output = optarg;
            break;
        case 'r':
            if (parse_u32(optarg, &options.fps) == -1 || options.fps == 0U) {
                fprintf(stderr, "Invalid FPS: %s\n", optarg);
                return EXIT_FAILURE;
            }
            break;
        case 'b': {
            uint32_t value;
            if (parse_u32(optarg, &value) == -1 || value == 0U) {
                fprintf(stderr, "Invalid bitrate: %s\n", optarg);
                return EXIT_FAILURE;
            }
            options.bitrate = (int32_t)value;
            break;
        }
        case 'g':
            if (parse_u32(optarg, &options.gop) == -1 || options.gop == 0U) {
                fprintf(stderr, "Invalid GOP: %s\n", optarg);
                return EXIT_FAILURE;
            }
            break;
        case 1001:
            if (strcmp(optarg, "file") != 0 && strcmp(optarg, "queue") != 0 &&
                strcmp(optarg, "rtsp") != 0) {
                fprintf(stderr, "Invalid sink: %s (use file, queue or rtsp)\n",
                        optarg);
                return EXIT_FAILURE;
            }
            options.sink_type = optarg;
            break;
        case 1002: {
            uint32_t value;

            if (parse_u32(optarg, &value) == -1 || value == 0U ||
                value > 65535U) {
                fprintf(stderr, "Invalid RTSP port: %s\n", optarg);
                return EXIT_FAILURE;
            }
            options.rtsp_port = (uint16_t)value;
            break;
        }
        case 1000:
            if (parse_u32(optarg, &options.warmup) == -1) {
                fprintf(stderr, "Invalid warmup count: %s\n", optarg);
                return EXIT_FAILURE;
            }
            break;
        case 1003:
            if (parse_u32(optarg, &options.ring_slots) == -1 ||
                options.ring_slots < 2U || options.ring_slots > 64U) {
                fprintf(stderr,
                        "Invalid ring slot count: %s (2 to 64)\n", optarg);
                return EXIT_FAILURE;
            }
            break;
        case 1004:
            options.threads = 1;
            break;
        case 'h':
            print_usage(argv[0]);
            return EXIT_SUCCESS;
        default:
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    if (optind != argc) {
        fprintf(stderr, "Unexpected argument: %s\n", argv[optind]);
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    memset(&capture_config, 0, sizeof(capture_config));
    capture_config.device = options.device;
    capture_config.width = options.width;
    capture_config.height = options.height;
    capture_config.buffer_count = 4U;
    capture_config.timeout_ms = 2000;
    capture_config.requested_kind = CAPTURE_KIND_AUTO;
    if (parse_fourcc("NV12", &capture_config.pixel_format) == -1) {
        fprintf(stderr, "Internal NV12 format error\n");
        return EXIT_FAILURE;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    fd = open(options.device, O_RDWR | O_NONBLOCK);
    if (fd == -1) {
        perror(options.device);
        return EXIT_FAILURE;
    }

    memset(&capabilities, 0, sizeof(capabilities));
    if (xioctl(fd, VIDIOC_QUERYCAP, &capabilities) == -1) {
        perror("VIDIOC_QUERYCAP");
        goto cleanup;
    }

    if (select_capture_kind(&capabilities,
                            capture_config.requested_kind,
                            &selected_kind) == -1) {
        goto cleanup;
    }

    if (set_capture_format(fd,
                           selected_kind,
                           &capture_config,
                           &actual_format,
                           &plane_count) == -1) {
        goto cleanup;
    }

    if (plane_count != 1U) {
        fprintf(stderr,
                "This first integration supports one NV12 memory plane, got %u\n",
                plane_count);
        goto cleanup;
    }

    {
        uint32_t pixel_format = selected_kind == CAPTURE_KIND_SINGLE
                                    ? actual_format.fmt.pix.pixelformat
                                    : actual_format.fmt.pix_mp.pixelformat;

        if (!is_nv12(pixel_format)) {
            fprintf(stderr, "Driver selected an unsupported pixel format\n");
            goto cleanup;
        }
    }

    if (selected_kind == CAPTURE_KIND_SINGLE) {
        fprintf(stderr,
                "V4L2 color: space=%u xfer=%u ycbcr=%u quant=%u\n",
                actual_format.fmt.pix.colorspace,
                actual_format.fmt.pix.xfer_func,
                actual_format.fmt.pix.ycbcr_enc,
                actual_format.fmt.pix.quantization);
    } else {
        fprintf(stderr,
                "V4L2 color: space=%u xfer=%u ycbcr=%u quant=%u\n",
                actual_format.fmt.pix_mp.colorspace,
                actual_format.fmt.pix_mp.xfer_func,
                actual_format.fmt.pix_mp.ycbcr_enc,
                actual_format.fmt.pix_mp.quantization);
    }

    if (request_buffers(fd,
                        selected_kind,
                        capture_config.buffer_count,
                        &buffer_count) == -1) {
        goto cleanup;
    }

    buffers = calloc(buffer_count, sizeof(*buffers));
    if (buffers == NULL) {
        perror("calloc");
        goto cleanup;
    }

    if (map_buffers(fd,
                    selected_kind,
                    plane_count,
                    buffer_count,
                    buffers) == -1) {
        goto cleanup;
    }

    for (unsigned int index = 0; index < buffer_count; index++) {
        if (queue_buffer(fd, selected_kind, &buffers[index], index) == -1) {
            goto cleanup;
        }
    }

    buf_type = capture_buf_type(selected_kind);
    if (xioctl(fd, VIDIOC_STREAMON, &buf_type) == -1) {
        perror("VIDIOC_STREAMON");
        goto cleanup;
    }
    streaming = true;

    if (selected_kind == CAPTURE_KIND_SINGLE) {
        actual_width = actual_format.fmt.pix.width;
        actual_height = actual_format.fmt.pix.height;
    } else {
        actual_width = actual_format.fmt.pix_mp.width;
        actual_height = actual_format.fmt.pix_mp.height;
    }

    if (actual_width != options.width || actual_height != options.height) {
        fprintf(stderr,
                "Driver adjusted resolution to %ux%u\n",
                actual_width,
                actual_height);
        goto cleanup;
    }

    tight_size = (size_t)actual_width * actual_height * 3U / 2U;
    tight_nv12 = malloc(tight_size);
    if (tight_nv12 == NULL) {
        perror("malloc");
        goto cleanup;
    }

    memset(&encoder_config, 0, sizeof(encoder_config));
    encoder_config.width = actual_width;
    encoder_config.height = actual_height;
    encoder_config.hor_stride = actual_width;
    encoder_config.ver_stride = actual_height;
    encoder_config.fps_num = options.fps;
    encoder_config.fps_den = 1U;
    encoder_config.gop = options.gop;
    encoder_config.bitrate = options.bitrate;
    encoder_config.quiet = options.quiet;

    if (mpp_encoder_open(&encoder, &encoder_config) == -1) {
        goto cleanup;
    }

    /*
     * file  : encoder -> file, byte for byte the validated baseline.
     * queue : encoder -> bounded packet queue -> drain thread -> file.
     *         Same bytes out, but every frame crosses the queue, so the queue
     *         can be exercised at real frame rate. Mostly kept for regression.
     * rtsp  : encoder -> bounded packet queue -> RTSP over TCP. The RTSP reader
     *         thread is the queue consumer and fans the packetized frame out to
     *         every viewer; clients only handle their own request/response.
     *         With nobody watching the reader still drains the queue, so the
     *         encoder never blocks and a late viewer starts on live frames.
     */
    if (strcmp(options.sink_type, "file") != 0) {
        struct packet_queue_config queue_config;

        memset(&queue_config, 0, sizeof(queue_config));
        queue_config.slot_count = PACKET_QUEUE_DEFAULT_SLOTS;
        queue_config.slot_bytes = PACKET_QUEUE_DEFAULT_SLOT_BYTES;

        if (packet_queue_open(&queue_config, &queue) == -1) {
            goto cleanup;
        }
        if (sink_queue_open(queue, &sink) == -1) {
            goto cleanup;
        }

        if (strcmp(options.sink_type, "rtsp") == 0) {
            struct rtsp_server_config rtsp_config;

            memset(&rtsp_config, 0, sizeof(rtsp_config));
            rtsp_config.port = options.rtsp_port;
            rtsp_config.path = ENC_DEFAULT_RTSP_PATH;
            rtsp_config.fps = options.fps;
            rtsp_config.width = actual_width;
            rtsp_config.height = actual_height;

            if (rtsp_server_start(queue, &rtsp_config, &rtsp) == -1) {
                goto cleanup;
            }
        } else {
            if (queue_file_drain_start(queue, options.output, &drain) == -1) {
                goto cleanup;
            }
        }
    } else {
        if (sink_file_open(options.output, &sink) == -1) {
            goto cleanup;
        }
    }

    printf("Device       : %s\n", options.device);
    printf("Build        : %s\n", ENC_BUILD_TAG);
    printf("Capture type : %s\n", capture_kind_name(selected_kind));
    printf("Resolution   : %ux%u\n", actual_width, actual_height);
    printf("Encoder      : H.264 CBR %d bps\n", options.bitrate);
    printf("Sink         : %s\n", options.sink_type);
    if (strcmp(options.sink_type, "rtsp") == 0) {
        printf("Stream       : rtsp://<board-ip>:%u%s\n",
               (unsigned)options.rtsp_port, ENC_DEFAULT_RTSP_PATH);
    } else {
        printf("Output       : %s\n", options.output);
    }
    printf("Capture mode : %s\n",
           options.threads ? "thread + frame ring" : "synchronous");
    printf("Streaming... press Ctrl+C to stop\n");

    /*
     * Threaded pipeline: V4L2 runs on the capture thread and drops finished
     * NV12 frames into a bounded ring; this thread only does encode plus sink.
     *
     * The point is decoupling, not throughput. Before this, a slow encoder
     * stalled VIDIOC_DQBUF directly, so the sensor queue filled up behind us
     * and the latency grew with it. Now the only thing that can delay a
     * requeue is a memcpy into the ring, and when the encoder falls behind the
     * ring overwrites its oldest frame instead of pushing the delay upstream.
     */
    if (options.threads) {
        struct threaded_capture capture_context;
        struct capture_thread_config capture_config_threaded;
        struct capture_thread *capture = NULL;

        memset(&capture_context, 0, sizeof(capture_context));
        capture_context.fd = fd;
        capture_context.kind = selected_kind;
        capture_context.plane_count = plane_count;
        capture_context.timeout_ms = capture_config.timeout_ms;
        capture_context.width = actual_width;
        capture_context.height = actual_height;
        capture_context.format = &actual_format;
        capture_context.buffers = buffers;
        capture_context.buffer_count = buffer_count;
        capture_context.tight_nv12 = tight_nv12;
        capture_context.tight_size = tight_size;

        memset(&capture_config_threaded, 0, sizeof(capture_config_threaded));
        capture_config_threaded.ring_slots = options.ring_slots;
        capture_config_threaded.slot_bytes = tight_size;
        capture_config_threaded.warmup_frames = options.warmup;

        g_capture_hard_error = false;

        if (capture_thread_start(&capture_config_threaded,
                                 threaded_capture_source,
                                 threaded_capture_release,
                                 &capture_context,
                                 &capture) == -1) {
            goto cleanup;
        }

        if (capture_thread_wait_ready(capture, 10000) == -1) {
            fprintf(stderr, "Capture thread produced no frame\n");
            capture_thread_stop(capture);
            capture_thread_destroy(capture);
            goto cleanup;
        }

        {
            struct frame_ring *ring = capture_thread_ring(capture);
            const struct frame_ring_slot *slot = NULL;

            while (!g_stop &&
                   (options.max_frames == 0U || encoded < options.max_frames)) {
                int result = frame_ring_acquire(ring, &slot, 200);

                if (result == FRAME_RING_STOPPED) {
                    break;
                }
                if (result != FRAME_RING_OK) {
                    if (capture_thread_failed(capture)) {
                        fprintf(stderr, "Capture thread stopped on error\n");
                        break;
                    }
                    continue;
                }

                if (encoded == 0U) {
                    first_timestamp = slot->pts_us * 1000ULL;
                }
                last_timestamp = slot->pts_us * 1000ULL;

                if (mpp_encoder_encode_nv12(encoder,
                                            slot->data,
                                            slot->length,
                                            last_timestamp,
                                            sink) == -1) {
                    frame_ring_release(ring);
                    capture_thread_stop(capture);
                    capture_thread_destroy(capture);
                    goto cleanup;
                }

                encoded++;
                frame_ring_release(ring);
            }

            /*
             * capture_thread_stop() also raises g_stop, which is what breaks
             * the capture thread out of a blocking DQBUF poll. That is safe
             * here only because it is the last thing this branch does; the
             * statistics below are read before destroy() frees the ring.
             */
            capture_thread_stop(capture);

            /*
             * Report the ring statistics before the thread is torn down: a
             * non zero dropped_oldest is not an error, but it does tell us the
             * encoder could not keep up and by how much.
             */
            {
                struct frame_ring_stats ring_stats;
                struct capture_thread_stats capture_stats;

                frame_ring_stats(ring, &ring_stats);
                capture_thread_stats(capture, &capture_stats);

                fprintf(stderr,
                        "Ring pushed=%llu popped=%llu dropped_oldest=%llu "
                        "dropped_busy=%llu peak_depth=%zu\n",
                        (unsigned long long)ring_stats.pushed,
                        (unsigned long long)ring_stats.popped,
                        (unsigned long long)ring_stats.dropped_oldest,
                        (unsigned long long)ring_stats.dropped_busy,
                        ring_stats.peak_depth);
                fprintf(stderr,
                        "Capture captured=%lu skipped=%lu errors=%lu\n",
                        capture_stats.captured,
                        capture_stats.skipped,
                        capture_stats.capture_errors);
            }

            capture_thread_destroy(capture);
        }
    } else {
        while (!g_stop &&
               (options.max_frames == 0U || encoded < options.max_frames)) {
            struct frame frame_data;
            int result;
            uint64_t timestamp;

            result = dequeue_buffer(fd,
                                    selected_kind,
                                    plane_count,
                                    capture_config.timeout_ms,
                                    &frame_data);
            if (result == DEQUEUE_STOPPED) {
                break;
            }
            if (result != DEQUEUE_OK) {
                goto cleanup;
            }

            if (frame_data.buffer.index >= buffer_count) {
                fprintf(stderr, "Invalid V4L2 buffer index: %u\n",
                        frame_data.buffer.index);
                goto cleanup;
            }

            if (warmup_done < options.warmup) {
                if (queue_buffer(fd,
                                 selected_kind,
                                 &buffers[frame_data.buffer.index],
                                 frame_data.buffer.index) == -1) {
                    goto cleanup;
                }
                warmup_done++;
                continue;
            }

            if (copy_nv12_to_tight(tight_nv12,
                                   tight_size,
                                   &buffers[frame_data.buffer.index],
                                   &frame_data,
                                   selected_kind,
                                   &actual_format,
                                   actual_width,
                                   actual_height) == -1) {
                fprintf(stderr, "Failed to normalize V4L2 NV12 frame\n");
                goto cleanup;
            }

            if (queue_buffer(fd,
                             selected_kind,
                             &buffers[frame_data.buffer.index],
                             frame_data.buffer.index) == -1) {
                goto cleanup;
            }

            timestamp = encode_monotonic_ns();
            if (encoded == 0U) {
                first_timestamp = timestamp;
            }
            last_timestamp = timestamp;

            if (mpp_encoder_encode_nv12(encoder,
                                        tight_nv12,
                                        tight_size,
                                        timestamp,
                                        sink) == -1) {
                goto cleanup;
            }

            encoded++;
        }
    }

    if (mpp_encoder_flush(encoder, sink) == -1) {
        goto cleanup;
    }

    if (encoded > 0U) {
        uint64_t elapsed = last_timestamp - first_timestamp;
        double fps = elapsed == 0U
                         ? 0.0
                         : (double)(encoded - 1U) * 1000000000.0 /
                               (double)elapsed;

        printf("Captured     : %lu frames\n", encoded);
        printf("Average FPS  : %.3f\n", fps);
    }

    exit_code = EXIT_SUCCESS;

cleanup:
    if (streaming) {
        buf_type = capture_buf_type(selected_kind);
        xioctl(fd, VIDIOC_STREAMOFF, &buf_type);
    }

    if (rtsp != NULL) {
        rtsp_server_stop(rtsp);
        rtsp = NULL;
    }

    if (drain != NULL) {
        queue_file_drain_stop(drain);
        drain = NULL;
    }

    packet_sink_close(sink);
    sink = NULL;

    if (queue != NULL) {
        struct packet_queue_stats stats;

        packet_queue_stats(queue, &stats);
        fprintf(stderr,
                "Queue pushed=%llu popped=%llu dropped_stale=%llu "
                "dropped_full=%llu dropped_oversize=%llu peak_depth=%zu\n",
                (unsigned long long)stats.pushed,
                (unsigned long long)stats.popped,
                (unsigned long long)stats.dropped_stale,
                (unsigned long long)stats.dropped_full,
                (unsigned long long)stats.dropped_oversize,
                stats.peak_depth);
        packet_queue_destroy(queue);
        queue = NULL;
    }

    mpp_encoder_close(encoder);
    free(tight_nv12);
    unmap_buffers(buffers, buffer_count);
    free(buffers);

    if (fd != -1) {
        close(fd);
    }

    return exit_code;
}
