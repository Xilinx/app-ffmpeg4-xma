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

#ifndef XLNX_ENCODER_H
#define XLNX_ENCODER_H

#include "xlnx_lookahead.h"
#include <inttypes.h>
#include <xma.h>

#define SCLEVEL1 2

#define MAX_ENC_PARAMS      (6)
/* MAX_EXTRADATA_SIZE should be consistent with AL_ENC_MAX_CONFIG_HEADER_SIZE on device */
#define MAX_EXTRADATA_SIZE   (2 * 1024)
#define MAX_ENC_WIDTH        3840
#define MAX_ENC_HEIGHT       2160
#define MAX_ENC_PIXELS       (MAX_ENC_WIDTH * MAX_ENC_HEIGHT)

#define OFFSET(x) offsetof(mpsoc_vcu_enc_ctx, x)
#define VE AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_ENCODING_PARAM

#define VCU_STRIDE_ALIGN    32
#define VCU_HEIGHT_ALIGN    32

#define XRM_PRECISION_1000000_BIT_MASK(load) ((load << 8))

#define DYN_PARAMS_LIB_NAME  "/opt/xilinx/ffmpeg/lib/libu30_enc_dyn_params.so"
#define XLNX_ENC_INIT_DYN_PARAMS_OBJ  "xlnx_enc_init_dyn_params_obj"

#define DEFAULT_NUM_B_FRAMES 2
#define UNSET_NUM_B_FRAMES   -1
#define DYNAMIC_GOP_MIN_LOOKAHEAD_DEPTH 5

typedef void *DynparamsHandle;

/* Functions pointers for loading functions from dynamic params library */
typedef DynparamsHandle (*fp_xlnx_enc_get_dyn_params)(char*, uint32_t*);
typedef uint32_t(*fp_xlnx_enc_get_dyn_param_frame_num) (DynparamsHandle, uint32_t);
typedef uint32_t(*fp_xlnx_enc_get_runtime_b_frames) (DynparamsHandle, uint32_t);
typedef void(*fp_xlnx_enc_reset_runtime_aq_params) (DynparamsHandle, uint32_t);
typedef int32_t(*fp_xlnx_enc_add_dyn_params) (DynparamsHandle, XmaFrame*, uint32_t);
typedef void (*fp_xlnx_enc_deinit_dyn_params) (DynparamsHandle dynamic_params_handle);

typedef struct XlnxDynParamsObj
{
    fp_xlnx_enc_get_dyn_params            xlnx_enc_get_dyn_params;
    fp_xlnx_enc_get_dyn_param_frame_num   xlnx_enc_get_dyn_param_frame_num;
    fp_xlnx_enc_get_runtime_b_frames      xlnx_enc_get_runtime_b_frames;
    fp_xlnx_enc_reset_runtime_aq_params   xlnx_enc_reset_runtime_aq_params;
    fp_xlnx_enc_add_dyn_params            xlnx_enc_add_dyn_params;
    fp_xlnx_enc_deinit_dyn_params         xlnx_enc_deinit_dyn_params;
} XlnxDynParamsObj;

typedef void(*InitDynParams) (XlnxDynParamsObj*);

// Dynamic params structure
typedef struct EncDynParams {
    char dynamic_params_file[256];
    bool dynamic_params_check;
    DynparamsHandle dynamic_param_handle;
    uint32_t dynamic_params_count;
    uint32_t dynamic_params_index;
    void* dyn_params_lib;
    XlnxDynParamsObj dyn_params_obj;
    InitDynParams xlnx_enc_init_dyn_params_obj;
} EncDynParams;

enum mpsoc_vcu_enc_supported_bitdepth {
	MPSOC_VCU_BITDEPTH_8BIT = 8,
	MPSOC_VCU_BITDEPTH_10BIT = 10,
};

#define MIN_LOOKAHEAD_DEPTH	(1)
#define MAX_LOOKAHEAD_DEPTH	(30)

