/*
* Copyright (c) 2018 Xilinx Inc
*
* This file is part of FFmpeg.
*
* FFmpeg is free software; you can redistribute it and/or
* modify it under the terms of the GNU Lesser General Public
* License as published by the Free Software Foundation; either
* version 2.1 of the License, or (at your option) any later version.
*
* FFmpeg is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
* Lesser General Public License for more details.
*
* You should have received a copy of the GNU Lesser General Public
* License along with FFmpeg; if not, write to the Free Software
* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
*/

#include "libavutil/internal.h"
#include "libavutil/common.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "libavutil/imgutils.h"
#include "libavutil/timestamp.h"
#include "libavutil/macros.h"
#include "libavutil/fifo.h"
#include "avcodec.h"
#include "internal.h"
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <memory.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <xma.h>
#include <xrm.h>
#include <pthread.h>
#include "xlnx_lookahead.h"
#include <xvbm.h>
#include <dlfcn.h>
#include "../xmaPropsTOjson.h"

#define SCLEVEL1 2

#define MAX_ENC_PARAMS      (6)
/* MAX_EXTRADATA_SIZE should be consistent with AL_ENC_MAX_CONFIG_HEADER_SIZE on device */
#define MAX_EXTRADATA_SIZE   (2 * 1024)
#define MAX_ENC_WIDTH        3840
#define MAX_ENC_HEIGHT       2160
#define MAX_ENC_PIXELS       (MAX_ENC_WIDTH * MAX_ENC_HEIGHT)
#define MAX_LA_INPUT_WIDTH   1920
#define MAX_LA_INPUT_HEIGHT  1080
#define MAX_LA_INPUT_PIXELS  (MAX_LA_INPUT_WIDTH * MAX_LA_INPUT_HEIGHT)

#define OFFSET(x) offsetof(mpsoc_vcu_enc_ctx, x)
#define VE AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_ENCODING_PARAM

#define VCU_STRIDE_ALIGN    256
#define VCU_HEIGHT_ALIGN    64

#define XRM_PRECISION_1000000_BIT_MASK(load) ((load << 8))


typedef struct {
    AVFrame *pic;
    XmaFrame *xframe;
} mpsoc_enc_req;

typedef struct mpsoc_vcu_enc_ctx {
    const AVClass     *class;
    XmaEncoderSession *enc_session;
    XmaParameter       enc_params[MAX_ENC_PARAMS];
    xrmContext        *xrm_ctx;
    xrmCuListResource  encode_cu_list_res;
    xrmCuResource      encode_cu_hw_res;
    xrmCuResource      encode_cu_sw_res;
    int                encode_res_inuse;
    int                ideal_latency;
    XmaFrame           frame;
    XmaDataBuffer      xma_buffer;
    bool               sent_flush;
    int                control_rate;
    int64_t            max_bitrate;
    int                slice_qp;
    int                min_qp;
    int                max_qp;
    double             cpb_size;
    double             initial_delay;
    int                gop_mode;
    int                gdr_mode;
    int                b_frames;
    int                periodicity_idr;
    int                profile;
    int                level;
    int                tier;
    int                num_slices;
    int                qp_mode;
    int                filler_data;
    int                aspect_ratio;
    int                dependent_slice;
    int                slice_size;
    int                scaling_list;
    int                entropy_mode;
    int                loop_filter;
    int                constrained_intra_pred;
    int                prefetch_buffer;
    int                cores;
    int                latency_logging;
    char               enc_options[2048];
    AVFifoBuffer      *pts_queue;
    int64_t            pts_0;
    int64_t            pts_1;
    int                is_first_outframe;
    const char        *lambda_file;
    //LA
    xlnx_lookahead_t la;
    int32_t lookahead_depth;
    int32_t spatial_aq;
    int32_t temporal_aq;
    int32_t rate_control_mode;
    int32_t spatial_aq_gain;
    XmaFrame* la_in_frame;
	char *expert_options;
	int32_t tune_metrics;
	int32_t lookahead_rc_off;
} mpsoc_vcu_enc_ctx;

int vcu_alloc_ff_packet(mpsoc_vcu_enc_ctx *ctx, AVPacket *pkt);

static const AVOption h264Options[] = {
    { "control-rate", "Rate Control Mode", OFFSET(control_rate), AV_OPT_TYPE_INT, { .i64 = 1}, 0,  3, VE, "control-rate"},
    { "max-bitrate", "Maximum Bit Rate", OFFSET(max_bitrate), AV_OPT_TYPE_INT64, { .i64 = 5000000}, 0,  35000000000, VE, "max-bitrate"},
    { "slice-qp", "Slice QP", OFFSET(slice_qp), AV_OPT_TYPE_INT, { .i64 = -1}, -1,  51, VE, "slice-qp"},
    { "min-qp", "Minimum QP value allowed for the rate control", OFFSET(min_qp), AV_OPT_TYPE_INT, { .i64 = 0}, 0, 51, VE, "min-qp"},
    { "max-qp", "Maximum QP value allowed for the rate control", OFFSET(max_qp), AV_OPT_TYPE_INT, { .i64 = 51}, 0, 51, VE, "max-qp"},
    { "bf", "Number of B-frames", OFFSET(b_frames), AV_OPT_TYPE_INT, { .i64 = 2}, 0, 4294967295, VE, "b-frames"},
    { "periodicity-idr", "IDR Picture Frequency", OFFSET(periodicity_idr), AV_OPT_TYPE_INT, { .i64 = -1}, -1, 4294967295, VE, "periodicity-idr"},
    { "profile", "Set the encoding profile", OFFSET(profile), AV_OPT_TYPE_INT, { .i64 = FF_PROFILE_H264_HIGH }, FF_PROFILE_H264_BASELINE, FF_PROFILE_H264_HIGH, VE, "profile" },
    { "level", "Set the encoding level restriction", OFFSET(level), AV_OPT_TYPE_INT, { .i64 = 10 }, 10, 52, VE, "level" },
    { "slices", "Number of Slices", OFFSET(num_slices), AV_OPT_TYPE_INT, { .i64 = 1}, 1, 68, VE, "slices"},
    { "qp-mode", "QP Control Mode", OFFSET(qp_mode), AV_OPT_TYPE_INT, { .i64 = 1}, 0, 2, VE, "qp-mode"},
    { "aspect-ratio", "Aspect-Ratio", OFFSET(aspect_ratio), AV_OPT_TYPE_INT, { .i64 = 0}, 0, 3, VE, "aspect-ratio"},
    { "scaling-list", "Scaling List Mode", OFFSET(scaling_list), AV_OPT_TYPE_INT, { .i64 = 1}, 0, 1, VE, "scaling-list"},
    { "cores", "Number of cores to use", OFFSET(cores), AV_OPT_TYPE_INT, { .i64 = 0}, 0, 4, VE, "cores"},
    { "lookahead_depth", "Number of frames to lookahead for qp maps generation or custom rate control. Up to 20", OFFSET(lookahead_depth), AV_OPT_TYPE_INT, {.i64 = 0}, 0, 20, VE, "lookahead_depth"},
    { "temporal-aq", "Enable Temporal AQ.", OFFSET(temporal_aq), AV_OPT_TYPE_INT, {.i64 = 1}, 0, 1, VE, "temporal-aq-mode"},
    { "spatial-aq", "Enable Spatial AQ.", OFFSET(spatial_aq), AV_OPT_TYPE_INT, {.i64 = 1}, 0, 1, VE, "spatial-aq-mode"},
    { "spatial-aq-gain", "Percentage of spatial AQ gain", OFFSET(spatial_aq_gain), AV_OPT_TYPE_INT, {.i64 = 50}, 0, 100, VE, "spatial-aq-gain"},
	{ "latency_logging", "Log latency information to syslog", OFFSET(latency_logging), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 1, VE, "latency_logging" },
	{ "expert-options", "Expert options for MPSoC H.264 Encoder", OFFSET(expert_options), AV_OPT_TYPE_STRING, {.str = NULL}, 0, 1024, VE, "expert_options"},
	{ "tune-metrics", "Tunes MPSoC H.264 Encoder's video quality for objective metrics", OFFSET(tune_metrics), AV_OPT_TYPE_INT, {.i64 = 0}, 0, 1, VE, "tune-metrics"},

    { "const-qp", "Constant QP", 0, AV_OPT_TYPE_CONST, { .i64 = 0}, 0, 0, VE, "control-rate"},
    { "cbr", "Constant Bitrate", 0, AV_OPT_TYPE_CONST, { .i64 = 1}, 0, 0, VE, "control-rate"},
    { "vbr", "Variable Bitrate", 0, AV_OPT_TYPE_CONST, { .i64 = 2}, 0, 0, VE, "control-rate"},
    { "low-latency", "Low Latency", 0, AV_OPT_TYPE_CONST, { .i64 = 3}, 0, 0, VE, "control-rate"},
    { "auto", "Auto", 0, AV_OPT_TYPE_CONST, { .i64 = -1}, 0, 0, VE, "slice-qp"},
    { "baseline", "Baseline profile", 0, AV_OPT_TYPE_CONST, { .i64 = FF_PROFILE_H264_BASELINE}, 0, 0, VE, "profile"},
    { "main", "Main profile", 0, AV_OPT_TYPE_CONST, { .i64 = FF_PROFILE_H264_MAIN}, 0, 0, VE, "profile"},
    { "high", "High profile", 0, AV_OPT_TYPE_CONST, { .i64 = FF_PROFILE_H264_HIGH}, 0, 0, VE, "profile"},
    { "1", "1 level", 0, AV_OPT_TYPE_CONST, { .i64 = 10}, 0, 0, VE, "level"},
    { "1.1", "1.1 level", 0, AV_OPT_TYPE_CONST, { .i64 = 11}, 0, 0, VE, "level"},
    { "1.2", "1.2 level", 0, AV_OPT_TYPE_CONST, { .i64 = 12}, 0, 0, VE, "level"},
    { "1.3", "1.3 level", 0, AV_OPT_TYPE_CONST, { .i64 = 13}, 0, 0, VE, "level"},
    { "2", "2 level", 0, AV_OPT_TYPE_CONST, { .i64 = 20}, 0, 0, VE, "level"},
    { "2.1", "2.1 level", 0, AV_OPT_TYPE_CONST, { .i64 = 21}, 0, 0, VE, "level"},
    { "2.2", "2.2 level", 0, AV_OPT_TYPE_CONST, { .i64 = 22}, 0, 0, VE, "level"},
    { "3", "3 level", 0, AV_OPT_TYPE_CONST, { .i64 = 30}, 0, 0, VE, "level"},
    { "3.1", "3.1 level", 0, AV_OPT_TYPE_CONST, { .i64 = 31}, 0, 0, VE, "level"},
    { "3.2", "3.2 level", 0, AV_OPT_TYPE_CONST, { .i64 = 32}, 0, 0, VE, "level"},
    { "4", "4 level", 0, AV_OPT_TYPE_CONST, { .i64 = 40}, 0, 0, VE, "level"},
    { "4.1", "4.1 level", 0, AV_OPT_TYPE_CONST, { .i64 = 41}, 0, 0, VE, "level"},
    { "4.2", "4.2 level", 0, AV_OPT_TYPE_CONST, { .i64 = 42}, 0, 0, VE, "level"},
    { "5", "5 level", 0, AV_OPT_TYPE_CONST, { .i64 = 50}, 0, 0, VE, "level"},
    { "5.1", "5.1 level", 0, AV_OPT_TYPE_CONST, { .i64 = 51}, 0, 0, VE, "level"},
    { "5.2", "5.2 level", 0, AV_OPT_TYPE_CONST, { .i64 = 52}, 0, 0, VE, "level"},
    { "uniform", "Use the same QP for all coding units of the frame", 0, AV_OPT_TYPE_CONST, { .i64 = 0}, 0, 0, VE, "qp-mode"},
    { "auto", "Let the VCU encoder change the QP for each coding unit according to its content", 0, AV_OPT_TYPE_CONST, { .i64 = 1}, 0, 0, VE, "qp-mode"},
    { "relative-load", "Use the information gathered in the lookahead to calculate the best QP", 0, AV_OPT_TYPE_CONST, { .i64 = 2}, 0, 0, VE, "qp-mode"},
    { "auto", "4:3 for SD video, 16:9 for HD video, unspecified for unknown format", 0, AV_OPT_TYPE_CONST, { .i64 = 0}, 0, 0, VE, "aspect-ratio"},
    { "4:3", "4:3 aspect ratio", 0, AV_OPT_TYPE_CONST, { .i64 = 1}, 0, 0, VE, "aspect-ratio"},
    { "16:9", "16:9 aspect ratio", 0, AV_OPT_TYPE_CONST, { .i64 = 2}, 0, 0, VE, "aspect-ratio"},
    { "none", "Aspect ratio information is not present in the stream", 0, AV_OPT_TYPE_CONST, { .i64 = 3}, 0, 0, VE, "aspect-ratio"},
    { "flat", "Flat scaling list mode", 0, AV_OPT_TYPE_CONST, { .i64 = 0}, 0, 0, VE, "scaling-list"},
    { "default", "Default scaling list mode", 0, AV_OPT_TYPE_CONST, { .i64 = 1}, 0, 0, VE, "scaling-list"},
    { "auto", "Automatic", 0, AV_OPT_TYPE_CONST, { .i64 = 0}, 0, 0, VE, "cores"},
    { "disable", "Disable Temporal AQ", 0, AV_OPT_TYPE_CONST, { .i64 = 0}, 0, 0, VE, "temporal-aq-mode"},
    { "enable", "Enable Temporal AQ", 0, AV_OPT_TYPE_CONST, { .i64 = 1}, 0, 0, VE, "temporal-aq-mode"},
    { "disable", "Disable Spatial AQ", 0, AV_OPT_TYPE_CONST, { .i64 = 0}, 0, 0, VE, "spatial-aq-mode"},
    { "enable", "Enable Spatial AQ", 0, AV_OPT_TYPE_CONST, { .i64 = 1}, 0, 0, VE, "spatial-aq-mode"},
	{ "disable", "Disable tune metrics", 0, AV_OPT_TYPE_CONST, { .i64 = 0}, 0, 0, VE, "tune-metrics"},
    { "enable", "Enable tune metrics", 0, AV_OPT_TYPE_CONST, { .i64 = 1}, 0, 0, VE, "tune-metrics"},
    {NULL},
};

