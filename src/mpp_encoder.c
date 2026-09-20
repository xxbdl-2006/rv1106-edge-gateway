#include "mpp_encoder.h"

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <rk_mpi_mb.h>
#include <rk_mpi_sys.h>
#include <rk_mpi_venc.h>

#define ENCODER_CHANNEL 0
#define STREAM_PACK_COUNT 1U

struct mpp_encoder {
    VENC_CHN channel;
    MB_POOL pool;
    MB_BLK frame_block;
    VENC_STREAM_S stream;
    VIDEO_FRAME_INFO_S frame;
    uint32_t width;
    uint32_t height;
    uint32_t hor_stride;
    uint32_t ver_stride;
    size_t frame_size;
    uint32_t time_ref;
    bool sys_initialized;
    bool pool_created;
    bool channel_created;
};

static uint32_t align_up(uint32_t value, uint32_t alignment)
{
    return (value + alignment - 1U) & ~(alignment - 1U);
}

static int check_rk_result(const char *operation, RK_S32 result)
{
    if (result == RK_SUCCESS) {
        return 0;
    }

    fprintf(stderr, "%s failed: 0x%08x\n", operation, result);
    return -1;
}

int mpp_encoder_open(struct mpp_encoder **encoder_ptr,
                     const struct mpp_encoder_config *config)
{
    struct mpp_encoder *encoder;
    MB_POOL_CONFIG_S pool_config;
    VENC_CHN_ATTR_S channel_attr;
    VENC_H264_VUI_S vui;
    VENC_RECV_PIC_PARAM_S receive;
    uint32_t bitrate_kbps;

    if (encoder_ptr == NULL || config == NULL ||
        config->width == 0U || config->height == 0U ||
        config->fps_num == 0U || config->fps_den == 0U) {
        fprintf(stderr, "Invalid encoder configuration\n");
        return -1;
    }

    encoder = calloc(1U, sizeof(*encoder));
    if (encoder == NULL) {
        perror("calloc");
        return -1;
    }

    encoder->channel = -1;
    encoder->width = config->width;
    encoder->height = config->height;
    encoder->hor_stride = align_up(config->width, 16U);
    encoder->ver_stride = align_up(config->height, 16U);
    encoder->frame_size = (size_t)encoder->hor_stride *
                          encoder->ver_stride * 3U / 2U;

    if (check_rk_result("RK_MPI_SYS_Init", RK_MPI_SYS_Init()) == -1) {
        goto fail;
    }
    encoder->sys_initialized = true;

    memset(&pool_config, 0, sizeof(pool_config));
    pool_config.u64MBSize = encoder->frame_size;
    pool_config.u32MBCnt = 2U;
    pool_config.enAllocType = MB_ALLOC_TYPE_DMA;
    pool_config.enDmaType = MB_DMA_TYPE_NONE;
    pool_config.enRemapMode = MB_REMAP_MODE_NONE;

    encoder->pool = RK_MPI_MB_CreatePool(&pool_config);
    if (encoder->pool == MB_INVALID_POOLID) {
        fprintf(stderr, "RK_MPI_MB_CreatePool failed\n");
        goto fail;
    }
    encoder->pool_created = true;

    encoder->frame_block = RK_MPI_MB_GetMB(encoder->pool,
                                           encoder->frame_size,
                                           RK_TRUE);
    if (encoder->frame_block == NULL) {
        fprintf(stderr, "RK_MPI_MB_GetMB failed\n");
        goto fail;
    }

    encoder->stream.pstPack = calloc(STREAM_PACK_COUNT,
                                     sizeof(*encoder->stream.pstPack));
    if (encoder->stream.pstPack == NULL) {
        perror("calloc");
        goto fail;
    }

    memset(&channel_attr, 0, sizeof(channel_attr));
    channel_attr.stVencAttr.enType = RK_VIDEO_ID_AVC;
    channel_attr.stVencAttr.enPixelFormat = RK_FMT_YUV420SP;
    channel_attr.stVencAttr.enMirror = MIRROR_NONE;
    channel_attr.stVencAttr.u32MaxPicWidth = encoder->width;
    channel_attr.stVencAttr.u32MaxPicHeight = encoder->height;
    channel_attr.stVencAttr.u32PicWidth = encoder->width;
    channel_attr.stVencAttr.u32PicHeight = encoder->height;
    channel_attr.stVencAttr.u32VirWidth = encoder->hor_stride;
    channel_attr.stVencAttr.u32VirHeight = encoder->ver_stride;
    channel_attr.stVencAttr.u32Profile = H264E_PROFILE_MAIN;
    channel_attr.stVencAttr.u32StreamBufCnt = 4U;
    channel_attr.stVencAttr.u32BufSize = (RK_U32)encoder->frame_size;
    channel_attr.stVencAttr.stAttrH264e.u32Level = 40U;

    bitrate_kbps = (uint32_t)((config->bitrate + 999) / 1000);
    if (bitrate_kbps < 2U) {
        bitrate_kbps = 2U;
    }

    channel_attr.stRcAttr.enRcMode = VENC_RC_MODE_H264CBR;
    channel_attr.stRcAttr.stH264Cbr.u32Gop = config->gop;
    channel_attr.stRcAttr.stH264Cbr.u32SrcFrameRateNum = config->fps_num;
    channel_attr.stRcAttr.stH264Cbr.u32SrcFrameRateDen = config->fps_den;
    channel_attr.stRcAttr.stH264Cbr.fr32DstFrameRateNum = config->fps_num;
    channel_attr.stRcAttr.stH264Cbr.fr32DstFrameRateDen = config->fps_den;
    channel_attr.stRcAttr.stH264Cbr.u32BitRate = bitrate_kbps;
    channel_attr.stRcAttr.stH264Cbr.u32StatTime = 1U;
    channel_attr.stGopAttr.enGopMode = VENC_GOPMODE_NORMALP;

    if (check_rk_result("RK_MPI_VENC_CreateChn",
                        RK_MPI_VENC_CreateChn(ENCODER_CHANNEL,
                                              &channel_attr)) == -1) {
        goto fail;
    }
    encoder->channel = ENCODER_CHANNEL;
    encoder->channel_created = true;

    memset(&vui, 0, sizeof(vui));
    vui.stVuiVideoSignal.video_signal_type_present_flag = 1U;
    vui.stVuiVideoSignal.video_format = 5U;
    vui.stVuiVideoSignal.video_full_range_flag = 1U;
    vui.stVuiVideoSignal.colour_description_present_flag = 1U;
    vui.stVuiVideoSignal.colour_primaries = 6U;
    vui.stVuiVideoSignal.transfer_characteristics = 6U;
    vui.stVuiVideoSignal.matrix_coefficients = 6U;
    if (RK_MPI_VENC_SetH264Vui(ENCODER_CHANNEL, &vui) != RK_SUCCESS) {
        memset(&vui, 0, sizeof(vui));
        vui.stVuiVideoSignal.video_signal_type_present_flag = 1U;
        vui.stVuiVideoSignal.video_format = 5U;
        vui.stVuiVideoSignal.video_full_range_flag = 1U;
        if (check_rk_result("RK_MPI_VENC_SetH264Vui",
                            RK_MPI_VENC_SetH264Vui(ENCODER_CHANNEL,
                                                   &vui)) == -1) {
            fprintf(stderr,
                    "Warning: unable to override H.264 color metadata\n");
        }
    }

    memset(&receive, 0, sizeof(receive));
    receive.s32RecvPicNum = -1;
    if (check_rk_result("RK_MPI_VENC_StartRecvFrame",
                        RK_MPI_VENC_StartRecvFrame(ENCODER_CHANNEL,
                                                   &receive)) == -1) {
        goto fail;
    }

    memset(&encoder->frame, 0, sizeof(encoder->frame));
    encoder->frame.stVFrame.pMbBlk = encoder->frame_block;
    encoder->frame.stVFrame.u32Width = encoder->width;
    encoder->frame.stVFrame.u32Height = encoder->height;
    encoder->frame.stVFrame.u32VirWidth = encoder->hor_stride;
    encoder->frame.stVFrame.u32VirHeight = encoder->ver_stride;
    encoder->frame.stVFrame.enPixelFormat = RK_FMT_YUV420SP;
    encoder->frame.stVFrame.enCompressMode = COMPRESS_MODE_NONE;
    encoder->frame.stVFrame.enDynamicRange = DYNAMIC_RANGE_SDR8;
    encoder->frame.stVFrame.enColorGamut = COLOR_GAMUT_BT601;

    *encoder_ptr = encoder;
    return 0;

fail:
    mpp_encoder_close(encoder);
    return -1;
}