typedef struct {
    AVFrame *pic;
    XmaFrame *xframe;
} mpsoc_enc_req;

typedef struct mpsoc_vcu_enc_ctx {
    const AVClass     *class;
    XmaEncoderSession *enc_session;
    XmaParameter       enc_params[MAX_ENC_PARAMS];
    xrmContext        *xrm_ctx;
    xrmCuListResourceV2  encode_cu_list_res;
    bool               encode_res_inuse;
    int ideal_latency;
    XmaFrame frame;
    XmaDataBuffer xma_buffer;
    bool sent_flush;
    int  lxlnx_hwdev;
    int bits_per_sample;
    int control_rate;
    int64_t max_bitrate;
    int slice_qp;
    int min_qp;
    int max_qp;
    int ip_delta;
    int pb_delta;
    double cpb_size;
    double initial_delay;
    int gop_mode;
    int gdr_mode;
    int b_frames;
    int dynamic_gop;
    int periodicity_idr;
    int profile;
    int level;
    int tier;
    int num_slices;
    int qp_mode;
    int filler_data;
    int aspect_ratio;
    int dependent_slice;
    int slice_size;
    int scaling_list;
    int entropy_mode;
    int loop_filter;
    int constrained_intra_pred;
    int prefetch_buffer;
    int cores;
    int latency_logging;
    int disable_pipeline;
    int avc_lowlat;
    char enc_options[2048];
    AVFifoBuffer *pts_queue;
    int64_t pts_0;
    int64_t pts_1;
    int is_first_outframe;
    int loop_filter_beta_offset;
    int loop_filter_tc_offset;
    int32_t out_packet_size;
    uint32_t enc_frame_cnt;
    //LA
    xlnx_lookahead_t la;
    int32_t lookahead_depth;
    int32_t spatial_aq;
    int32_t temporal_aq;
    int32_t rate_control_mode;
    int32_t spatial_aq_gain;
    XmaFrame* la_in_frame;
    //Expert options
    char *expert_options;
    int32_t tune_metrics;
    int32_t lookahead_rc_off;
    EncDynParams enc_dyn_params;
} mpsoc_vcu_enc_ctx;