static const AVOption hevcOptions[] = {
    { "control-rate", "Rate Control Mode", OFFSET(control_rate), AV_OPT_TYPE_INT, { .i64 = 1}, 0,  3, VE, "control-rate"},
    { "max-bitrate", "Maximum Bit Rate", OFFSET(max_bitrate), AV_OPT_TYPE_INT64, { .i64 = 5000000}, 0,  35000000000, VE, "max-bitrate"},
    { "slice-qp", "Slice QP", OFFSET(slice_qp), AV_OPT_TYPE_INT, { .i64 = -1}, -1,  51, VE, "slice-qp"},
    { "min-qp", "Minimum QP value allowed for the rate control", OFFSET(min_qp), AV_OPT_TYPE_INT, { .i64 = 0}, 0, 51, VE, "min-qp"},
    { "max-qp", "Maximum QP value allowed for the rate control", OFFSET(max_qp), AV_OPT_TYPE_INT, { .i64 = 51}, 0, 51, VE, "max-qp"},
    { "bf", "Number of B-frames", OFFSET(b_frames), AV_OPT_TYPE_INT, { .i64 = 2}, 0, 4294967295, VE, "b-frames"},
    { "periodicity-idr", "IDR Picture Frequency", OFFSET(periodicity_idr), AV_OPT_TYPE_INT, { .i64 = -1}, -1, 4294967295, VE, "periodicity-idr"},
    { "profile", "Set the encoding profile", OFFSET(profile), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 1, VE, "profile" },
    { "level", "Set the encoding level restriction", OFFSET(level), AV_OPT_TYPE_INT, { .i64 = 10 }, 10, 51, VE, "level" },
    { "tier", "Set the encoding tier", OFFSET(tier), AV_OPT_TYPE_INT, { .i64 = 0}, 0, 1, VE, "tier" },
    { "slices", "Number of Slices", OFFSET(num_slices), AV_OPT_TYPE_INT, { .i64 = 1}, 1, 68, VE, "slices"},
    { "qp-mode", "QP Control Mode", OFFSET(qp_mode), AV_OPT_TYPE_INT, { .i64 = 1}, 0, 2, VE, "qp-mode"},
    { "aspect-ratio", "Aspect-Ratio", OFFSET(aspect_ratio), AV_OPT_TYPE_INT, { .i64 = 0}, 0, 3, VE, "aspect-ratio"},
    { "scaling-list", "Scaling List Mode", OFFSET(scaling_list), AV_OPT_TYPE_INT, { .i64 = 1}, 0, 1, VE, "scaling-list"},
    { "cores", "Number of cores to use", OFFSET(cores), AV_OPT_TYPE_INT, { .i64 = 0}, 0, 4, VE, "cores"},
    { "lookahead_depth", "Number of frames to lookahead for qp maps generation or custom rate control. Up to 20", OFFSET(lookahead_depth), AV_OPT_TYPE_INT, {.i64 = 0}, 0, 20, VE, "lookahead_depth"},
    { "temporal-aq", "Enable Temporal AQ.", OFFSET(temporal_aq), AV_OPT_TYPE_INT, {.i64 = 1}, 0, 1, VE, "temporal-aq-mode"},
    { "spatial-aq", "Enable Spatial AQ.", OFFSET(spatial_aq), AV_OPT_TYPE_INT, {.i64 = 1}, 0, 1, VE, "spatial-aq-mode"},
    { "spatial-aq-gain", "Percentage of spatial AQ gain", OFFSET(spatial_aq_gain), AV_OPT_TYPE_INT, {.i64 = 50}, 0, 100, VE, "spatial-aq-gain"},
	{ "latency_logging", "Log latency information to syslog", OFFSET(latency_logging), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 1, VE, "latency_logging" },
	{ "expert-options", "Expert options for MPSoC HEVC Encoder", OFFSET(expert_options), AV_OPT_TYPE_STRING, {.str = NULL}, 0, 1024, VE, "expert_options"},
	{ "tune-metrics", "Tunes MPSoC HEVC Encoder's video quality for objective metrics", OFFSET(tune_metrics), AV_OPT_TYPE_INT, {.i64 = 0}, 0, 1, VE, "tune-metrics"},

    { "const-qp", "Constant QP", 0, AV_OPT_TYPE_CONST, { .i64 = 0}, 0, 0, VE, "control-rate"},
    { "cbr", "Constant Bitrate", 0, AV_OPT_TYPE_CONST, { .i64 = 1}, 0, 0, VE, "control-rate"},
    { "vbr", "Variable Bitrate", 0, AV_OPT_TYPE_CONST, { .i64 = 2}, 0, 0, VE, "control-rate"},
    { "low-latency", "Low Latency", 0, AV_OPT_TYPE_CONST, { .i64 = 3}, 0, 0, VE, "control-rate"},
    { "auto", "Auto", 0, AV_OPT_TYPE_CONST, { .i64 = -1}, 0, 0, VE, "slice-qp"},
    { "main", "Main profile", 0, AV_OPT_TYPE_CONST, { .i64 = 0}, 0, 0, VE, "profile"},
    { "main-intra", "Main Intra profile", 0, AV_OPT_TYPE_CONST, { .i64 = 1}, 0, 0, VE, "profile"},
    { "1", "1 level", 0, AV_OPT_TYPE_CONST, { .i64 = 10}, 0, 0, VE, "level"},
    { "2", "2 level", 0, AV_OPT_TYPE_CONST, { .i64 = 20}, 0, 0, VE, "level"},
    { "2.1", "2.1 level", 0, AV_OPT_TYPE_CONST, { .i64 = 21}, 0, 0, VE, "level"},
    { "3", "3 level", 0, AV_OPT_TYPE_CONST, { .i64 = 30}, 0, 0, VE, "level"},
    { "3.1", "3.1 level", 0, AV_OPT_TYPE_CONST, { .i64 = 31}, 0, 0, VE, "level"},
    { "4", "4 level", 0, AV_OPT_TYPE_CONST, { .i64 = 40}, 0, 0, VE, "level"},
    { "4.1", "4.1 level", 0, AV_OPT_TYPE_CONST, { .i64 = 41}, 0, 0, VE, "level"},
    { "5", "5 level", 0, AV_OPT_TYPE_CONST, { .i64 = 50}, 0, 0, VE, "level"},
    { "5.1", "5.1 level", 0, AV_OPT_TYPE_CONST, { .i64 = 51}, 0, 0, VE, "level"},
    { "main", "Main tier", 0, AV_OPT_TYPE_CONST, { .i64 = 0}, 0, 0, VE, "tier"},
    { "high", "High tier", 0, AV_OPT_TYPE_CONST, { .i64 = 1}, 0, 0, VE, "tier"},
    { "uniform", "Use the same QP for all coding units of the frame", 0, AV_OPT_TYPE_CONST, { .i64 = 0}, 0, 0, VE, "qp-mode"},
    { "auto", "Let the VCU encoder change the QP for each coding unit according to its content", 0, AV_OPT_TYPE_CONST, { .i64 = 1}, 0, 0, VE, "qp-mode"},
    { "relative-load", "Use the information gathered in the lookahead to calculate the best QP", 0, AV_OPT_TYPE_CONST, { .i64 = 2}, 0, 0, VE, "qp-mode"},
    { "auto", "4:3 for SD video, 16:9 for HD video, unspecified for unknown format", 0, AV_OPT_TYPE_CONST, { .i64 = 0}, 0, 0, VE, "aspect-ratio"},
    { "4:3", "4:3 aspect ratio", 0, AV_OPT_TYPE_CONST, { .i64 = 1}, 0, 0, VE, "aspect-ratio"},
    { "16:9", "16:9 aspect ratio", 0, AV_OPT_TYPE_CONST, { .i64 = 2}, 0, 0, VE, "aspect-ratio"},
    { "none", "Aspect ratio information is not present in the stream", 0, AV_OPT_TYPE_CONST, { .i64 = 3}, 0, 0, VE, "aspect-ratio"},
    { "flat", "Flat scaling list mode", 0, AV_OPT_TYPE_CONST, { .i64 = 0}, 0, 0, VE, "scaling-list"},
    { "default", "Default scaling list mode", 0, AV_OPT_TYPE_CONST, { .i64 = 1}, 0, 0, VE, "scaling-list"},
    { "auto", "Automatic", 0, AV_OPT_TYPE_CONST, { .i64 = 0}, 0, 0, VE, "cores"},
    { "disable", "Disable Temporal AQ", 0, AV_OPT_TYPE_CONST, { .i64 = 0}, 0, 0, VE, "temporal-aq-mode"},
    { "enable", "Enable Temporal AQ", 0, AV_OPT_TYPE_CONST, { .i64 = 1}, 0, 0, VE, "temporal-aq-mode"},
    { "disable", "Disable Spatial AQ", 0, AV_OPT_TYPE_CONST, { .i64 = 0}, 0, 0, VE, "spatial-aq-mode"},
    { "enable", "Enable Spatial AQ", 0, AV_OPT_TYPE_CONST, { .i64 = 1}, 0, 0, VE, "spatial-aq-mode"},
	{ "disable", "Disable tune metrics", 0, AV_OPT_TYPE_CONST, { .i64 = 0}, 0, 0, VE, "tune-metrics"},
    { "enable", "Enable tune metrics", 0, AV_OPT_TYPE_CONST, { .i64 = 1}, 0, 0, VE, "tune-metrics"},
    {NULL},
};

