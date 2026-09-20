#ifndef MPP_ENCODER_H
#define MPP_ENCODER_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

struct mpp_encoder;

struct mpp_encoder_config {
    uint32_t width;
    uint32_t height;
    uint32_t hor_stride;
    uint32_t ver_stride;
    uint32_t fps_num;
    uint32_t fps_den;
    uint32_t gop;
    int32_t bitrate;
};

int mpp_encoder_open(struct mpp_encoder **encoder,
                     const struct mpp_encoder_config *config);

int mpp_encoder_encode_nv12(struct mpp_encoder *encoder,
                            const void *nv12,
                            size_t size,
                            uint64_t pts,
                            FILE *output);

int mpp_encoder_flush(struct mpp_encoder *encoder, FILE *output);

void mpp_encoder_close(struct mpp_encoder *encoder);

#endif