int mpp_encoder_encode_nv12(struct mpp_encoder *encoder,
                            const void *nv12,
                            size_t size,
                            uint64_t pts,
                            FILE *output)
{
    void *destination;
    RK_S32 result;

    if (encoder == NULL || nv12 == NULL || output == NULL ||
        !encoder->channel_created || encoder->frame_block == NULL) {
        return -1;
    }

    if (size < encoder->frame_size) {
        fprintf(stderr,
                "NV12 frame is too small: got %zu bytes, need %zu bytes\n",
                size,
                encoder->frame_size);
        return -1;
    }

    destination = RK_MPI_MB_Handle2VirAddr(encoder->frame_block);
    if (destination == NULL) {
        fprintf(stderr, "RK_MPI_MB_Handle2VirAddr failed\n");
        return -1;
    }

    memcpy(destination, nv12, encoder->frame_size);
    (void)RK_MPI_SYS_MmzFlushCache(encoder->frame_block, RK_FALSE);

    encoder->frame.stVFrame.u32TimeRef = encoder->time_ref++;
    encoder->frame.stVFrame.u64PTS = pts / 1000U;

    result = RK_MPI_VENC_SendFrame(ENCODER_CHANNEL,
                                   &encoder->frame,
                                   -1);
    if (check_rk_result("RK_MPI_VENC_SendFrame", result) == -1) {
        return -1;
    }

    result = RK_MPI_VENC_GetStream(ENCODER_CHANNEL,
                                   &encoder->stream,
                                   -1);
    if (check_rk_result("RK_MPI_VENC_GetStream", result) == -1) {
        return -1;
    }

    if (encoder->stream.u32PackCount == 0U) {
        fprintf(stderr, "VENC returned no packet for frame %u\n",
                encoder->frame.stVFrame.u32TimeRef);
        (void)RK_MPI_VENC_ReleaseStream(ENCODER_CHANNEL,
                                        &encoder->stream);
        return -1;
    }

    fprintf(stderr,
            "VENC frame %u: packs=%u first_len=%u segments=%u\n",
            encoder->frame.stVFrame.u32TimeRef,
            encoder->stream.u32PackCount,
            encoder->stream.pstPack[0].u32Len,
            encoder->stream.pstPack[0].u32DataNum);

    for (uint32_t index = 0; index < encoder->stream.u32PackCount; index++) {
        VENC_PACK_S *pack = &encoder->stream.pstPack[index];
        uint8_t *base;

        base = RK_MPI_MB_Handle2VirAddr(pack->pMbBlk);
        if (base == NULL) {
            fprintf(stderr, "Encoded packet has no CPU mapping\n");
            (void)RK_MPI_VENC_ReleaseStream(ENCODER_CHANNEL,
                                            &encoder->stream);
            return -1;
        }

        if (pack->u32DataNum > 0U) {
            for (uint32_t part = 0U;
                 part < pack->u32DataNum &&
                 part < sizeof(pack->stPackInfo) /
                        sizeof(pack->stPackInfo[0]);
                 part++) {
                VENC_PACK_INFO_S *info = &pack->stPackInfo[part];

                if (info->u32PackLength > 0U &&
                    fwrite(base + info->u32PackOffset,
                           1U,
                           info->u32PackLength,
                           output) != info->u32PackLength) {
                    fprintf(stderr, "Failed to write H.264 packet: %s\n",
                            strerror(errno));
                    (void)RK_MPI_VENC_ReleaseStream(ENCODER_CHANNEL,
                                                    &encoder->stream);
                    return -1;
                }
            }
        } else if (pack->u32Len > 0U &&
                   fwrite(base + pack->u32Offset,
                          1U,
                          pack->u32Len,
                          output) != pack->u32Len) {
            fprintf(stderr, "Failed to write H.264 packet: %s\n",
                    strerror(errno));
            (void)RK_MPI_VENC_ReleaseStream(ENCODER_CHANNEL,
                                            &encoder->stream);
            return -1;
        }
    }

    result = RK_MPI_VENC_ReleaseStream(ENCODER_CHANNEL, &encoder->stream);
    if (check_rk_result("RK_MPI_VENC_ReleaseStream", result) == -1) {
        return -1;
    }

    if (fflush(output) == EOF) {
        perror("fflush");
        return -1;
    }

    return 0;
}

int mpp_encoder_flush(struct mpp_encoder *encoder, FILE *output)
{
    (void)encoder;

    if (output == NULL || fflush(output) == EOF) {
        return -1;
    }

    return 0;
}

void mpp_encoder_close(struct mpp_encoder *encoder)
{
    if (encoder == NULL) {
        return;
    }

    if (encoder->channel_created) {
        (void)RK_MPI_VENC_StopRecvFrame(encoder->channel);
        (void)RK_MPI_VENC_DestroyChn(encoder->channel);
        encoder->channel_created = false;
    }

    free(encoder->stream.pstPack);
    encoder->stream.pstPack = NULL;

    if (encoder->frame_block != NULL) {
        (void)RK_MPI_MB_ReleaseMB(encoder->frame_block);
        encoder->frame_block = NULL;
    }

    if (encoder->pool_created) {
        (void)RK_MPI_MB_DestroyPool(encoder->pool);
        encoder->pool_created = false;
    }

    if (encoder->sys_initialized) {
        (void)RK_MPI_SYS_Exit();
        encoder->sys_initialized = false;
    }

    free(encoder);
}