static int mpsoc_report_error(mpsoc_vcu_enc_ctx *ctx, const char *err_str, int32_t err_type)
{
    if (ctx)
    {
        if (ctx->encode_res_inuse == 2)
        {
            av_log(ctx, AV_LOG_ERROR, "encoder error: %s : ffmpeg pid %d on device index =  %d cu index = %d\n",
                  err_str, getpid(), ctx->encode_cu_hw_res.deviceId,
                  ctx->encode_cu_sw_res.cuId);
        }
        else
        {
            av_log(ctx, AV_LOG_ERROR, "encoder error: %s : ffmpeg pid %d on device index =  %d cu index = %d\n",
                  err_str, getpid(), ctx->encode_cu_list_res.cuResources[0].deviceId,
                  ctx->encode_cu_list_res.cuResources[1].cuId);
        }
    }

    return err_type;
}

static void
mpsoc_vcu_encode_queue_pts (AVFifoBuffer* queue, int64_t pts)
{
    av_fifo_generic_write(queue, &pts, sizeof(pts), NULL);
}

static int64_t
mpsoc_vcu_encode_dequeue_pts(AVFifoBuffer* queue)
{
    int64_t ts = AV_NOPTS_VALUE;
    if (av_fifo_size(queue) > 0)
        av_fifo_generic_read(queue, &ts, sizeof(ts), NULL);
    return ts;
}

static bool mpsoc_encode_is_h264_idr(AVPacket *pkt)
{
    unsigned char* pt = pkt->data;
    unsigned char* end = pkt->data + pkt->size - 3;
    while (pt < end)
    {
        if ((pt[0] == 0x00) && (pt[1] == 0x00) && (pt[2] == 0x01) && ((pt[3] & 0x1F) == 0x05))
            return true;
        pt++;
    }
    return false;
}

static bool mpsoc_encode_is_hevc_idr(AVPacket *pkt)
{
    unsigned char* pt = pkt->data;
    unsigned char* end = pkt->data + pkt->size - 3;
    while (pt < end)
    {
        if ((pt[0] == 0x00) && (pt[1] == 0x00) && (pt[2] == 0x01))
        {
            unsigned char naluType = (pt[3] & 0x7E) >> 1;
            if (naluType == 19 || naluType == 20)
                return true;
        }
        pt++;
    }
    return false;
}

static void
mpsoc_vcu_encode_prepare_out_timestamp (AVCodecContext *avctx, AVPacket *pkt)
{
    mpsoc_vcu_enc_ctx *ctx = avctx->priv_data;

    // TODO: code is written based on nvenc, need to check legalities of this
    if (ctx->pts_1 != AV_NOPTS_VALUE && ctx->b_frames > 0 && ctx->is_first_outframe) {
        int64_t ts0 = ctx->pts_0, ts1 = ctx->pts_1;
        int64_t delta;

        if ((ts0 < 0 && ts1 > INT64_MAX + ts0) ||
            (ts0 > 0 && ts1 < INT64_MIN + ts0))
            return;
        delta = ts1 - ts0;

        if ((delta < 0 && ts0 > INT64_MAX + delta) ||
            (delta > 0 && ts0 < INT64_MIN + delta))
          return;
        pkt->dts = ts0 - delta;

        ctx->is_first_outframe = 0;
        return;
    }

    pkt->dts = mpsoc_vcu_encode_dequeue_pts(ctx->pts_queue);
}


static void deinit_la(mpsoc_vcu_enc_ctx *ctx)
{
    if (!ctx->la) {
        return;
    }
    destroy_xlnx_la(ctx->la);
    ctx->la = NULL;
}

static int32_t init_la(AVCodecContext *avctx)
{
    xlnx_la_cfg_t la_cfg;
    mpsoc_vcu_enc_ctx *ctx = avctx->priv_data;
    la_cfg.width = avctx->width;
    la_cfg.height = avctx->height;
    la_cfg.framerate.numerator = avctx->framerate.num;
    la_cfg.framerate.denominator = avctx->framerate.den;
    //@TODO: Assume 256 aligned for now. Needs to be fixed later
    la_cfg.stride = FFALIGN(avctx->width, VCU_STRIDE_ALIGN);
    la_cfg.bits_per_pixel = 8;

    if (avctx->gop_size <= 0) {
        la_cfg.gop_size = 120;
    } else {
        la_cfg.gop_size = avctx->gop_size;
    }

    la_cfg.lookahead_depth = ctx->lookahead_depth;
    la_cfg.spatial_aq_mode = ctx->spatial_aq;
    la_cfg.spatial_aq_gain = ctx->spatial_aq_gain;
    la_cfg.temporal_aq_mode = ctx->temporal_aq;
    la_cfg.rate_control_mode = ctx->rate_control_mode;
    la_cfg.b_frames = ctx->b_frames;
    la_cfg.framerate.numerator   = avctx->framerate.num;
    la_cfg.framerate.denominator = avctx->framerate.den;
    la_cfg.latency_logging = ctx->latency_logging;
    switch (avctx->pix_fmt) {
    case AV_PIX_FMT_NV12:
        la_cfg.enableHwInBuf = 0;
        la_cfg.fmt_type = XMA_VCU_NV12_FMT_TYPE;
        break;
    case AV_PIX_FMT_XVBM:
        la_cfg.enableHwInBuf = 1;
        la_cfg.fmt_type = XMA_VCU_NV12_FMT_TYPE;
        break;
    case AV_PIX_FMT_YUV420P:
        la_cfg.enableHwInBuf = 0;
        la_cfg.fmt_type = XMA_YUV420_FMT_TYPE;
        break;
    }
    switch (avctx->codec_id) {
    case AV_CODEC_ID_H264:
        la_cfg.codec_type = EXlnxAvc;
        break;
    case AV_CODEC_ID_HEVC:
        la_cfg.codec_type = EXlnxHevc;
        break;
    }

    ctx->la = create_xlnx_la(&la_cfg);
    if (!ctx->la) {
        av_log(NULL, AV_LOG_ERROR, "Error : init_la : create_xlnx_la Failed OOM\n");
        return AVERROR(ENOMEM);
    }
    return 0;
}

static av_cold int mpsoc_vcu_encode_close(AVCodecContext *avctx)
{
    mpsoc_vcu_enc_ctx *ctx = avctx->priv_data;

    if (getenv("XRM_RESERVE_ID"))
    {
       int xrm_reserve_id = atoi(getenv("XRM_RESERVE_ID"));

       //XRM encoder de-allocation
       if (ctx->encode_res_inuse ==1)
       {
          if (!(xrmCuListRelease(ctx->xrm_ctx, &ctx->encode_cu_list_res)))
             av_log(avctx, AV_LOG_ERROR, "XRM: failed to release encoder cu\n");
       }
    }
    else if (ctx->encode_res_inuse == 2)
    {

          if (!(xrmCuRelease(ctx->xrm_ctx, &ctx->encode_cu_hw_res)))
             av_log(avctx, AV_LOG_ERROR, "XRM: failed to release encoder HW cu\n");
          if (!(xrmCuRelease(ctx->xrm_ctx, &ctx->encode_cu_sw_res)))
             av_log(avctx, AV_LOG_ERROR, "XRM: failed to release encoder SW cu\n");
    }

    av_fifo_freep(&ctx->pts_queue);
    if (xrmDestroyContext(ctx->xrm_ctx) != XRM_SUCCESS)
       av_log(NULL, AV_LOG_ERROR, "XRM : encoder destroy context failed\n");

     xma_enc_session_destroy(ctx->enc_session);
    deinit_la(ctx);
    if(ctx->la_in_frame) free(ctx->la_in_frame);
    ctx->la_in_frame = NULL;
    return 0;
}

static int check_expert_value(AVDictionaryEntry *entry, int min, int max)
{
	int val = atoi(entry->value);

	// Check for the case when atoi(x) returns 0 when the input is not a number
	if (val == 0 && *(entry->value) != '0'){
		av_log (NULL, AV_LOG_ERROR, "[FFMPEG] ERROR: For expert setting %s, value=%s is invalid; using default value instead\n", entry->key, entry->value);
		return -1;
	}

	if (val >= min && val <= max)
		return val;
	else {
		av_log (NULL, AV_LOG_ERROR, "[FFMPEG] ERROR: For expert option %s, value=%s is out of range, valid range is [%d, %d]; using default value instead\n", entry->key, entry->value, min, max);
		return -1;
	}
}