static const AVOption h264Options[] = {
    { "lxlnx_hwdev", "set local device ID for encoder if it needs to be different from global xlnx_hwdev", OFFSET(lxlnx_hwdev), AV_OPT_TYPE_INT, {.i64 = -1}, -1, INT_MAX, VE, "lxlnx_hwdev"},
    { "control-rate", "Rate Control Mode", OFFSET(control_rate), AV_OPT_TYPE_INT, { .i64 = 1}, 0,  3, VE, "control-rate"},
    { "max-bitrate", "Maximum Bit Rate", OFFSET(max_bitrate), AV_OPT_TYPE_INT64, { .i64 = 5000000}, 0,  35000000000, VE, "max-bitrate"},
    { "slice-qp", "Slice QP", OFFSET(slice_qp), AV_OPT_TYPE_INT, { .i64 = -1}, -1,  51, VE, "slice-qp"},
    { "min-qp", "Minimum QP value allowed for the rate control", OFFSET(min_qp), AV_OPT_TYPE_INT, { .i64 = 0}, 0, 51, VE, "min-qp"},
    { "max-qp", "Maximum QP value allowed for the rate control", OFFSET(max_qp), AV_OPT_TYPE_INT, { .i64 = 51}, 0, 51, VE, "max-qp"},
    { "bf", "Number of B-frames Default 2", OFFSET(b_frames), AV_OPT_TYPE_INT, { .i64 = UNSET_NUM_B_FRAMES}, UNSET_NUM_B_FRAMES, 4294967295, VE, "b-frames"},
    { "dynamic-gop", "Automatically change B-frame structure based on motion vectors. Requires Lookahead_depth of at least 5.", OFFSET(dynamic_gop), AV_OPT_TYPE_INT, { .i64 = 0}, 0, 1, VE, "dynamic-gop"},
    { "periodicity-idr", "IDR Picture Frequency", OFFSET(periodicity_idr), AV_OPT_TYPE_INT, { .i64 = -1}, -1, 4294967295, VE, "periodicity-idr"},
    { "profile", "Set the encoding profile", OFFSET(profile), AV_OPT_TYPE_INT, { .i64 = FF_PROFILE_H264_HIGH }, FF_PROFILE_H264_BASELINE, FF_PROFILE_H264_HIGH_10_INTRA, VE, "profile" },
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
    { "disable-pipeline", "Disable pipelining for encoder. Serializes encoding (does not affect lookahead)", OFFSET(disable_pipeline), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 1, VE, "disable-pipeline" },
	{ "avc-lowlat", "Enable AVC low latency flag for H264 to run on multiple cores incase of pipeline disabled", OFFSET(avc_lowlat), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 1, VE, "avc-lowlat" },
    { "expert-options", "Expert options for MPSoC H264 Encoder", OFFSET(expert_options), AV_OPT_TYPE_STRING, {.str = NULL}, 0, 1024, VE, "expert_options"},
    { "tune-metrics", "Tunes MPSoC H.264 Encoder's video quality for objective metrics", OFFSET(tune_metrics), AV_OPT_TYPE_INT, {.i64 = 0}, 0, 1, VE, "tune-metrics"},

    { "const-qp", "Constant QP (0)", 0, AV_OPT_TYPE_CONST, { .i64 = 0}, 0, 0, VE, "control-rate"},
    { "cbr", "Constant Bitrate (1)", 0, AV_OPT_TYPE_CONST, { .i64 = 1}, 0, 0, VE, "control-rate"},
    { "vbr", "Variable Bitrate (2)", 0, AV_OPT_TYPE_CONST, { .i64 = 2}, 0, 0, VE, "control-rate"},
    { "low-latency", "Low Latency (3)", 0, AV_OPT_TYPE_CONST, { .i64 = 3}, 0, 0, VE, "control-rate"},
    { "auto", "Auto (-1)", 0, AV_OPT_TYPE_CONST, { .i64 = -1}, 0, 0, VE, "slice-qp"},
    { "unset", "Unset (-1)", 0, AV_OPT_TYPE_CONST, { .i64 = UNSET_NUM_B_FRAMES}, 0, 0, VE, "b-frames"},
    { "baseline", "Baseline profile (66)", 0, AV_OPT_TYPE_CONST, { .i64 = FF_PROFILE_H264_BASELINE}, 0, 0, VE, "profile"},
    { "main", "Main profile (77)", 0, AV_OPT_TYPE_CONST, { .i64 = FF_PROFILE_H264_MAIN}, 0, 0, VE, "profile"},
    { "high", "High profile (100)", 0, AV_OPT_TYPE_CONST, { .i64 = FF_PROFILE_H264_HIGH}, 0, 0, VE, "profile"},
    { "high-10", "High 10 profile (110)", 0, AV_OPT_TYPE_CONST, {.i64 = FF_PROFILE_H264_HIGH_10}, 0, 0, VE, "profile"},
    { "high-10-intra", "High 10 Intra profile (110 with constraint set 3, 2158)", 0, AV_OPT_TYPE_CONST, { .i64 = FF_PROFILE_H264_HIGH_10_INTRA}, 0, 0, VE, "profile"},
    { "1", "1 level (10)", 0, AV_OPT_TYPE_CONST, { .i64 = 10}, 0, 0, VE, "level"},
    { "1.1", "1.1 level (11)", 0, AV_OPT_TYPE_CONST, { .i64 = 11}, 0, 0, VE, "level"},
    { "1.2", "1.2 level (12)", 0, AV_OPT_TYPE_CONST, { .i64 = 12}, 0, 0, VE, "level"},
    { "1.3", "1.3 level (13)", 0, AV_OPT_TYPE_CONST, { .i64 = 13}, 0, 0, VE, "level"},
    { "2", "2 level (20)", 0, AV_OPT_TYPE_CONST, { .i64 = 20}, 0, 0, VE, "level"},
    { "2.1", "2.1 level (21)", 0, AV_OPT_TYPE_CONST, { .i64 = 21}, 0, 0, VE, "level"},
    { "2.2", "2.2 level (22)", 0, AV_OPT_TYPE_CONST, { .i64 = 22}, 0, 0, VE, "level"},
    { "3", "3 level (30)", 0, AV_OPT_TYPE_CONST, { .i64 = 30}, 0, 0, VE, "level"},
    { "3.1", "3.1 level (31)", 0, AV_OPT_TYPE_CONST, { .i64 = 31}, 0, 0, VE, "level"},
    { "3.2", "3.2 level (32)", 0, AV_OPT_TYPE_CONST, { .i64 = 32}, 0, 0, VE, "level"},
    { "4", "4 level (40)", 0, AV_OPT_TYPE_CONST, { .i64 = 40}, 0, 0, VE, "level"},
    { "4.1", "4.1 level (41)", 0, AV_OPT_TYPE_CONST, { .i64 = 41}, 0, 0, VE, "level"},
    { "4.2", "4.2 level (42)", 0, AV_OPT_TYPE_CONST, { .i64 = 42}, 0, 0, VE, "level"},
    { "5", "5 level (50)", 0, AV_OPT_TYPE_CONST, { .i64 = 50}, 0, 0, VE, "level"},
    { "5.1", "5.1 level (51)", 0, AV_OPT_TYPE_CONST, { .i64 = 51}, 0, 0, VE, "level"},
    { "5.2", "5.2 level (52)", 0, AV_OPT_TYPE_CONST, { .i64 = 52}, 0, 0, VE, "level"},
    { "uniform", "Use the same QP for all coding units of the frame (0)", 0, AV_OPT_TYPE_CONST, { .i64 = 0}, 0, 0, VE, "qp-mode"},
    { "auto", "Let the VCU encoder change the QP for each coding unit according to its content (1)", 0, AV_OPT_TYPE_CONST, { .i64 = 1}, 0, 0, VE, "qp-mode"},
    { "relative-load", "Use the information gathered in the lookahead to calculate the best QP (2)", 0, AV_OPT_TYPE_CONST, { .i64 = 2}, 0, 0, VE, "qp-mode"},
    { "auto", "4:3 for SD video, 16:9 for HD video, unspecified for unknown format (0)", 0, AV_OPT_TYPE_CONST, { .i64 = 0}, 0, 0, VE, "aspect-ratio"},
    { "4:3", "4:3 aspect ratio (1)", 0, AV_OPT_TYPE_CONST, { .i64 = 1}, 0, 0, VE, "aspect-ratio"},
    { "16:9", "16:9 aspect ratio (2)", 0, AV_OPT_TYPE_CONST, { .i64 = 2}, 0, 0, VE, "aspect-ratio"},
    { "none", "Aspect ratio information is not present in the stream (3)", 0, AV_OPT_TYPE_CONST, { .i64 = 3}, 0, 0, VE, "aspect-ratio"},
    { "flat", "Flat scaling list mode (0)", 0, AV_OPT_TYPE_CONST, { .i64 = 0}, 0, 0, VE, "scaling-list"},
    { "default", "Default scaling list mode (1)", 0, AV_OPT_TYPE_CONST, { .i64 = 1}, 0, 0, VE, "scaling-list"},
    { "auto", "Automatic (0)", 0, AV_OPT_TYPE_CONST, { .i64 = 0}, 0, 0, VE, "cores"},
    { "disable", "Disable Temporal AQ (0)", 0, AV_OPT_TYPE_CONST, { .i64 = 0}, 0, 0, VE, "temporal-aq-mode"},
    { "enable", "Enable Temporal AQ (1)", 0, AV_OPT_TYPE_CONST, { .i64 = 1}, 0, 0, VE, "temporal-aq-mode"},
    { "disable", "Disable Spatial AQ (0)", 0, AV_OPT_TYPE_CONST, { .i64 = 0}, 0, 0, VE, "spatial-aq-mode"},
    { "enable", "Enable Spatial AQ (1)", 0, AV_OPT_TYPE_CONST, { .i64 = 1}, 0, 0, VE, "spatial-aq-mode"},
    { "disable", "Enable encoder pipelining (0)", 0, AV_OPT_TYPE_CONST, { .i64 = 0}, 0, 0, VE, "disable-pipeline"},
    { "enable", "Disable encoder pipelining (1)", 0, AV_OPT_TYPE_CONST, { .i64 = 1}, 0, 0, VE, "disable-pipeline"},
    { "disable", "Disable AVC low latency (0)", 0, AV_OPT_TYPE_CONST, { .i64 = 0}, 0, 0, VE, "avc-lowlat"},
    { "enable", "Enable AVC low latency (1)", 0, AV_OPT_TYPE_CONST, { .i64 = 1}, 0, 0, VE, "avc-lowlat"},
    { "disable", "Disable dynamic gop (0)", 0, AV_OPT_TYPE_CONST, { .i64 = 0}, 0, 0, VE, "dynamic-gop"},
	{ "enable", "Enable dynamic gop (1)", 0, AV_OPT_TYPE_CONST, { .i64 = 1}, 0, 0, VE, "dynamic-gop"},
    { "disable", "Disable tune metrics (0)", 0, AV_OPT_TYPE_CONST, { .i64 = 0}, 0, 0, VE, "tune-metrics"},
    { "enable", "Enable tune metrics (1)", 0, AV_OPT_TYPE_CONST, { .i64 = 1}, 0, 0, VE, "tune-metrics"},
    {NULL},
};

