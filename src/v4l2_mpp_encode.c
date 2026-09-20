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

#include "mpp_encoder.h"

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
#define ENC_BUILD_TAG "rockit-venc-v21"

struct encode_options {
    const char *device;
    const char *output;
    uint32_t width;
    uint32_t height;
    uint32_t fps;
    uint32_t gop;
    uint32_t warmup;
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
            "  -r, --fps N           frame rate (default: %u)\n"
            "  -b, --bitrate N       CBR bitrate (default: %d)\n"
            "  -g, --gop N           GOP length (default: %u)\n"
            "      --warmup N        discard initial frames (default: %u)\n"
            "  -h, --help            show this help\n",
            program,
            ENC_DEFAULT_DEVICE,
            ENC_DEFAULT_WIDTH,
            ENC_DEFAULT_HEIGHT,
            ENC_DEFAULT_FPS,
            ENC_DEFAULT_BITRATE,
            ENC_DEFAULT_GOP,
            ENC_DEFAULT_WARMUP);
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

static int copy_nv12_to_tight(uint8_t *destination,
                              size_t destination_size,
                              const struct buffer_slot *slot,
                              const struct frame *frame,
                              enum capture_kind kind,
                              const struct v4l2_format *format,
                              uint32_t width,
                              uint32_t height)
{
    const uint8_t *source;
    size_t source_length;
    uint32_t y_stride;
    uint32_t uv_stride;
    size_t uv_rows = height / 2U;
    size_t y_size;
    size_t uv_size;
    size_t chroma_bytes;

    if (destination == NULL || width == 0U || height == 0U ||
        (height & 1U) != 0U) {
        return -1;
    }

    if (kind == CAPTURE_KIND_SINGLE) {
        source = slot->start[0];
        source_length = frame->buffer.bytesused;
        y_stride = format->fmt.pix.bytesperline;
    } else {
        source = plane_data(slot, frame, 0);
        source_length = frame->planes[0].bytesused;
        y_stride = format->fmt.pix_mp.plane_fmt[0].bytesperline;
    }

    if (source == NULL || y_stride < width) {
        return -1;
    }

    y_size = (size_t)y_stride * height;
    if (source_length < y_size) {
        return -1;
    }

    chroma_bytes = source_length - y_size;
    uv_stride = width;
    if (uv_rows != 0U && chroma_bytes % uv_rows == 0U) {
        size_t inferred = chroma_bytes / uv_rows;

        if (inferred >= width && inferred <= UINT32_MAX) {
            uv_stride = (uint32_t)inferred;
        }
    }

    uv_size = (size_t)uv_stride * uv_rows;
    if (source_length < y_size + uv_size) {
        return -1;
    }

    if (destination_size < (size_t)width * height +
                           (size_t)width * uv_rows) {
        return -1;
    }

    for (uint32_t row = 0; row < height; row++) {
        memcpy(destination + (size_t)row * width,
               source + (size_t)row * y_stride,
               width);
    }

    for (uint32_t row = 0; row < uv_rows; row++) {
        memcpy(destination + (size_t)width * height + (size_t)row * width,
               source + y_size + (size_t)row * uv_stride,
               width);
    }

    return 0;
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
    FILE *output = NULL;
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
    options.width = ENC_DEFAULT_WIDTH;
    options.height = ENC_DEFAULT_HEIGHT;
    options.fps = ENC_DEFAULT_FPS;
    options.gop = ENC_DEFAULT_GOP;
    options.warmup = ENC_DEFAULT_WARMUP;
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
            {"help", no_argument, NULL, 'h'},
            {NULL, 0, NULL, 0},
        };

        option = getopt_long(argc,
                             argv,
                             "d:w:H:n:o:r:b:g:h",
                             long_options,
                             &option_index);
        if (option == -1) {
            break;
        }

        switch (option) {
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
        case 1000:
            if (parse_u32(optarg, &options.warmup) == -1) {
                fprintf(stderr, "Invalid warmup count: %s\n", optarg);
                return EXIT_FAILURE;
            }
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

    if (mpp_encoder_open(&encoder, &encoder_config) == -1) {
        goto cleanup;
    }

    output = fopen(options.output, "wb");
    if (output == NULL) {
        perror(options.output);
        goto cleanup;
    }

    printf("Device       : %s\n", options.device);
    printf("Build        : %s\n", ENC_BUILD_TAG);
    printf("Capture type : %s\n", capture_kind_name(selected_kind));
    printf("Resolution   : %ux%u\n", actual_width, actual_height);
    printf("Encoder      : H.264 CBR %d bps\n", options.bitrate);
    printf("Output       : %s\n", options.output);
    printf("Streaming... press Ctrl+C to stop\n");

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
                                    output) == -1) {
            goto cleanup;
        }

        encoded++;
    }

    if (mpp_encoder_flush(encoder, output) == -1) {
        goto cleanup;
    }

    if (fflush(output) == EOF) {
        perror("fflush");
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

    if (output != NULL) {
        fclose(output);
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