static int fill_options_file_h264 (AVCodecContext *avctx)
{
    mpsoc_vcu_enc_ctx *ctx = avctx->priv_data;

	// Initialize default values for expert settings
	ctx->cpb_size = 2.0;
	ctx->initial_delay = 1.0;
	ctx->gop_mode = 0;
	ctx->gdr_mode = 0;
	ctx->filler_data = 0;
	ctx->slice_size = 0;
	ctx->entropy_mode = 1;
	ctx->loop_filter = 1;
	ctx->constrained_intra_pred = 0;
	ctx->prefetch_buffer = 1;
	ctx->lookahead_rc_off = 0;

	// Parse and store CLI expert settings
	if (ctx->expert_options != NULL){
		AVDictionary *dict = NULL;
		AVDictionaryEntry *entry = NULL;

		if (!av_dict_parse_string(&dict, ctx->expert_options, "=", ":", 0)) {
			double fval;
			int ret;
			// Iterate through all the dictionary entries
			while ((entry = av_dict_get(dict, "", entry, AV_DICT_IGNORE_SUFFIX))) {
				if (strcmp(entry->key, "cpb-size") == 0){
					fval = atof(entry->value);
					if (fval == 0 && *(entry->value) != '0'){
						av_log(NULL, AV_LOG_ERROR, "[FFMPEG] ERROR: For expert setting %s, value=%s is invalid; using default value instead\n", entry->key, entry->value);
					}
					else if (fval > 0.0 && fval <= 100){
						ctx->cpb_size = fval;
						//printf("EXPERT SETTING: key=%s, value=%f\n", entry->key, ctx->cpb_size);
					}
					else
						av_log(NULL, AV_LOG_ERROR, "[FFMPEG] ERROR: For expert setting %s, value=%s is out of range, valid range is [0.0 < value <= 100.0]; using default value instead\n", entry->key, entry->value);
				}
				else if (strcmp(entry->key, "initial-delay") == 0){
					fval = atof(entry->value);
					if (fval == 0 && *(entry->value) != '0'){
						av_log(NULL, AV_LOG_ERROR, "[FFMPEG] ERROR: For expert setting %s, value=%s is invalid; using default value instead\n", entry->key, entry->value);
					}
					else if (fval >= 0 && fval <= 100){
						ctx->initial_delay = fval;
						//printf("Expert setting: key=%s, value=%f\n", entry->key, ctx->initial_delay);
					}
					else
						av_log(NULL, AV_LOG_ERROR, "[FFMPEG] ERROR: For expert setting %s, value=%s is out of range, valid range is [0 to 100]; using default value instead\n", entry->key, entry->value);
				}
				else if (strcmp(entry->key, "gop-mode") == 0){
					if ((ret = check_expert_value(entry, 0, 3)) > -1){
						ctx->gop_mode = (int)ret;
						//printf("EXPERT SETTING: key=%s, value=%d\n", entry->key, ctx->gop_mode);
					}
				}
				else if (strcmp(entry->key, "gdr-mode") == 0){
					if ((ret = check_expert_value(entry, 0, 2)) > -1){
						ctx->gdr_mode = (int)ret;
						//printf("EXPERT SETTING: key=%s, value=%d\n", entry->key, ctx->gdr_mode);
					}
				}
				else if (strcmp(entry->key, "filler-data") == 0){
					if ((ret = check_expert_value(entry, 0, 1)) > -1){
						ctx->filler_data = (int)ret;
						//printf("EXPERT SETTING: key=%s, value=%d\n", entry->key, ctx->filler_data);
					}
				}
				else if (strcmp(entry->key, "slice-size") == 0){
					if ((ret = check_expert_value(entry, 0, 65535)) > -1){
						ctx->slice_size = (int)ret;
						//printf("EXPERT SETTING: key=%s, value=%d\n", entry->key, ctx->slice_size);
					}
				}
				else if (strcmp(entry->key, "entropy-mode") == 0){
					if ((ret = check_expert_value(entry, 0, 1)) > -1){
						ctx->entropy_mode = (int)ret;
						//printf("EXPERT SETTING: key=%s, value=%d\n", entry->key, ctx->entropy_mode);
					}
				}
				else if (strcmp(entry->key, "loop-filter") == 0){
					if ((ret = check_expert_value(entry, 0, 1)) > -1){
						ctx->loop_filter = (int)ret;
						//printf("EXPERT SETTING: key=%s, value=%d\n", entry->key, ctx->loop_filter);
					}
				}
				else if (strcmp(entry->key, "constrained-intra-pred") == 0){
					if ((ret = check_expert_value(entry, 0, 1)) > -1){
						ctx->constrained_intra_pred = (int)ret;
						//printf("EXPERT SETTING: key=%s, value=%d\n", entry->key, ctx->constrained_intra_pred);
					}
				}
				else if (strcmp(entry->key, "prefetch-buffer") == 0){
					if ((ret = check_expert_value(entry, 0, 1)) > -1){
						ctx->prefetch_buffer = (int)ret;
						//printf("EXPERT SETTING: key=%s, value=%d\n", entry->key, ctx->prefetch_buffer);
					}
				}
				else if (strcmp(entry->key, "lookahead-rc-off") == 0){
					if ((ret = check_expert_value(entry, 0, 1)) > -1){
						ctx->lookahead_rc_off = (int)ret;
						av_log(avctx, AV_LOG_DEBUG, "EXPERT SETTING: key=%s, value=%d\n", entry->key, ctx->lookahead_rc_off);
					}
				}
				else
					av_log(NULL, AV_LOG_ERROR, "[FFMPEG] ERROR: Expert setting '%s' does not exist, check for spelling mistakes or the naming convention...\n", entry->key);
			}
		}
		av_dict_free(&dict);
	}

	// Enable Adaptive Quantization by default, if lookahead is enabled
	if (ctx->lookahead_depth >= 1 && (ctx->temporal_aq == 1 || ctx->spatial_aq == 1) && ctx->tune_metrics == 0){
		ctx->qp_mode = 2;
	}
	else if (ctx->lookahead_depth == 0 || ctx->tune_metrics == 1)
	{
		if (ctx->temporal_aq)
		    ctx->temporal_aq = 0;

		if (ctx->spatial_aq)
		    ctx->spatial_aq = 0;
	}

	// Tunes video quality for objective scores by setting flat scaling-list and uniform qp-mode
	if (ctx->tune_metrics){
		ctx->scaling_list = 0;
		ctx->qp_mode = 0;
	}

    const char* RateCtrlMode = "CONST_QP";
    switch (ctx->control_rate) {
        case 0: RateCtrlMode = "CONST_QP"; break;
        case 1: RateCtrlMode = "CBR"; break;
        case 2: RateCtrlMode = "VBR"; break;
        case 3: RateCtrlMode = "LOW_LATENCY"; break;
    }

    char FrameRate[16];
    sprintf(FrameRate, "%llu/%llu", avctx->framerate.num, avctx->framerate.den);

    char SliceQP[8];
    if (ctx->slice_qp == -1)
        strcpy (SliceQP, "AUTO");
    else
        sprintf(SliceQP, "%d", ctx->slice_qp);

    const char* GopCtrlMode = "DEFAULT_GOP";
    switch (ctx->gop_mode) {
        case 0: GopCtrlMode = "DEFAULT_GOP"; break;
        case 1: GopCtrlMode = "PYRAMIDAL_GOP"; break;
        case 2: GopCtrlMode = "LOW_DELAY_P"; break;
        case 3: GopCtrlMode = "LOW_DELAY_B"; break;
    }

    const char* GDRMode = "DISABLE";
    switch (ctx->gdr_mode) {
        case 0: GDRMode = "DISABLE"; break;
        case 1: GDRMode = "GDR_VERTICAL"; break;
        case 2: GDRMode = "GDR_HORIZONTAL"; break;
    }

    const char* Profile = "AVC_BASELINE";
    switch (ctx->profile) {
        case FF_PROFILE_H264_BASELINE: Profile = "AVC_BASELINE"; break;
        case FF_PROFILE_H264_MAIN: Profile = "AVC_MAIN"; break;
        case FF_PROFILE_H264_HIGH: Profile = "AVC_HIGH"; break;
        default:
            av_log(ctx, AV_LOG_ERROR, "[FFMPEG] ERROR: Invalid H264 codec profile value %d \n", ctx->profile);
            return XMA_ERROR;
    }

    const char* Level = "1";
    switch (ctx->level) {
        case 10: Level = "1"; break;
        case 11: Level = "1.1"; break;
        case 12: Level = "1.2"; break;
        case 13: Level = "1.3"; break;
        case 20: Level = "2"; break;
        case 21: Level = "2.1"; break;
        case 22: Level = "2.2"; break;
        case 30: Level = "3"; break;
        case 31: Level = "3.1"; break;
        case 32: Level = "3.2"; break;
        case 40: Level = "4"; break;
        case 41: Level = "4.1"; break;
        case 42: Level = "4.2"; break;
        case 50: Level = "5"; break;
        case 51: Level = "5.1"; break;
        case 52: Level = "5.2"; break;
    }

    const char* QPCtrlMode = "UNIFORM_QP";
    switch (ctx->qp_mode) {
        case 0: QPCtrlMode = "UNIFORM_QP"; break;
        case 1: QPCtrlMode = "AUTO_QP"; break;
        case 2: QPCtrlMode = "LOAD_QP | RELATIVE_QP"; break;
    }

    const char* FillerData = "DISABLE";
    switch (ctx->filler_data) {
        case 0: FillerData = "DISABLE"; break;
        case 1: FillerData = "ENABLE"; break;
    }

    const char* AspectRatio = "ASPECT_RATIO_AUTO";
    switch (ctx->aspect_ratio) {
        case 0: AspectRatio = "ASPECT_RATIO_AUTO"; break;
        case 1: AspectRatio = "ASPECT_RATIO_4_3"; break;
        case 2: AspectRatio = "ASPECT_RATIO_16_9"; break;
        case 3: AspectRatio = "ASPECT_RATIO_NONE"; break;
    }

    const char* ColorSpace = "COLOUR_DESC_UNSPECIFIED";
    switch (avctx->colorspace) {
        case AVCOL_SPC_BT709: ColorSpace = "COLOUR_DESC_BT_709"; break;
        case AVCOL_SPC_UNSPECIFIED: ColorSpace = "COLOUR_DESC_UNSPECIFIED"; break;
        case AVCOL_SPC_RESERVED: ColorSpace = "COLOUR_DESC_RESERVED"; break;
        case AVCOL_SPC_BT470BG: ColorSpace = "COLOUR_DESC_BT_470_NTSC"; break;
        case AVCOL_SPC_SMPTE170M: ColorSpace = "COLOUR_DESC_BT_601_PAL"; break;
        case AVCOL_SPC_SMPTE240M: ColorSpace = "COLOUR_DESC_BT_601_NTSC"; break;
        case AVCOL_SPC_BT2020_NCL: ColorSpace = "COLOUR_DESC_BT_2020"; break;
        case AVCOL_SPC_BT2020_CL: ColorSpace = "COLOUR_DESC_BT_2020"; break;
    }

    const char* ScalingList = "FLAT";
    switch (ctx->scaling_list) {
        case 0: ScalingList = "FLAT"; break;
        case 1: ScalingList = "DEFAULT"; break;
    }

    const char* EntropyMode = "MODE_CABAC";
    switch (ctx->entropy_mode) {
        case 0: EntropyMode = "MODE_CAVLC"; break;
        case 1: EntropyMode = "MODE_CABAC"; break;
    }

    const char* LoopFilter = "ENABLE";
    switch (ctx->loop_filter) {
        case 0: LoopFilter = "DISABLE"; break;
        case 1: LoopFilter = "ENABLE"; break;
    }

    const char* ConstIntraPred = "DISABLE";
    switch (ctx->constrained_intra_pred) {
        case 0: ConstIntraPred = "DISABLE"; break;
        case 1: ConstIntraPred = "ENABLE"; break;
    }

    const char* LambdaCtrlMode = "DEFAULT_LDA";

    const char* PrefetchBuffer = "ENABLE";
    switch (ctx->prefetch_buffer) {
        case 0: PrefetchBuffer = "DISABLE"; break;
        case 1: PrefetchBuffer = "ENABLE"; break;
    }

	av_log(avctx, AV_LOG_DEBUG, "qp-mode = %d \n", ctx->qp_mode);
	av_log(avctx, AV_LOG_DEBUG, "spatial-aq = %d \n", ctx->spatial_aq);
	av_log(avctx, AV_LOG_DEBUG, "temporal-aq = %d \n", ctx->temporal_aq);

	// Set IDR period to gop-size, when the user has not specified it on the command line
	if (ctx->periodicity_idr == -1)
	{
		if (avctx->gop_size > 0){
			ctx->periodicity_idr = avctx->gop_size;
		}
		av_log(avctx, AV_LOG_DEBUG, "ctx->periodicity_idr = %d \n", ctx->periodicity_idr);
	}

    sprintf (ctx->enc_options, "[INPUT]\n"
            "Width = %d\n"
            "Height = %d\n"
            "[RATE_CONTROL]\n"
            "RateCtrlMode = %s\n"
            "FrameRate = %s\n"
            "BitRate = %ld\n"
            "MaxBitRate = %ld\n"
            "SliceQP = %s\n"
            "MaxQP = %d\n"
            "MinQP = %d\n"
            "CPBSize = %f\n"
            "InitialDelay = %f\n"
            "[GOP]\n"
            "GopCtrlMode = %s\n"
            "Gop.GdrMode = %s\n"
            "Gop.Length = %d\n"
            "Gop.NumB = %d\n"
            "Gop.FreqIDR = %d\n"
            "[SETTINGS]\n"
            "Profile = %s\n"
            "Level = %s\n"
            "ChromaMode = CHROMA_4_2_0\n"
            "BitDepth = 8\n"
            "NumSlices = %d\n"
            "QPCtrlMode = %s\n"
            "SliceSize = %d\n"
            "EnableFillerData = %s\n"
            "AspectRatio = %s\n"
            "ColourDescription = %s\n"
            "ScalingList = %s\n"
            "EntropyMode = %s\n"
            "LoopFilter = %s\n"
            "ConstrainedIntraPred = %s\n"
            "LambdaCtrlMode = %s\n"
            "CacheLevel2 = %s\n"
            "NumCore = %d\n",
            avctx->width, avctx->height, RateCtrlMode, FrameRate, avctx->bit_rate / 1000,
            ctx->max_bitrate / 1000, SliceQP, ctx->max_qp, ctx->min_qp, ctx->cpb_size,
            ctx->initial_delay, GopCtrlMode, GDRMode, avctx->gop_size, ctx->b_frames,
            ctx->periodicity_idr, Profile, Level, ctx->num_slices, QPCtrlMode, ctx->slice_size,
            FillerData, AspectRatio, ColorSpace, ScalingList, EntropyMode, LoopFilter,
            ConstIntraPred, LambdaCtrlMode, PrefetchBuffer, ctx->cores);

    return XMA_SUCCESS;
}