static const AVOption hevcOptions[] = {
    { "lxlnx_hwdev", "set local device ID for encoder if it needs to be different from global xlnx_hwdev", OFFSET(lxlnx_hwdev), AV_OPT_TYPE_INT, {.i64 = -1}, -1, INT_MAX, VE, "lxlnx_hwdev"},
    { "control-rate", "Rate Control Mode", OFFSET(control_rate), AV_OPT_TYPE_INT, { .i64 = 1}, 0,  3, VE, "control-rate"},
    { "max-bitrate", "Maximum Bit Rate", OFFSET(max_bitrate), AV_OPT_TYPE_INT64, { .i64 = 5000000}, 0,  35000000000, VE, "max-bitrate"},
    { "slice-qp", "Slice QP", OFFSET(slice_qp), AV_OPT_TYPE_INT, { .i64 = -1}, -1,  51, VE, "slice-qp"},
    { "min-qp", "Minimum QP value allowed for the rate control", OFFSET(min_qp), AV_OPT_TYPE_INT, { .i64 = 0}, 0, 51, VE, "min-qp"},
    { "max-qp", "Maximum QP value allowed for the rate control", OFFSET(max_qp), AV_OPT_TYPE_INT, { .i64 = 51}, 0, 51, VE, "max-qp"},
    { "bf", "Number of B-frames Default 2", OFFSET(b_frames), AV_OPT_TYPE_INT, { .i64 = UNSET_NUM_B_FRAMES}, UNSET_NUM_B_FRAMES, 4294967295, VE, "b-frames"},
    { "dynamic-gop", "Automatically change B-frame structure based on motion vectors. Requires Lookahead_depth of at least 5.", OFFSET(dynamic_gop), AV_OPT_TYPE_INT, { .i64 = 0}, 0, 1, VE, "dynamic-gop"},
    { "periodicity-idr", "IDR Picture Frequency", OFFSET(periodicity_idr), AV_OPT_TYPE_INT, { .i64 = -1}, -1, 4294967295, VE, "periodicity-idr"},
    { "profile", "Set the encoding profile", OFFSET(profile), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 3, VE, "profile" },
    { "level", "Set the encoding level restriction", OFFSET(level), AV_OPT_TYPE_INT, { .i64 = 10 }, 10, 52, VE, "level" },
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
    { "disable-pipeline", "Disable pipelining for encoder. Serializes encoding (does not affect lookahead)", OFFSET(disable_pipeline), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 1, VE, "disable-pipeline" },
    { "expert-options", "Expert options for MPSoC HEVC Encoder", OFFSET(expert_options), AV_OPT_TYPE_STRING, {.str = NULL}, 0, 1024, VE, "expert_options"},
	{ "tune-metrics", "Tunes MPSoC HEVC Encoder's video quality for objective metrics", OFFSET(tune_metrics), AV_OPT_TYPE_INT, {.i64 = 0}, 0, 1, VE, "tune-metrics"},

    { "const-qp", "Constant QP (0)", 0, AV_OPT_TYPE_CONST, { .i64 = 0}, 0, 0, VE, "control-rate"},
    { "cbr", "Constant Bitrate (1)", 0, AV_OPT_TYPE_CONST, { .i64 = 1}, 0, 0, VE, "control-rate"},
    { "vbr", "Variable Bitrate (2)", 0, AV_OPT_TYPE_CONST, { .i64 = 2}, 0, 0, VE, "control-rate"},
    { "low-latency", "Low Latency (3)", 0, AV_OPT_TYPE_CONST, { .i64 = 3}, 0, 0, VE, "control-rate"},
    { "auto", "Auto (-1)", 0, AV_OPT_TYPE_CONST, { .i64 = -1}, 0, 0, VE, "slice-qp"},
    { "unset", "Unset (-1)", 0, AV_OPT_TYPE_CONST, { .i64 = UNSET_NUM_B_FRAMES}, 0, 0, VE, "b-frames"},
    { "main", "Main profile (0)", 0, AV_OPT_TYPE_CONST, { .i64 = 0}, 0, 0, VE, "profile"},
    { "main-intra", "Main Intra profile (1)", 0, AV_OPT_TYPE_CONST, { .i64 = 1}, 0, 0, VE, "profile"},
    { "main-10", "Main 10 profile (2)", 0, AV_OPT_TYPE_CONST, { .i64 = 2}, 0, 0, VE, "profile"},
    { "main-10-intra", "Main 10 Intra profile (3)", 0, AV_OPT_TYPE_CONST, { .i64 = 3}, 0, 0, VE, "profile"},
    { "1", "1 level (10)", 0, AV_OPT_TYPE_CONST, { .i64 = 10}, 0, 0, VE, "level"},
    { "2", "2 level (20)", 0, AV_OPT_TYPE_CONST, { .i64 = 20}, 0, 0, VE, "level"},
    { "2.1", "2.1 level (21)", 0, AV_OPT_TYPE_CONST, { .i64 = 21}, 0, 0, VE, "level"},
    { "3", "3 level (30)", 0, AV_OPT_TYPE_CONST, { .i64 = 30}, 0, 0, VE, "level"},
    { "3.1", "3.1 level (31)", 0, AV_OPT_TYPE_CONST, { .i64 = 31}, 0, 0, VE, "level"},
    { "4", "4 level (40)", 0, AV_OPT_TYPE_CONST, { .i64 = 40}, 0, 0, VE, "level"},
    { "4.1", "4.1 level (41)", 0, AV_OPT_TYPE_CONST, { .i64 = 41}, 0, 0, VE, "level"},
    { "5", "5 level (50)", 0, AV_OPT_TYPE_CONST, { .i64 = 50}, 0, 0, VE, "level"},
    { "5.1", "5.1 level (51)", 0, AV_OPT_TYPE_CONST, { .i64 = 51}, 0, 0, VE, "level"},
    { "5.2", "5.2 level (52)", 0, AV_OPT_TYPE_CONST, { .i64 = 52}, 0, 0, VE, "level"},
    { "main", "Main tier (0)", 0, AV_OPT_TYPE_CONST, { .i64 = 0}, 0, 0, VE, "tier"},
    { "high", "High tier (1)", 0, AV_OPT_TYPE_CONST, { .i64 = 1}, 0, 0, VE, "tier"},
    { "uniform", "Use the same QP for all coding units of the frame (0)", 0, AV_OPT_TYPE_CONST, { .i64 = 0}, 0, 0, VE, "qp-mode"},
    { "auto", "Let the VCU encoder change the QP for each coding unit according to its content (1)", 0, AV_OPT_TYPE_CONST, { .i64 = 1}, 0, 0, VE, "qp-mode"},
    { "relative-load", "Use the information gathered in the lookahead to calculate the best QP (2)", 0, AV_OPT_TYPE_CONST, { .i64 = 2}, 0, 0, VE, "qp-mode"},
    { "auto", "4:3 for SD video, 16:9 for HD video, unspecified for unknown format (0)", 0, AV_OPT_TYPE_CONST, { .i64 = 0}, 0, 0, VE, "aspect-ratio"},
    { "4:3", "4:3 aspect ratio (1)", 0, AV_OPT_TYPE_CONST, { .i64 = 1}, 0, 0, VE, "aspect-ratio"},
    { "16:9", "16:9 aspect ratio (2)", 0, AV_OPT_TYPE_CONST, { .i64 = 2}, 0, 0, VE, "aspect-ratio"},
    { "none", "Aspect ratio information is not present in the stream (3)", 0, AV_OPT_TYPE_CONST, { .i64 = 3}, 0, 0, VE, "aspect-ratio"},
    { "flat", "Flat scaling list mode (0)", 0, AV_OPT_TYPE_CONST, { .i64 = 0}, 0, 0, VE, "scaling-list"},
    { "default", "Default scaling list mode (1)", 0, AV_OPT_TYPE_CONST, { .i64 = 1}, 0, 0, VE, "scaling-list"},
    { "auto", "Automatic (0)", 0, AV_OPT_TYPE_CONST, { .i64 = 0}, 0, 0, VE, "cores"},
    { "disable", "Disable Temporal AQ (0)", 0, AV_OPT_TYPE_CONST, { .i64 = 0}, 0, 0, VE, "temporal-aq-mode"},
    { "enable", "Enable Temporal AQ (1)", 0, AV_OPT_TYPE_CONST, { .i64 = 1}, 0, 0, VE, "temporal-aq-mode"},
    { "disable", "Disable Spatial AQ (0)", 0, AV_OPT_TYPE_CONST, { .i64 = 0}, 0, 0, VE, "spatial-aq-mode"},
    { "enable", "Enable Spatial AQ (1)", 0, AV_OPT_TYPE_CONST, { .i64 = 1}, 0, 0, VE, "spatial-aq-mode"},
    { "disable", "Disable encoder pipelining (0)", 0, AV_OPT_TYPE_CONST, { .i64 = 0}, 0, 0, VE, "disable-pipeline"},
    { "enable", "Enable encoder pipelining (1)", 0, AV_OPT_TYPE_CONST, { .i64 = 1}, 0, 0, VE, "disable-pipeline"},
    { "disable", "Disable dynamic gop (0)", 0, AV_OPT_TYPE_CONST, { .i64 = 0}, 0, 0, VE, "dynamic-gop"},
	{ "enable", "Enable dynamic gop (1)", 0, AV_OPT_TYPE_CONST, { .i64 = 1}, 0, 0, VE, "dynamic-gop"},
    { "disable", "Disable tune metrics (0)", 0, AV_OPT_TYPE_CONST, { .i64 = 0}, 0, 0, VE, "tune-metrics"},
    { "enable", "Enable tune metrics (1)", 0, AV_OPT_TYPE_CONST, { .i64 = 1}, 0, 0, VE, "tune-metrics"},
    {NULL},
};

int vcu_alloc_ff_packet(mpsoc_vcu_enc_ctx *ctx, AVPacket *pkt);

#endif //XLNX_ENCODER_H