static void fill_options_file_hevc (AVCodecContext *avctx)
{
    mpsoc_vcu_enc_ctx *ctx = avctx->priv_data;

	// Initialize default values for expert settings
	ctx->cpb_size = 2.0;
	ctx->initial_delay = 1.0;
	ctx->gop_mode = 0;
	ctx->gdr_mode = 0;
	ctx->filler_data = 0;
	ctx->dependent_slice = 0;
	ctx->slice_size = 0;
	ctx->loop_filter = 1;
	ctx->constrained_intra_pred = 0;
	ctx->prefetch_buffer = 1;
	ctx->lookahead_rc_off = 0;

	// Parse and store CLI expert settings
	if (ctx->expert_options != NULL){
		AVDictionary *dict = NULL;
		AVDictionaryEntry *entry = NULL;

		if (!av_dict_parse_string(&dict, ctx->expert_options, "=", ":", 0)) {
			// Iterate through all the dictionary entries
			double fval;
			int ret;
			while ((entry = av_dict_get(dict, "", entry, AV_DICT_IGNORE_SUFFIX))) {
				if (strcmp(entry->key, "cpb-size") == 0){
					fval = atof(entry->value);
					if (fval == 0 && *(entry->value) != '0'){
						av_log(NULL, AV_LOG_ERROR, "[FFMPEG] ERROR: For expert setting %s, value=%s is invalid; using default value instead\n", entry->key, entry->value);
					}
					else if (fval > 0.0 && fval <= 100){
						ctx->cpb_size = fval;
						//printf("EXPERT SETTING: key=%s, value=%f\n", entry->key, ctx->cpb_size);
					}
					else
						av_log(NULL, AV_LOG_ERROR, "[FFMPEG] ERROR: For expert setting %s, value=%s is out of range, valid range is [0.0 < value <= 100.0]; using default value instead\n", entry->key, entry->value);
				}
				else if (strcmp(entry->key, "initial-delay") == 0){
					fval = atof(entry->value);
					if (fval == 0 && *(entry->value) != '0'){
						av_log(NULL, AV_LOG_ERROR, "[FFMPEG] ERROR: For expert setting %s, value=%s is invalid; using default value instead\n", entry->key, entry->value);
					}
					else if (fval >= 0 && fval <= 100){
						ctx->initial_delay = fval;
						//printf("EXPERT SETTING: key=%s, value=%f\n", entry->key, ctx->initial_delay);
					}
					else
						av_log(NULL, AV_LOG_ERROR, "[FFMPEG] ERROR: For expert setting %s, value=%s is out of range, valid range is [0 to 100]; using default value instead\n", entry->key, entry->value);
				}
				else if (strcmp(entry->key, "gop-mode") == 0){
					if ((ret = check_expert_value(entry, 0, 3)) > -1){
						ctx->gop_mode = (int)ret;
						//printf("EXPERT SETTING: key=%s, value=%d\n", entry->key, ctx->gop_mode);
					}
				}
				else if (strcmp(entry->key, "gdr-mode") == 0){
					if ((ret = check_expert_value(entry, 0, 2)) > -1){
						ctx->gdr_mode = (int)ret;
						//printf("EXPERT SETTING: key=%s, value=%d\n", entry->key, ctx->gdr_mode);
					}
				}
				else if (strcmp(entry->key, "filler-data") == 0){
					if ((ret = check_expert_value(entry, 0, 1)) > -1){
						ctx->filler_data = (int)ret;
						//printf("EXPERT SETTING: key=%s, value=%d\n", entry->key, ctx->filler_data);
					}
				}
				else if (strcmp(entry->key, "dependent-slice") == 0){
					if ((ret = check_expert_value(entry, 0, 1)) > -1){
						ctx->dependent_slice = (int)ret;
						//printf("EXPERT SETTING: key=%s, value=%d\n", entry->key, ctx->dependent_slice);
					}
				}
				else if (strcmp(entry->key, "slice-size") == 0){
					if ((ret = check_expert_value(entry, 0, 65535)) > -1){
						ctx->slice_size = (int)ret;
						//printf("EXPERT SETTING: key=%s, value=%d\n", entry->key, ctx->slice_size);
					}
				}
				else if (strcmp(entry->key, "loop-filter") == 0){
					if ((ret = check_expert_value(entry, 0, 1)) > -1){
						ctx->loop_filter = (int)ret;
						//printf("EXPERT SETTING: key=%s, value=%d\n", entry->key, ctx->loop_filter);
					}
				}
				else if (strcmp(entry->key, "constrained-intra-pred") == 0){
					if ((ret = check_expert_value(entry, 0, 1)) > -1){
						ctx->constrained_intra_pred = (int)ret;
						//printf("EXPERT SETTING: key=%s, value=%d\n", entry->key, ctx->constrained_intra_pred);
					}
				}
				else if (strcmp(entry->key, "prefetch-buffer") == 0){
					if ((ret = check_expert_value(entry, 0, 1)) > -1){
						ctx->prefetch_buffer = (int)ret;
						//printf("EXPERT SETTING: key=%s, value=%d\n", entry->key, ctx->prefetch_buffer);
					}
				}
				else if (strcmp(entry->key, "lookahead-rc-off") == 0){
					if ((ret = check_expert_value(entry, 0, 1)) > -1){
						ctx->lookahead_rc_off = (int)ret;
						av_log(avctx, AV_LOG_DEBUG, "EXPERT SETTING: key=%s, value=%d\n", entry->key, ctx->lookahead_rc_off);
					}
				}
				else
					av_log(NULL, AV_LOG_ERROR, "[FFMPEG] ERROR: Expert setting '%s' does not exist, check for spelling mistakes or the naming convention...\n", entry->key);
			}
		}
		av_dict_free(&dict);
	}

	// Enable Adaptive Quantization by default, if lookahead is enabled
	if (ctx->lookahead_depth >= 1 && (ctx->temporal_aq == 1 || ctx->spatial_aq == 1) && ctx->tune_metrics == 0){
		ctx->qp_mode = 2;
	}
	else if (ctx->lookahead_depth == 0 || ctx->tune_metrics == 1)
	{
		if (ctx->temporal_aq)
		    ctx->temporal_aq = 0;

		if (ctx->spatial_aq)
		    ctx->spatial_aq = 0;
	}

	// Tunes video quality for objective scores by setting flat scaling-list and uniform qp-mode
	if (ctx->tune_metrics){
		ctx->scaling_list = 0;
		ctx->qp_mode = 0;
	}

    const char* RateCtrlMode = "CONST_QP";
    switch (ctx->control_rate) {
        case 0: RateCtrlMode = "CONST_QP"; break;
        case 1: RateCtrlMode = "CBR"; break;
        case 2: RateCtrlMode = "VBR"; break;
        case 3: RateCtrlMode = "LOW_LATENCY"; break;
    }

    char FrameRate[16];
    sprintf(FrameRate, "%llu/%llu", avctx->framerate.num, avctx->framerate.den);

    char SliceQP[8];
    if (ctx->slice_qp == -1)
        strcpy (SliceQP, "AUTO");
    else
        sprintf(SliceQP, "%d", ctx->slice_qp);

    const char* GopCtrlMode = "DEFAULT_GOP";
    switch (ctx->gop_mode) {
        case 0: GopCtrlMode = "DEFAULT_GOP"; break;
        case 1: GopCtrlMode = "PYRAMIDAL_GOP"; break;
        case 2: GopCtrlMode = "LOW_DELAY_P"; break;
        case 3: GopCtrlMode = "LOW_DELAY_B"; break;
    }

    const char* GDRMode = "DISABLE";
    switch (ctx->gdr_mode) {
        case 0: GDRMode = "DISABLE"; break;
        case 1: GDRMode = "GDR_VERTICAL"; break;
        case 2: GDRMode = "GDR_HORIZONTAL"; break;
    }

    const char* Profile = "HEVC_MAIN";
    switch (ctx->profile) {
        case 0: Profile = "HEVC_MAIN"; break;
        case 1: Profile = "HEVC_MAIN_INTRA"; break;
    }

    const char* Level = "1";
    switch (ctx->level) {
        case 10: Level = "1"; break;
        case 20: Level = "2"; break;
        case 21: Level = "2.1"; break;
        case 30: Level = "3"; break;
        case 31: Level = "3.1"; break;
        case 40: Level = "4"; break;
        case 41: Level = "4.1"; break;
        case 50: Level = "5"; break;
        case 51: Level = "5.1"; break;
    }

    const char* Tier = "MAIN_TIER";
    switch (ctx->tier) {
        case 0: Tier = "MAIN_TIER"; break;
        case 1: Tier = "HIGH_TIER"; break;
    }

    const char* QPCtrlMode = "UNIFORM_QP";
    switch (ctx->qp_mode) {
        case 0: QPCtrlMode = "UNIFORM_QP"; break;
        case 1: QPCtrlMode = "AUTO_QP"; break;
        case 2: QPCtrlMode = "LOAD_QP | RELATIVE_QP"; break;
    }

    const char* DependentSlice = "FALSE";
    switch (ctx->dependent_slice) {
        case 0: DependentSlice = "FALSE"; break;
        case 1: DependentSlice = "TRUE"; break;
    }

    const char* FillerData = "DISABLE";
    switch (ctx->filler_data) {
        case 0: FillerData = "DISABLE"; break;
        case 1: FillerData = "ENABLE"; break;
    }

    const char* AspectRatio = "ASPECT_RATIO_AUTO";
    switch (ctx->aspect_ratio) {
        case 0: AspectRatio = "ASPECT_RATIO_AUTO"; break;
        case 1: AspectRatio = "ASPECT_RATIO_4_3"; break;
        case 2: AspectRatio = "ASPECT_RATIO_16_9"; break;
        case 3: AspectRatio = "ASPECT_RATIO_NONE"; break;
    }

    const char* ColorSpace = "COLOUR_DESC_UNSPECIFIED";
    switch (avctx->colorspace) {
        case AVCOL_SPC_BT709: ColorSpace = "COLOUR_DESC_BT_709"; break;
        case AVCOL_SPC_UNSPECIFIED: ColorSpace = "COLOUR_DESC_UNSPECIFIED"; break;
        case AVCOL_SPC_RESERVED: ColorSpace = "COLOUR_DESC_RESERVED"; break;
        case AVCOL_SPC_BT470BG: ColorSpace = "COLOUR_DESC_BT_470_NTSC"; break;
        case AVCOL_SPC_SMPTE170M: ColorSpace = "COLOUR_DESC_BT_601_PAL"; break;
        case AVCOL_SPC_SMPTE240M: ColorSpace = "COLOUR_DESC_BT_601_NTSC"; break;
        case AVCOL_SPC_BT2020_NCL: ColorSpace = "COLOUR_DESC_BT_2020"; break;
        case AVCOL_SPC_BT2020_CL: ColorSpace = "COLOUR_DESC_BT_2020"; break;
    }

    const char* ScalingList = "FLAT";
    switch (ctx->scaling_list) {
        case 0: ScalingList = "FLAT"; break;
        case 1: ScalingList = "DEFAULT"; break;
    }

    const char* LoopFilter = "ENABLE";
    switch (ctx->loop_filter) {
        case 0: LoopFilter = "DISABLE"; break;
        case 1: LoopFilter = "ENABLE"; break;
    }

    const char* ConstIntraPred = "DISABLE";
    switch (ctx->constrained_intra_pred) {
        case 0: ConstIntraPred = "DISABLE"; break;
        case 1: ConstIntraPred = "ENABLE"; break;
    }

    const char* LambdaCtrlMode = "DEFAULT_LDA";

    const char* PrefetchBuffer = "ENABLE";
    switch (ctx->prefetch_buffer) {
        case 0: PrefetchBuffer = "DISABLE"; break;
        case 1: PrefetchBuffer = "ENABLE"; break;
    }

	av_log(avctx, AV_LOG_DEBUG, "qp-mode = %d \n", ctx->qp_mode);
	av_log(avctx, AV_LOG_DEBUG, "spatial-aq = %d \n", ctx->spatial_aq);
	av_log(avctx, AV_LOG_DEBUG, "temporal-aq = %d \n", ctx->temporal_aq);

	// Set IDR period to gop-size, when the user has not specified it on the command line
	if (ctx->periodicity_idr == -1)
	{
		if (avctx->gop_size > 0){
			ctx->periodicity_idr = avctx->gop_size;
		}
		av_log(avctx, AV_LOG_DEBUG, "ctx->periodicity_idr = %d \n", ctx->periodicity_idr);
	}

    sprintf (ctx->enc_options, "[INPUT]\n"
            "Width = %d\n"
            "Height = %d\n"
            "[RATE_CONTROL]\n"
            "RateCtrlMode = %s\n"
            "FrameRate = %s\n"
            "BitRate = %ld\n"
            "MaxBitRate = %ld\n"
            "SliceQP = %s\n"
            "MaxQP = %d\n"
            "MinQP = %d\n"
            "CPBSize = %f\n"
            "InitialDelay = %f\n"
            "[GOP]\n"
            "GopCtrlMode = %s\n"
            "Gop.GdrMode = %s\n"
            "Gop.Length = %d\n"
            "Gop.NumB = %d\n"
            "Gop.FreqIDR = %d\n"
            "[SETTINGS]\n"
            "Profile = %s\n"
            "Level = %s\n"
            "Tier = %s\n"
            "ChromaMode = CHROMA_4_2_0\n"
            "BitDepth = 8\n"
            "NumSlices = %d\n"
            "QPCtrlMode = %s\n"
            "SliceSize = %d\n"
            "DependentSlice = %s\n"
            "EnableFillerData = %s\n"
            "AspectRatio = %s\n"
            "ColourDescription = %s\n"
            "ScalingList = %s\n"
            "LoopFilter = %s\n"
            "ConstrainedIntraPred = %s\n"
            "LambdaCtrlMode = %s\n"
            "CacheLevel2 = %s\n"
            "NumCore = %d\n",
            avctx->width, avctx->height, RateCtrlMode, FrameRate, avctx->bit_rate / 1000,
            ctx->max_bitrate / 1000, SliceQP, ctx->max_qp, ctx->min_qp, ctx->cpb_size,
            ctx->initial_delay, GopCtrlMode, GDRMode, avctx->gop_size, ctx->b_frames,
            ctx->periodicity_idr, Profile, Level, Tier, ctx->num_slices, QPCtrlMode,
            ctx->slice_size, DependentSlice, FillerData, AspectRatio, ColorSpace, ScalingList,
            LoopFilter, ConstIntraPred, LambdaCtrlMode, PrefetchBuffer, ctx->cores);
}

#define MIN_LOOKAHEAD_DEPTH	(1)
#define MAX_LOOKAHEAD_DEPTH	(30)


static int _calc_enc_load(xrmContext *xrm_ctx, XmaEncoderProperties *enc_props, int32_t func_id, int32_t *enc_load)
{
   char pluginName[XRM_MAX_NAME_LEN];

    xrmPluginFuncParam param;
    char *err;
    void *handle;
    void (*convertXmaPropsToJson)(void* props, char* funcName, char* jsonJob);

    memset(&param, 0, sizeof(xrmPluginFuncParam));
    handle = dlopen("/opt/xilinx/xrm/plugin/libxmaPropsTOjson.so", RTLD_NOW );
    if (!handle) {
        av_log(NULL, AV_LOG_ERROR, "Unable to load libxmaPropsTOjson.so  - %s\n", dlerror());
        return XMA_ERROR;
    }
    dlerror(); /* clear error code */

    convertXmaPropsToJson = dlsym(handle, "convertXmaPropsToJson");
    if ((err = dlerror()) != NULL) {
        av_log(NULL, AV_LOG_ERROR, "convertXmaPropsToJson symbol not found\n");
        return XMA_ERROR;
    }

    (*convertXmaPropsToJson) (enc_props, "ENCODER",param.input);
    dlclose(handle);

    strcpy(pluginName, "xrmU30EncPlugin");

    if (xrmExecPluginFunc(xrm_ctx, pluginName, func_id, &param) != XRM_SUCCESS)
    {
        av_log(NULL, AV_LOG_ERROR, "xrm_load_calculation: encoder plugin function %d, fail to run the function\n", func_id);
        return XMA_ERROR;
    }
    else
    {
         *enc_load = atoi((char*)(strtok(param.output, " ")));
         if (*enc_load <= 0)
         {
            av_log(NULL, AV_LOG_ERROR, "xrm_load_calculation: encoder plugin function %d, calculated load %d .\n", *enc_load);
            return XMA_ERROR;
         }
         else if (*enc_load > XRM_MAX_CU_LOAD_GRANULARITY_1000000)
         {
            av_log(NULL, AV_LOG_ERROR, "xrm_load_calculation: encoder plugin function %d, calculated load %d is greater than maximum supported.\n", *enc_load);
            return XMA_ERROR;
         }
    }
    return 0;
}

static int _xrm_enc_cuAlloc(mpsoc_vcu_enc_ctx *ctx, int32_t enc_load, XmaEncoderProperties *enc_props)
{
    xrmCuProperty encode_cu_hw_prop, encode_cu_sw_prop;
    int ret = -1;
    int hw_xdevice_id =  atoi(getenv("XRM_DEVICE_ID"));

    memset(&encode_cu_hw_prop, 0, sizeof(xrmCuProperty));
    memset(&encode_cu_sw_prop, 0, sizeof(xrmCuProperty));
    memset(&ctx->encode_cu_hw_res, 0, sizeof(xrmCuResource));
    memset(&ctx->encode_cu_sw_res, 0, sizeof(xrmCuResource));

    strcpy(encode_cu_hw_prop.kernelName, "encoder");
    strcpy(encode_cu_hw_prop.kernelAlias, "ENCODER_MPSOC");
    encode_cu_hw_prop.devExcl = false;
    encode_cu_hw_prop.requestLoad = XRM_PRECISION_1000000_BIT_MASK(enc_load);

    strcpy(encode_cu_sw_prop.kernelName, "kernel_vcu_encoder");
    encode_cu_sw_prop.devExcl = false;
    encode_cu_sw_prop.requestLoad = XRM_PRECISION_1000000_BIT_MASK(XRM_MAX_CU_LOAD_GRANULARITY_1000000);

    ret = xrmCuAllocFromDev(ctx->xrm_ctx, hw_xdevice_id, &encode_cu_hw_prop, &ctx->encode_cu_hw_res);

    if (ret != 0)
    {
        av_log(NULL, AV_LOG_ERROR, "xrm_allocation: failed to allocate encoder resources from device %d\n", hw_xdevice_id );
        return ret;
    }
    else
    {
        ret = xrmCuAllocFromDev(ctx->xrm_ctx, hw_xdevice_id, &encode_cu_sw_prop, &ctx->encode_cu_sw_res);
        if (ret != 0)
        {
           av_log(NULL, AV_LOG_ERROR, "xrm_allocation: failing to allocate encoder resources from device %d\n", hw_xdevice_id );
           return ret;
        }
        else
           ctx->encode_res_inuse =2;
    }

    //Set XMA plugin SO and device index
    enc_props->plugin_lib = ctx->encode_cu_hw_res.kernelPluginFileName;
    enc_props->dev_index = ctx->encode_cu_hw_res.deviceId;
    enc_props->ddr_bank_index = -1;//XMA to select the ddr bank based on xclbin meta data
    enc_props->cu_index = ctx->encode_cu_sw_res.cuId;
    enc_props->channel_id = ctx->encode_cu_sw_res.channelId;//SW kernel always used 100%

    return 0;
}

static int _xrm_enc_cuListAlloc(mpsoc_vcu_enc_ctx *ctx, int32_t enc_load, int32_t xrm_reserve_id, XmaEncoderProperties *enc_props)
{
    xrmCuListProperty encode_cu_list_prop;
    int ret = -1;

    memset(&encode_cu_list_prop, 0, sizeof(xrmCuListProperty));
    memset(&ctx->encode_cu_list_res, 0, sizeof(xrmCuListResource));

    encode_cu_list_prop.cuNum = 2;
    strcpy(encode_cu_list_prop.cuProps[0].kernelName, "encoder");
    strcpy(encode_cu_list_prop.cuProps[0].kernelAlias, "ENCODER_MPSOC");
    encode_cu_list_prop.cuProps[0].devExcl = false;
    encode_cu_list_prop.cuProps[0].requestLoad = XRM_PRECISION_1000000_BIT_MASK(enc_load);
    encode_cu_list_prop.cuProps[0].poolId = xrm_reserve_id;

    strcpy(encode_cu_list_prop.cuProps[1].kernelName, "kernel_vcu_encoder");
    encode_cu_list_prop.cuProps[1].devExcl = false;
    encode_cu_list_prop.cuProps[1].requestLoad = XRM_PRECISION_1000000_BIT_MASK(XRM_MAX_CU_LOAD_GRANULARITY_1000000);
    encode_cu_list_prop.cuProps[1].poolId = xrm_reserve_id;

    ret = xrmCuListAlloc(ctx->xrm_ctx, &encode_cu_list_prop, &ctx->encode_cu_list_res);

    if (ret != 0)
    {
        av_log(NULL, AV_LOG_ERROR, "xrm_allocation: failed to allocate encoder cu  from reserve\n");
        return XMA_ERROR;
    }
    else
    {
        ctx->encode_res_inuse = 1;
    }

    //Set XMA plugin SO and device index
    enc_props->plugin_lib = ctx->encode_cu_list_res.cuResources[0].kernelPluginFileName;
    enc_props->dev_index = ctx->encode_cu_list_res.cuResources[0].deviceId;
    enc_props->ddr_bank_index = -1;//XMA to select the ddr bank based on xclbin meta data
    enc_props->cu_index = ctx->encode_cu_list_res.cuResources[1].cuId;
    enc_props->channel_id = ctx->encode_cu_list_res.cuResources[1].channelId;

    return 0;
}

static int  _allocate_xrm_enc_cu(mpsoc_vcu_enc_ctx *ctx, XmaEncoderProperties *enc_props)
{
    int xrm_reserve_id = -1;
    int ret =-1;

    //create XRM local context
    ctx->xrm_ctx = (xrmContext *)xrmCreateContext(XRM_API_VERSION_1);
    if (ctx->xrm_ctx == NULL)
    {
        av_log(NULL, AV_LOG_ERROR, "create local XRM context failed\n");
        return XMA_ERROR;
    }

    //XRM encoder plugin load calculation
    int func_id = 0, enc_load=0;
    ret = _calc_enc_load(ctx->xrm_ctx, enc_props, func_id, &enc_load);
    if (ret < 0) return ret;

    if (getenv("XRM_RESERVE_ID"))
    {
       xrm_reserve_id = atoi(getenv("XRM_RESERVE_ID"));
       ret = _xrm_enc_cuListAlloc(ctx, enc_load, xrm_reserve_id, enc_props);
       if (ret < 0) return ret;
    }
    else
    {
       ret = _xrm_enc_cuAlloc(ctx, enc_load, enc_props);
       if (ret < 0) return ret;
    }
    av_log(NULL, AV_LOG_DEBUG, "---encoder xrm out: enc_load=%d, plugin=%s, device=%d, cu=%d, ch=%d\n", 
    enc_load, enc_props->plugin_lib, enc_props->dev_index, enc_props->cu_index, enc_props->channel_id );
    return ret;
}

static av_cold int mpsoc_vcu_encode_init(AVCodecContext *avctx)
{
    XmaEncoderProperties enc_props;
    mpsoc_vcu_enc_ctx *ctx = avctx->priv_data;

    if ((avctx->width > MAX_ENC_WIDTH) || (avctx->height > MAX_ENC_WIDTH) ||
        ((avctx->width * avctx->height) > MAX_ENC_PIXELS)) {
        av_log(avctx, AV_LOG_ERROR, "input resolution %dx%d exceeds maximum supported resolution (%dx%d)\n",
               avctx->width, avctx->height, MAX_ENC_WIDTH, MAX_ENC_HEIGHT);
        return AVERROR(EINVAL);
    }

    if (ctx->lookahead_depth > avctx->gop_size) {
        av_log(avctx, AV_LOG_ERROR, "Error : mpsoc_vcu_encode_frame : Invalid arguments. gop size(%d) should be grater than lookahead_depth(%d)\n", avctx->gop_size, ctx->lookahead_depth);
        return AVERROR(EINVAL);
    }

    if (avctx->codec_id == AV_CODEC_ID_H264)
    {
        int ret = fill_options_file_h264 (avctx);
        if (ret == XMA_ERROR)
            return ret;
    }
    else if (avctx->codec_id == AV_CODEC_ID_HEVC) fill_options_file_hevc (avctx);
    else {
        av_log(NULL, AV_LOG_ERROR, "Unknown codec id!\n");
        return AVERROR_ENCODER_NOT_FOUND;
    }
    enc_props.hwencoder_type = XMA_MULTI_ENCODER_TYPE;
    strcpy(enc_props.hwvendor_string, "MPSoC");

    enc_props.format = XMA_VCU_NV12_FMT_TYPE;
    enc_props.bits_per_pixel  = 8;
    enc_props.param_cnt = 0;
    enc_props.params = ctx->enc_params;
    enc_props.width = avctx->width;
    enc_props.height = avctx->height;

	// Enable custom rate control when rate control is set to CBR and lookahead is set, disable when expert option lookahead-rc-off is set.
	if (ctx->control_rate == 1 && ctx->lookahead_depth > 1 && ctx->lookahead_rc_off == 0){
		ctx->rate_control_mode = 1;
	}
	else if (ctx->control_rate == 1 && ctx->lookahead_depth > 1 && ctx->lookahead_rc_off == 1){
		ctx->rate_control_mode = 0;
	}

    enc_props.rc_mode =  ctx->rate_control_mode;

    if (ctx->lookahead_depth)
    {
        //Validate input resolution against MAX supported
        if ((enc_props.width > MAX_LA_INPUT_WIDTH) || //for landscape use-case
            (enc_props.height > MAX_LA_INPUT_WIDTH) || //for portrait use-case
            ((enc_props.width * enc_props.height) > MAX_LA_INPUT_PIXELS)) {
            av_log (ctx, AV_LOG_ERROR, "\n Input to encoder for look ahead usecase %4dx%4d exceeds max supported resolution %4dx%4d (or %4dx%4d portrait mode)\n\n",
                    enc_props.width ,enc_props.height, MAX_LA_INPUT_WIDTH, MAX_LA_INPUT_HEIGHT, MAX_LA_INPUT_HEIGHT, MAX_LA_INPUT_WIDTH);
            return XMA_ERROR;
        }
    }

    switch(enc_props.rc_mode) {
      case 0 : av_log(avctx, AV_LOG_INFO, "Custom Rate Control Mode is Disabled\n");
               break;
      case 1 : if (ctx->lookahead_depth < MIN_LOOKAHEAD_DEPTH ||
                   ctx->lookahead_depth > MAX_LOOKAHEAD_DEPTH) {
                 av_log(avctx, AV_LOG_ERROR, "Error: Provided LA Depth %d is invalid !\n", ctx->lookahead_depth);
                 av_log(avctx, AV_LOG_ERROR, "To enable lookahead based Custom Rate Control: %d < lookahead_depth < %d\n",
                        MIN_LOOKAHEAD_DEPTH, MAX_LOOKAHEAD_DEPTH);
                 return AVERROR(EINVAL);
               } else {
                 enc_props.lookahead_depth = ctx->lookahead_depth;
               }
               av_log(avctx, AV_LOG_INFO, "#### Custom Rate Control Mode is Enabled with LA Depth = %d ####\n", enc_props.lookahead_depth);
               break;
      default: enc_props.rc_mode = 0;
               av_log(avctx, AV_LOG_INFO, "Rate Control Mode is default\n");
               break;
    }

	// Check for valid number of b-frames in different gop-modes
	switch(ctx->gop_mode){
		case 0: if (ctx->b_frames < 0 || ctx->b_frames > 4){
		           av_log(avctx, AV_LOG_ERROR, "Error: For gop-mode = default_gop(0), supported number of b-frames is between 0 and 4\n");
		           return AVERROR(EINVAL);
			   } else
			       break;
	    case 1: if (!(ctx->b_frames == 3 || ctx->b_frames == 5 || ctx->b_frames ==7 || ctx->b_frames == 15 )){
		           av_log(avctx, AV_LOG_ERROR, "Error: For gop-mode = pyramidal-gop(1), supported number of b-frames is 3, 5, 7 or 15 \n");
			       return AVERROR(EINVAL);
			   } else
			       break;
	}

	// Check if gop-mode=low_delay_p or low_delay_b, when GDR mode is enabled
	if ((ctx->gdr_mode == 1 || ctx->gdr_mode == 2) && (ctx->gop_mode == 0 || ctx->gop_mode == 1)){
		av_log(avctx, AV_LOG_ERROR, "Error: When gdr-mode = vertical (1) or horizontal(2) is enabled, gop-mode should be set to low_delay_p or low_delay_b \n");
		return AVERROR(EINVAL);
	}

	// Check if b-frames=0 when control_rate = low_latency(3)
	if (ctx->control_rate == 3 && ctx->b_frames != 0){
		av_log(avctx, AV_LOG_ERROR, "Error: For control_rate = low_latency(3), number of b-frames should be set to 0 \n");
		return AVERROR(EINVAL);
	}

    enc_props.framerate.numerator   = avctx->framerate.num;
    enc_props.framerate.denominator = avctx->framerate.den;
    ctx->frame.frame_props.format   = XMA_VCU_NV12_FMT_TYPE;
    ctx->frame.frame_props.width    = FFALIGN(avctx->width, VCU_STRIDE_ALIGN);
    ctx->frame.frame_props.height   = FFALIGN(avctx->height, VCU_HEIGHT_ALIGN);
    ctx->frame.frame_props.bits_per_pixel = 8;

    const char* enc_options = ctx->enc_options;
    if (avctx->pix_fmt == AV_PIX_FMT_XVBM)
        ctx->frame.data[0].buffer_type = XMA_DEVICE_BUFFER_TYPE;
    else if (avctx->pix_fmt == AV_PIX_FMT_NV12)
    	ctx->frame.data[0].buffer_type = XMA_HOST_BUFFER_TYPE;
    ctx->enc_params[enc_props.param_cnt].name   = "enc_options";
    ctx->enc_params[enc_props.param_cnt].type   = XMA_STRING;
    ctx->enc_params[enc_props.param_cnt].length = strlen(ctx->enc_options);
    ctx->enc_params[enc_props.param_cnt].value  = &(enc_options);
    enc_props.param_cnt++;

    ctx->enc_params[enc_props.param_cnt].name   = "latency_logging";
    ctx->enc_params[enc_props.param_cnt].type   = XMA_UINT32;
    ctx->enc_params[enc_props.param_cnt].length = sizeof(ctx->latency_logging);
    ctx->enc_params[enc_props.param_cnt].value  = &(ctx->latency_logging);
    enc_props.param_cnt++;

    if (!avctx->extradata_size) {
        /* will be freed by ffmpeg */
        avctx->extradata = av_mallocz(MAX_EXTRADATA_SIZE);
        if (avctx->extradata) {
          ctx->enc_params[enc_props.param_cnt].name   = "extradata";
          ctx->enc_params[enc_props.param_cnt].type   = XMA_STRING;
          ctx->enc_params[enc_props.param_cnt].length = MAX_EXTRADATA_SIZE;
          ctx->enc_params[enc_props.param_cnt].value  = &(avctx->extradata);
          enc_props.param_cnt++;

          /* let xma plugin assign the size of valid extradata */
          ctx->enc_params[enc_props.param_cnt].name   = "extradata_size";
          ctx->enc_params[enc_props.param_cnt].type   = XMA_UINT32;
          ctx->enc_params[enc_props.param_cnt].length = 0;
          ctx->enc_params[enc_props.param_cnt].value  = &(avctx->extradata_size);
          enc_props.param_cnt++;
        }
    }

    ctx->sent_flush = false;

    ctx->la = NULL;
    if(init_la(avctx)) {
        av_log(avctx, AV_LOG_ERROR, "Error: Unable to init_la Invalid params\n");
        return AVERROR(EINVAL);
    }
    ctx->la_in_frame = NULL;

    uint32_t enableHwInBuf = 0;
    if ((avctx->pix_fmt == AV_PIX_FMT_XVBM) ||
        (xlnx_la_in_bypass_mode(ctx->la) == 0)) {
        enableHwInBuf = 1;
    }
    ctx->enc_params[enc_props.param_cnt].name   = "enable_hw_in_buf";
    ctx->enc_params[enc_props.param_cnt].type   = XMA_UINT32;
    ctx->enc_params[enc_props.param_cnt].length = sizeof(enableHwInBuf);
    ctx->enc_params[enc_props.param_cnt].value  = &enableHwInBuf;
    enc_props.param_cnt++;
    /*----------------------------------------------------
      Allocate encoder resource from XRM reserved resource
      ----------------------------------------------------*/
    ctx->encode_res_inuse = 0;
    if(_allocate_xrm_enc_cu(ctx, &enc_props) < 0) {
            av_log(ctx, AV_LOG_ERROR, "xrm_allocation: resource allocation failed\n");
            return XMA_ERROR;
    }

    ctx->enc_session = xma_enc_session_create(&enc_props);
    if (!ctx->enc_session)
        return mpsoc_report_error(ctx, "ERROR: Unable to allocate MPSoC encoder session", AVERROR_EXTERNAL);

    /* TODO:temporary workaround for 4K HEVC MP4, not decodable by VCU decoder.
     * When size is 0, ffmpeg will not consider the already populated extradata */
    if (avctx->codec_id == AV_CODEC_ID_HEVC)
      avctx->extradata_size = 0;

    if (!avctx->extradata_size)
      av_log(avctx, AV_LOG_WARNING, "! output stream might not be playable by some media players !\n");

    ctx->pts_0 = AV_NOPTS_VALUE;
    ctx->pts_1 = AV_NOPTS_VALUE;
    ctx->is_first_outframe = 1;

    // TODO: find a proper way to find pts_queue size
    ctx->pts_queue = av_fifo_alloc(64 * sizeof(int64_t));
    if (!ctx->pts_queue)
        return mpsoc_report_error(ctx, "out of memory", AVERROR(ENOMEM));

    return 0;
}

static void vcu_enc_free_out_buffer(void *opaque, uint8_t *data)
{
    /*do nothing, if this CB is not provided, ffmpeg tries to free xrt buffer */
}

int vcu_alloc_ff_packet(mpsoc_vcu_enc_ctx *ctx, AVPacket *pkt)
{
    pkt->buf = av_buffer_create(ctx->xma_buffer.data.buffer, pkt->size, vcu_enc_free_out_buffer,  NULL,
                                AV_GET_BUFFER_FLAG_REF);
    if (!pkt->buf)
        return mpsoc_report_error(ctx, "out of memory", AVERROR(ENOMEM));

    pkt->data = ctx->xma_buffer.data.buffer;
    pkt->size = pkt->size;
    if(!pkt->size)
        return mpsoc_report_error(ctx, "invalid pkt size", AVERROR(ENOMEM));
    return 0;
}

static XmaFrame* xframe_from_avframe(const AVFrame *pic)
{
    XmaFrameProperties *frame_props = NULL;
    int32_t num_planes = 0;

    if (pic == NULL) {
        return NULL;
    }
    XmaFrame *frame = (XmaFrame*) calloc(1, sizeof(XmaFrame));
    if (frame == NULL) {
        return NULL;
    }

    memset(frame, 0, sizeof(XmaFrame));
    frame_props = &frame->frame_props;
    frame_props->format = XMA_VCU_NV12_FMT_TYPE;
    frame_props->width  = pic->width;
    frame_props->height = pic->height;
    frame_props->linesize[0] = pic->linesize[0];
    frame_props->linesize[1] = pic->linesize[1];
    frame_props->bits_per_pixel = 8;
    num_planes = av_pix_fmt_count_planes(pic->format);

    for (int32_t i = 0; i < num_planes; i++) {
        frame->data[i].refcount++;
        frame->data[i].buffer_type = XMA_HOST_BUFFER_TYPE;
        frame->data[i].buffer = pic->data[i];
        frame->data[i].is_clone = true;
        frame->data[i].xma_device_buf = NULL;
    }

    return frame;
}

static int mpsoc_vcu_encode_frame(AVCodecContext *avctx, AVPacket *pkt, const AVFrame *pic, int *got_packet)
{
    mpsoc_vcu_enc_ctx *ctx = avctx->priv_data;
    int recv_size, ret;
    XmaFrame *la_in_frame = NULL;
    XmaFrame *enc_in_frame = NULL;
    *got_packet = 0;
    recv_size = 0;

    if (pic && pic->data) {
        if (avctx->pix_fmt == AV_PIX_FMT_XVBM) {
            if (ctx->la_in_frame == NULL) {
            ctx->la_in_frame = (XmaFrame*) calloc(1, sizeof(XmaFrame));
                if (ctx->la_in_frame == NULL)
                    return mpsoc_report_error(ctx, "Error: mpsoc_vcu_encode_frame OOM failed!!", AVERROR(EIO));
            }
            la_in_frame = ctx->la_in_frame;
            memcpy (la_in_frame, pic->data[0], sizeof (XmaFrame));
	    if (!la_in_frame->data[0].buffer)
                return mpsoc_report_error(ctx, "Error: invalid input buffer to encode", AVERROR(EIO));
            xvbm_buffer_refcnt_inc(la_in_frame->data[0].buffer);
            la_in_frame->pts = pic->pts;
            mpsoc_vcu_encode_queue_pts(ctx->pts_queue, la_in_frame->pts);
        } else {
            if (ctx->la_in_frame == NULL) {
                ctx->la_in_frame = xframe_from_avframe(pic);
                if (ctx->la_in_frame == NULL)
                    return mpsoc_report_error(ctx, "Error: mpsoc_vcu_encode_frame OOM failed!!", AVERROR(EIO));
            } else {
                for (int plane_id = 0; plane_id < av_pix_fmt_count_planes (pic->format); plane_id++) {
                    ctx->la_in_frame->data[plane_id].buffer = pic->data[plane_id];
                    ctx->la_in_frame->frame_props.linesize[plane_id] = pic->linesize[plane_id]; // need this as at sometimes changes from one frame to another
                }
            }

            ctx->la_in_frame->pts = pic->pts;
            la_in_frame = ctx->la_in_frame;
            mpsoc_vcu_encode_queue_pts(ctx->pts_queue, la_in_frame->pts);
        }
        if (ctx->pts_0 == AV_NOPTS_VALUE)
            ctx->pts_0 = la_in_frame->pts;
        else if (ctx->pts_1 == AV_NOPTS_VALUE)
            ctx->pts_1 = la_in_frame->pts;
    }

    if (la_in_frame && la_in_frame->data[0].buffer == NULL) {
        la_in_frame->is_last_frame = 1;
    }
    ret = xlnx_la_send_recv_frame(ctx->la, la_in_frame, &enc_in_frame);
    if (ret <= XMA_ERROR) {
        if (avctx->pix_fmt == AV_PIX_FMT_XVBM) {
            XvbmBufferHandle handle = (XvbmBufferHandle)(la_in_frame->data[0].buffer);
            if (handle) {
                xvbm_buffer_pool_entry_free(handle);
            }
    	}
        return mpsoc_report_error(ctx, "Error: mpsoc_vcu_encode_frame xlnx_la_send_recv_frame failed!!", AVERROR(EIO));
    } else if ((ret == XMA_SEND_MORE_DATA) && (la_in_frame && la_in_frame->data[0].buffer != NULL)) {
        goto end;
    }
    if (enc_in_frame && enc_in_frame->data[0].buffer) {
        ret = xma_enc_session_send_frame(ctx->enc_session, enc_in_frame);
        if (enc_in_frame) {
            if (ret == XMA_ERROR) {
                XvbmBufferHandle xvbm_handle = (XvbmBufferHandle)(enc_in_frame->data[0].buffer);
                if (xvbm_handle) {
                    xvbm_buffer_pool_entry_free(xvbm_handle);
                }
            }
            xlnx_la_release_frame(ctx->la, enc_in_frame);
            enc_in_frame = NULL;
        }
        if(ret == XMA_SEND_MORE_DATA) {
            goto end;
        }
        if (ret == XMA_SUCCESS) {
            while (1) {
                ret = xma_enc_session_recv_data(ctx->enc_session, &(ctx->xma_buffer), &recv_size);
                if (ret == XMA_SUCCESS) {
                    if (recv_size == 0) {
                        *got_packet = 0;
                        goto end;
                    }
                    /* valid data received */
                    *got_packet = 1;
                    pkt->size = recv_size;
                    if(ret = vcu_alloc_ff_packet(ctx, pkt) < 0)
                        return ret;
                    pkt->pts = ctx->xma_buffer.pts;
                    mpsoc_vcu_encode_prepare_out_timestamp (avctx, pkt);
                    pkt->flags |= ((avctx->codec_id == AV_CODEC_ID_H264) ? mpsoc_encode_is_h264_idr (pkt) : mpsoc_encode_is_hevc_idr (pkt)) ? AV_PKT_FLAG_KEY : 0;
                    break;
                } else if (ret == XMA_TRY_AGAIN) {
                    if (pic && pic->data) {
                        /* vcu not ready with an output buffer */
                        *got_packet = 0;
                        goto end;
                    } else {
                        ret = xlnx_la_send_recv_frame(ctx->la, NULL, &enc_in_frame);
                        if (ret <= XMA_ERROR) {
                            if (avctx->pix_fmt == AV_PIX_FMT_XVBM) {
                                XvbmBufferHandle handle = (XvbmBufferHandle)(la_in_frame->data[0].buffer);
                                if (handle) {
                                    xvbm_buffer_pool_entry_free(handle);
                                }
                            }
                            return mpsoc_report_error(ctx, "Error: mpsoc_vcu_encode_frame xlnx_la_send_recv_frame failed!!", AVERROR(EIO));
                        }
                        if (enc_in_frame && enc_in_frame->data[0].buffer) {
                            ret = xma_enc_session_send_frame(ctx->enc_session, enc_in_frame);
                            if (enc_in_frame) {
                                if (ret == XMA_ERROR) {
                                    XvbmBufferHandle xvbm_handle = (XvbmBufferHandle)(enc_in_frame->data[0].buffer);
                                    if (xvbm_handle) {
                                        xvbm_buffer_pool_entry_free(xvbm_handle);
                                    }
                                }
                                xlnx_la_release_frame(ctx->la, enc_in_frame);
                                enc_in_frame = NULL;
                            }
                            if(ret == XMA_SEND_MORE_DATA) {
                                goto start_flush;
                            }
                        } else {
                            goto start_flush;
                        }
                    }
                } else {
                    /* vcu not ready with an output buffer */
                    *got_packet = 0;
                    if (ret == XMA_EOS)
                        return AVERROR_EOF;
		    else
                        goto end;
                }
            }
        } else {
            /* send raw data failed */
            *got_packet = 0;
            return mpsoc_report_error(ctx, "Error : mpsoc_vcu_encode_frame send raw data failed", AVERROR(EIO));
        }
    } else { /* end of input data */
start_flush:
        ctx->frame.is_last_frame = 1;
        if (ctx->sent_flush == false) {
            ctx->sent_flush = true;
            ctx->frame.pts = -1;
            ret = xma_enc_session_send_frame(ctx->enc_session, &(ctx->frame));
            if ((ret == XMA_SUCCESS) || (ret == XMA_FLUSH_AGAIN)) {
                if (ret == XMA_FLUSH_AGAIN)
                    ctx->sent_flush = false; //force flush to clear pipeline in next iteration
                ret = xma_enc_session_recv_data(ctx->enc_session, &(ctx->xma_buffer), &recv_size);
                if (ret == XMA_SUCCESS) {
                    if (recv_size == 0) {
                        *got_packet = 0;
                        goto end;
                    }
                    *got_packet = 1;
                    pkt->size = recv_size;
                    if(ret = vcu_alloc_ff_packet(ctx, pkt) < 0)
                        return ret;
                    pkt->pts = ctx->xma_buffer.pts;
                    mpsoc_vcu_encode_prepare_out_timestamp (avctx, pkt);
                    pkt->flags |= ((avctx->codec_id == AV_CODEC_ID_H264) ? mpsoc_encode_is_h264_idr (pkt) : mpsoc_encode_is_hevc_idr (pkt)) ? AV_PKT_FLAG_KEY : 0;
                } else {
	            *got_packet = 0;
                }
            }
        } else {
            ret = xma_enc_session_recv_data(ctx->enc_session, &(ctx->xma_buffer), &recv_size);
            if (ret == XMA_SUCCESS) {
                if (recv_size == 0) {
                    *got_packet = 0;
                    goto end;
                }
                pkt->size = recv_size;
                if(ret = vcu_alloc_ff_packet(ctx, pkt) < 0)
                    return ret;
                pkt->pts = ctx->xma_buffer.pts;
                mpsoc_vcu_encode_prepare_out_timestamp (avctx, pkt);
                pkt->flags |= ((avctx->codec_id == AV_CODEC_ID_H264) ? mpsoc_encode_is_h264_idr (pkt) : mpsoc_encode_is_hevc_idr (pkt)) ? AV_PKT_FLAG_KEY : 0;
                *got_packet = 1;
            } else {
	        *got_packet = 0;
            }
        }
    }
end:
    return 0;
}

static const AVCodecDefault mpsoc_defaults[] = {
    { "b", "5M" },
    { "g", "120" },
    { NULL },
};

static const AVClass mpsoc_h264_class = {
    .class_name = "MPSOC VCU H264 encoder",
    .item_name = av_default_item_name,
    .option = h264Options,
    .version = LIBAVUTIL_VERSION_INT,
};

AVCodec ff_h264_vcu_mpsoc_encoder = {
    .name = "mpsoc_vcu_h264",
    .long_name = NULL_IF_CONFIG_SMALL("MPSOC H.264 Encoder"),
    .type = AVMEDIA_TYPE_VIDEO,
    .id = AV_CODEC_ID_H264,
    .init = mpsoc_vcu_encode_init,
    .encode2 = mpsoc_vcu_encode_frame,
    .close = mpsoc_vcu_encode_close,
    .priv_data_size = sizeof(mpsoc_vcu_enc_ctx),
    .priv_class = &mpsoc_h264_class,
    .defaults = mpsoc_defaults,
    .pix_fmts = (const enum AVPixelFormat[]) { AV_PIX_FMT_XVBM,
                                               AV_PIX_FMT_NV12,
                                               AV_PIX_FMT_NONE },
    .capabilities = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_AUTO_THREADS,
};

static const AVClass mpsoc_hevc_vcu_class = {
    .class_name = "MPSOC VCU HEVC encoder",
    .item_name = av_default_item_name,
    .option = hevcOptions,
    .version = LIBAVUTIL_VERSION_INT,
};

AVCodec ff_hevc_vcu_mpsoc_encoder = {
    .name = "mpsoc_vcu_hevc",
    .long_name = NULL_IF_CONFIG_SMALL("MPSOC VCU HEVC Encoder"),
    .type  = AVMEDIA_TYPE_VIDEO,
    .id = AV_CODEC_ID_HEVC,
    .init = mpsoc_vcu_encode_init,
    .encode2 = mpsoc_vcu_encode_frame,
    .close = mpsoc_vcu_encode_close,
    .priv_data_size = sizeof(mpsoc_vcu_enc_ctx),
    .priv_class = &mpsoc_hevc_vcu_class,
    .defaults = mpsoc_defaults,
    .pix_fmts = (const enum AVPixelFormat[]) { AV_PIX_FMT_XVBM,
                                               AV_PIX_FMT_NV12,
                                               AV_PIX_FMT_NONE },
    .capabilities = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_AUTO_THREADS | AV_CODEC_CAP_AVOID_PROBING,
};
