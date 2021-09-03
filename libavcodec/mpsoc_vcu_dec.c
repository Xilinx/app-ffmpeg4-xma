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
#include "libavutil/fifo.h"
#include "libavcodec/h264dec.h"
#include "libavcodec/h264_parse.h"
#include "libavcodec/hevc_parse.h"
#include "libavcodec/hevcdec.h"
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
#include <dlfcn.h>

#include "../xmaPropsTOjson.h"

/* Below are the initial sizes. These sizes will not be the max that can be seen
 * at runtime, can grow but not indefinetely. */
#define PKT_FIFO_SIZE  20
#define PKT_FIFO_WATERMARK_SIZE 10

#define MAX_DEC_PARAMS 11
#define XRM_PRECISION_1000000_BIT_MASK(load) ((load << 8))


typedef struct mpsoc_vcu_dec_ctx {
    const AVClass     *class;
    XmaDecoderSession *dec_session;
    char               dec_params_name[MAX_DEC_PARAMS][100];
    XmaParameter       dec_params[MAX_DEC_PARAMS];
    xrmContext        *xrm_ctx;
    xrmCuListResource  decode_cu_list_res;
    xrmCuResource      decode_cu_hw_res;
    xrmCuResource      decode_cu_sw_res;
    int                decode_res_inuse;
    XmaDataBuffer      buffer;
    XmaFrame           xma_frame;
    XmaFrameProperties props;
    AVCodecContext    *avctx;
    bool               flush_sent;
    uint32_t           bitdepth;
    uint32_t           codec_type;
    uint32_t           low_latency;
    uint32_t           entropy_buffers_count;
    uint32_t           latency_logging;
    uint32_t           splitbuff_mode;
    int                first_idr_found;
    AVFifoBuffer      *pkt_fifo;
    int64_t            genpts;
    AVRational         pts_q;
    uint32_t           chroma_mode;
} mpsoc_vcu_dec_ctx;

#define VD AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_DECODING_PARAM
#define OFFSET(x) offsetof(mpsoc_vcu_dec_ctx, x)

static int vcu_dec_get_out_buffer(struct AVCodecContext *s, AVFrame *frame, int flags);
#define FLAGS (AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_FILTERING_PARAM)

static const AVOption options[] = {
    { "low_latency", "Should low latency decoding be used", OFFSET(low_latency), AV_OPT_TYPE_INT, { }, 0, 1, VD, "low_latency" },
    { "entropy_buffers_count", "Specify number of internal entropy buffers", OFFSET(entropy_buffers_count), AV_OPT_TYPE_INT , { .i64 = 2 }, 2, 10, VD, "entropy_buffers_count" },
    { "latency_logging", "Log latency information to syslog", OFFSET(latency_logging), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 1, VD, "latency_logging" },
    { "splitbuff_mode", "configure decoder in split/unsplit input buffer mode", OFFSET(splitbuff_mode), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 1, VD, "splitbuff_mode" },
    { NULL },
};

static int mpsoc_report_error(mpsoc_vcu_dec_ctx *ctx, const char *err_str, int32_t err_type)
{
    if (ctx)
    {
        if (ctx->decode_res_inuse == 2)
        {
            av_log(ctx, AV_LOG_ERROR, "decoder error: %s : ffmpeg pid %d on device index =  %d cu index = %d\n",
                   err_str, getpid(), ctx->decode_cu_hw_res.deviceId,
                   ctx->decode_cu_sw_res.cuId);
        }
        else
        {
            av_log(ctx, AV_LOG_ERROR, "decoder error: %s : ffmpeg pid %d on device index =  %d cu index = %d\n",
                   err_str, getpid(), ctx->decode_cu_list_res.cuResources[0].deviceId,
                   ctx->decode_cu_list_res.cuResources[1].cuId);
        }
    }

    return err_type;
}

static bool mpsoc_decode_is_h264_idr (AVPacket *pkt)
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

static bool mpsoc_decode_is_hevc_idr (AVPacket *pkt)
{
    unsigned char* pt = pkt->data;
    unsigned char* end = pkt->data + pkt->size - 3;
    while (pt < end)
    {
        if ((pt[0] == 0x00) && (pt[1] == 0x00) && (pt[2] == 0x01))
        {
            unsigned char naluType = (pt[3] & 0x7E) >> 1;
            if (naluType == 19 || naluType == 20 || naluType == 21)
                return true;
        }
        pt++;
    }
    return false;
}

static void  mpsoc_vcu_flush(AVCodecContext *avctx)
{
    mpsoc_vcu_dec_ctx *ctx = avctx->priv_data;

    /* reinitialize as we loop (-stream_loop) without going through init */
    ctx->flush_sent = false;
}

static av_cold int mpsoc_vcu_decode_close (AVCodecContext *avctx)
{
    mpsoc_vcu_dec_ctx *ctx = avctx->priv_data;

    if (ctx->pkt_fifo) {
        AVPacket *avpkt_free = NULL;
        while (av_fifo_size(ctx->pkt_fifo)) {
            av_fifo_generic_read(ctx->pkt_fifo, &avpkt_free, sizeof(AVPacket*), NULL);
            av_packet_free(&avpkt_free);
        }
        av_fifo_free(ctx->pkt_fifo);
    }

    xma_dec_session_destroy(ctx->dec_session);

    if (getenv("XRM_RESERVE_ID"))
    {
       int xrm_reserve_id = atoi(getenv("XRM_RESERVE_ID"));

       //XRM decoder list/CU de-allocation
       if (ctx->decode_res_inuse ==1)
       {
          if (!(xrmCuListRelease(ctx->xrm_ctx, &ctx->decode_cu_list_res)))
             av_log(avctx, AV_LOG_ERROR, "XRM: failed to release decoder HW cu\n");
       }
    }
    else if (ctx->decode_res_inuse == 2)
    {
          if (!(xrmCuRelease(ctx->xrm_ctx, &ctx->decode_cu_hw_res)))
             av_log(avctx, AV_LOG_ERROR, "XRM: failed to release decoder HW cu\n");
          if (!(xrmCuRelease(ctx->xrm_ctx, &ctx->decode_cu_sw_res)))
             av_log(avctx, AV_LOG_ERROR, "XRM: failed to release decoder SW cu\n");
    }

    if (xrmDestroyContext(ctx->xrm_ctx) != XRM_SUCCESS)
       av_log(avctx, AV_LOG_ERROR, "XRM : decoder destroy context failed\n");

    return 0;
}

static int vcu_dec_get_out_buffer(struct AVCodecContext *s, AVFrame *frame, int flags)
{
    mpsoc_vcu_dec_ctx *ctx;

    if (!s || !frame)
        return -1;

    ctx  = s->priv_data;

    frame->width       = s->width;;
    frame->height      = s->height;
    frame->linesize[0] = FFALIGN(s->width, 256);
    frame->linesize[1] = FFALIGN(s->height, 64);
    frame->format      = AV_PIX_FMT_XVBM; //output hard coded to zero copy
    return av_frame_clone_xma_frame (frame, &(ctx->xma_frame));
}

static int32_t mpsoc_send_data (
    mpsoc_vcu_dec_ctx *ctx,
    unsigned char *buf,
    uint32_t       size,
    int32_t        pts,
    int            is_eof
)
{
    int data_used;
    int offset    = 0;
    while (offset < size)
    {
        ctx->buffer.data.buffer = buf;
        ctx->buffer.alloc_size = size;
        ctx->buffer.is_eof = is_eof;
        ctx->buffer.pts = pts;

        int32_t ret = xma_dec_session_send_data(ctx->dec_session, &ctx->buffer, &data_used);
        if (ret != XMA_SUCCESS)
            return ret;

        offset += data_used;
        pts = -1; /* only first packet will carry pts */
    }
    return XMA_SUCCESS;
}


//XRM decoder plugin load calculation
static int _calc_dec_load(xrmContext *xrm_ctx, XmaDecoderProperties *dec_props, int32_t func_id, int32_t *dec_load)
{
    char pluginName[XRM_MAX_NAME_LEN];

    xrmPluginFuncParam param;
    memset(&param, 0, sizeof(xrmPluginFuncParam));
    char *err;
    void *handle;
    void (*convertXmaPropsToJson)(void* props, char* funcName, char* jsonJob);

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

    (*convertXmaPropsToJson) (dec_props, "DECODER", param.input);
    dlclose(handle);

    strcpy(pluginName, "xrmU30DecPlugin");
    if (xrmExecPluginFunc(xrm_ctx, pluginName, func_id, &param) != XRM_SUCCESS)
    {
       av_log(NULL, AV_LOG_ERROR, "xrm_load_calculation: decoder plugin function %d, fail to run the function\n", func_id);
       return XMA_ERROR;
    }
    else
    {
       *dec_load = atoi((char*)(strtok(param.output, " ")));
       if (*dec_load <= 0)
       {
          av_log(NULL, AV_LOG_ERROR, "xrm_load_calculation: decoder plugin function %d, calculated load %d.\n", *dec_load);
          return XMA_ERROR;
       }
       else if(*dec_load > XRM_MAX_CU_LOAD_GRANULARITY_1000000)
       {
          av_log(NULL, AV_LOG_ERROR, "xrm_load_calculation: decoder plugin function %d, calculated load %d is greater than maximum supported.\n", *dec_load);
          return XMA_ERROR;
       }
    }

    return 0;
}

//XRM decoder CU list allocation
static int _xrm_dec_cuListAlloc(mpsoc_vcu_dec_ctx *ctx, int32_t dec_load, int32_t xrm_reserve_id, XmaDecoderProperties *dec_props)
{
    xrmCuListProperty decode_cu_list_prop;
    int ret = -1;
    memset(&decode_cu_list_prop, 0, sizeof(xrmCuListProperty));
    memset(&ctx->decode_cu_list_res, 0, sizeof(xrmCuListResource));

    decode_cu_list_prop.cuNum = 2;
    strcpy(decode_cu_list_prop.cuProps[0].kernelName, "decoder");
    strcpy(decode_cu_list_prop.cuProps[0].kernelAlias, "DECODER_MPSOC");
    decode_cu_list_prop.cuProps[0].devExcl = false;
    decode_cu_list_prop.cuProps[0].requestLoad = XRM_PRECISION_1000000_BIT_MASK(dec_load);
    decode_cu_list_prop.cuProps[0].poolId = xrm_reserve_id;

    strcpy(decode_cu_list_prop.cuProps[1].kernelName, "kernel_vcu_decoder");
    decode_cu_list_prop.cuProps[1].devExcl = false;
    decode_cu_list_prop.cuProps[1].requestLoad = XRM_PRECISION_1000000_BIT_MASK(XRM_MAX_CU_LOAD_GRANULARITY_1000000);
    decode_cu_list_prop.cuProps[1].poolId = xrm_reserve_id;

    ret = xrmCuListAlloc(ctx->xrm_ctx, &decode_cu_list_prop, &ctx->decode_cu_list_res);

    if (ret != 0)
    {
        av_log(NULL, AV_LOG_ERROR, "xrm_allocation: fail to allocate cu list from reserve id %d\n", xrm_reserve_id );
        return ret;
    }
    else
    {
        ctx->decode_res_inuse =1;
    }

    //Set XMA plugin SO and device index
    dec_props->plugin_lib = ctx->decode_cu_list_res.cuResources[0].kernelPluginFileName;
    dec_props->dev_index = ctx->decode_cu_list_res.cuResources[0].deviceId;
    dec_props->ddr_bank_index = -1;//XMA to select the ddr bank based on xclbin meta data
    dec_props->cu_index = ctx->decode_cu_list_res.cuResources[1].cuId;
    dec_props->channel_id = ctx->decode_cu_list_res.cuResources[1].channelId;//SW kernel always used 100%

    return 0;
}

//XRM decoder CU allocation
static int _xrm_dec_cuAlloc(mpsoc_vcu_dec_ctx *ctx, int32_t dec_load, XmaDecoderProperties *dec_props)
{
    xrmCuProperty decode_cu_hw_prop, decode_cu_sw_prop;
    int ret = -1;
    int hw_xdevice_id =  atoi(getenv("XRM_DEVICE_ID"));
    memset(&decode_cu_hw_prop, 0, sizeof(xrmCuProperty));
    memset(&decode_cu_sw_prop, 0, sizeof(xrmCuProperty));
    memset(&ctx->decode_cu_hw_res, 0, sizeof(xrmCuResource));
    memset(&ctx->decode_cu_sw_res, 0, sizeof(xrmCuResource));

    strcpy(decode_cu_hw_prop.kernelName, "decoder");
    strcpy(decode_cu_hw_prop.kernelAlias, "DECODER_MPSOC");
    decode_cu_hw_prop.devExcl = false;
    decode_cu_hw_prop.requestLoad = XRM_PRECISION_1000000_BIT_MASK(dec_load);

    strcpy(decode_cu_sw_prop.kernelName, "kernel_vcu_decoder");
    decode_cu_sw_prop.devExcl = false;
    decode_cu_sw_prop.requestLoad = XRM_PRECISION_1000000_BIT_MASK(XRM_MAX_CU_LOAD_GRANULARITY_1000000);

    ret = xrmCuAllocFromDev(ctx->xrm_ctx, hw_xdevice_id, &decode_cu_hw_prop, &ctx->decode_cu_hw_res);

    if (ret != 0)
    {
        av_log(NULL, AV_LOG_ERROR, "xrm_allocation: failed to allocate decoder resources from device %d\n", hw_xdevice_id );
        return ret;
    }
    else
    {
        ret = xrmCuAllocFromDev(ctx->xrm_ctx, hw_xdevice_id, &decode_cu_sw_prop, &ctx->decode_cu_sw_res);
        if (ret != 0)
        {
           av_log(NULL, AV_LOG_ERROR, "xrm_allocation: failed to allocate decoder resources from device %d\n", hw_xdevice_id );
           return ret;
        }
        else
           ctx->decode_res_inuse =2;
    }

    //Set XMA plugin SO and device index
    dec_props->plugin_lib = ctx->decode_cu_hw_res.kernelPluginFileName;
    dec_props->dev_index = ctx->decode_cu_hw_res.deviceId;
    dec_props->ddr_bank_index = -1;//XMA to select the ddr bank based on xclbin meta data
    dec_props->cu_index = ctx->decode_cu_sw_res.cuId;
    dec_props->channel_id = ctx->decode_cu_sw_res.channelId;//SW kernel always used 100%

    return 0;
}

static int _allocate_xrm_dec_cu(mpsoc_vcu_dec_ctx *ctx, XmaDecoderProperties *dec_props)
{

    int xrm_reserve_id = -1;
    int ret = -1;

    ctx->xrm_ctx = (xrmContext *)xrmCreateContext(XRM_API_VERSION_1);
    if (ctx->xrm_ctx == NULL)
    {
       av_log(NULL, AV_LOG_ERROR, "create local XRM context failed\n");
       return XMA_ERROR;
    }

    //XRM decoder plugin load calculation
    int32_t func_id = 0, dec_load=0;
    ret = _calc_dec_load(ctx->xrm_ctx, dec_props, func_id, &dec_load);
    if (ret < 0) return ret;

    if (getenv("XRM_RESERVE_ID"))
    {
       xrm_reserve_id = atoi(getenv("XRM_RESERVE_ID"));
       ret = _xrm_dec_cuListAlloc(ctx,dec_load,xrm_reserve_id,dec_props);
       if (ret < 0) return ret;
    }
    else
    {
       ret = _xrm_dec_cuAlloc(ctx,dec_load,dec_props);
       if (ret < 0) return ret;
    }
    av_log(NULL, AV_LOG_DEBUG, "---decoder xrm out: dec_load=%d, plugin=%s, device=%d, cu=%d, ch=%d\n", 
    dec_load, dec_props->plugin_lib,dec_props->dev_index,dec_props->cu_index,dec_props->channel_id);

    return ret;
}

static bool extract_stream_info(AVCodecContext *avctx)
{
    uint32_t fr_num, fr_den;
    int64_t gcd;
    int ret;
    mpsoc_vcu_dec_ctx *ctx = avctx->priv_data;

    /* FFmpeg assigns codec_id based on user specified parameter on command line. Hence it is not reliable.
     * Following is a workaround to prevent incorrect parsing based on avctx->codec_id.
     */
    char *name = avcodec_profile_name(avctx->codec_id, avctx->profile);
    if (!name) {
        av_log(ctx, AV_LOG_ERROR, "input stream type does not match with specified codec type\n");
	return false;
    }

    if (avctx->extradata_size > 0 && avctx->extradata) {
        if (avctx->codec_id == AV_CODEC_ID_H264) {
            const SPS *h264_sps = NULL;
            H264Context s;
            memset(&s, 0, sizeof(H264Context));
            ret = ff_h264_decode_extradata(avctx->extradata, avctx->extradata_size, &s.ps, &s.is_avc, &s.nal_length_size, avctx->err_recognition, avctx);
            if (ret < 0) {
                ff_h264_ps_uninit(&s.ps);
                av_log(ctx, AV_LOG_ERROR, "decoding extradata failed\n");
		return false;
            }

            for (int i = 0; i < MAX_SPS_COUNT; i++) {
                if (s.ps.sps_list[i]) {
                    h264_sps = (SPS *) s.ps.sps_list[i]->data;
                    break;
                }
            }

            if (h264_sps && h264_sps->timing_info_present_flag) {
                fr_num = h264_sps->time_scale;
                fr_den = h264_sps->num_units_in_tick * 2;
                gcd = av_gcd(fr_num, fr_den);
                if (gcd > 0) {
                    avctx->framerate.num = fr_num/gcd;
                    avctx->framerate.den = fr_den/gcd;
                }
            } else {
                av_log(ctx, AV_LOG_INFO, "timing information from stream is not available\n");
            }

            ctx->bitdepth = h264_sps->bit_depth_luma;

            if (h264_sps->chroma_format_idc == 0)
                ctx->chroma_mode = 400;
            else if (h264_sps->chroma_format_idc == 1)
                ctx->chroma_mode = 420;
            else if (h264_sps->chroma_format_idc == 2)
                ctx->chroma_mode = 422;
            else if (h264_sps->chroma_format_idc == 3)
                ctx->chroma_mode = 444;
            else
                ctx->chroma_mode = 420;

            ff_h264_ps_uninit(&s.ps);
        } else {
            //HEVC
            HEVCContext s;
            const HEVCSPS *hevc_sps = NULL;
            memset(&s, 0, sizeof(HEVCContext));
            ret = ff_hevc_decode_extradata(avctx->extradata, avctx->extradata_size, &s.ps, &s.sei, &s.is_nalff,
                                                &s.nal_length_size, avctx->err_recognition,
                                                s.apply_defdispwin, avctx);
            if (ret < 0) {
                ff_hevc_ps_uninit(&s.ps);
                av_log(ctx, AV_LOG_ERROR, "decoding extradata failed\n");
		return false;
            }

            for (int i = 0; i < MAX_SPS_COUNT; i++) {
                if (s.ps.sps_list[i]) {
                    hevc_sps= (HEVCSPS *) s.ps.sps_list[i]->data;
                    break;
                }
            }

            if (hevc_sps && hevc_sps->vui.vui_timing_info_present_flag) {
                fr_num = hevc_sps->vui.vui_time_scale;
                fr_den = hevc_sps->vui.vui_num_units_in_tick;
                gcd = av_gcd(fr_num, fr_den);
                if (gcd > 0) {
                    avctx->framerate.num = fr_num/gcd;
                    avctx->framerate.den = fr_den/gcd;
	        }
            } else {
                av_log(ctx, AV_LOG_INFO, "timing information from stream is not available\n");
            }

            ctx->bitdepth = hevc_sps->bit_depth;

            if (hevc_sps->chroma_format_idc == 0)
                ctx->chroma_mode = 400;
            else if (hevc_sps->chroma_format_idc == 1)
                ctx->chroma_mode = 420;
            else if (hevc_sps->chroma_format_idc == 2)
                ctx->chroma_mode = 422;
            else if (hevc_sps->chroma_format_idc == 3)
                ctx->chroma_mode = 444;
            else
                ctx->chroma_mode = 420;

            ff_hevc_ps_uninit(&s.ps);
        }
    }

    return true;
}

static av_cold int mpsoc_vcu_decode_init (AVCodecContext *avctx)
{
    XmaDecoderProperties dec_props;
    mpsoc_vcu_dec_ctx   *ctx  = avctx->priv_data;
    uint32_t scan_type, zero_copy, index;
    bool valid;

    /* extract stream information from SPS and update 'mpsoc_vcu_dec_ctx'.
     * This prior information is needed for decoder to derive optimum output buffer size during preallocation */
    valid = extract_stream_info(avctx);
    if (!valid)
        return AVERROR(ENOTSUP);

    strcpy(dec_props.hwvendor_string, "MPSoC");

    dec_props.hwdecoder_type        = XMA_MULTI_DECODER_TYPE;
    dec_props.params                = ctx->dec_params;
    dec_props.param_cnt             = MAX_DEC_PARAMS;
    dec_props.width                 = avctx->width;
    dec_props.height                = avctx->height;
    dec_props.framerate.numerator   = avctx->framerate.num;

    if (avctx->framerate.den)
        dec_props.framerate.denominator = avctx->framerate.den;
    else
        dec_props.framerate.denominator = 1;

    scan_type = avctx->field_order;

    ctx->avctx = avctx;
    ctx->flush_sent = false;
    index = 0;

    strcpy(ctx->dec_params_name[index], "bitdepth");
    ctx->dec_params[index].name       = ctx->dec_params_name[index];
    ctx->dec_params[index].type       = XMA_UINT32;
    ctx->dec_params[index].length     = sizeof(ctx->bitdepth);
    ctx->dec_params[index].value      = &(ctx->bitdepth);
    index++;

    ctx->codec_type = (avctx->codec_id == AV_CODEC_ID_H264) ? 0 : 1;
    strcpy(ctx->dec_params_name[index], "codec_type");
    ctx->dec_params[index].name   = ctx->dec_params_name[index];
    ctx->dec_params[index].type   = XMA_UINT32;
    ctx->dec_params[index].length = sizeof(ctx->codec_type);
    ctx->dec_params[index].value  = &(ctx->codec_type);
    index++;

    strcpy(ctx->dec_params_name[index], "low_latency");
    ctx->dec_params[index].name   = ctx->dec_params_name[index];
    ctx->dec_params[index].type   = XMA_UINT32;
    ctx->dec_params[index].length = sizeof(ctx->low_latency);
    ctx->dec_params[index].value  = &(ctx->low_latency);
    index++;

    strcpy(ctx->dec_params_name[index], "entropy_buffers_count");
    ctx->dec_params[index].name   = ctx->dec_params_name[index];
    ctx->dec_params[index].type   = XMA_UINT32;
    ctx->dec_params[index].length = sizeof(ctx->entropy_buffers_count);
    ctx->dec_params[index].value  = &(ctx->entropy_buffers_count);
    index++;

    zero_copy = 1; //always zero copy output
    strcpy(ctx->dec_params_name[index], "zero_copy");
    ctx->dec_params[index].name   = ctx->dec_params_name[index];
    ctx->dec_params[index].type   = XMA_UINT32;
    ctx->dec_params[index].length = sizeof(zero_copy);
    ctx->dec_params[index].value  = &(zero_copy);
    index++;

    strcpy(ctx->dec_params_name[index], "profile");
    ctx->dec_params[index].name   = ctx->dec_params_name[index];
    ctx->dec_params[index].type   = XMA_UINT32;
    ctx->dec_params[index].length = sizeof(avctx->profile);
    ctx->dec_params[index].value  = &(avctx->profile);
    index++;

    strcpy(ctx->dec_params_name[index], "level");
    ctx->dec_params[index].name   = ctx->dec_params_name[index];
    ctx->dec_params[index].type   = XMA_UINT32;
    ctx->dec_params[index].length = sizeof(avctx->level);
    ctx->dec_params[index].value  = &(avctx->level);
    index++;

    strcpy(ctx->dec_params_name[index], "chroma_mode");
    ctx->dec_params[index].name   = ctx->dec_params_name[index];
    ctx->dec_params[index].type   = XMA_UINT32;
    ctx->dec_params[index].length = sizeof(ctx->chroma_mode);
    ctx->dec_params[index].value  = &(ctx->chroma_mode);
    index++;

    strcpy(ctx->dec_params_name[index], "scan_type");
    ctx->dec_params[index].name   = ctx->dec_params_name[index];
    ctx->dec_params[index].type   = XMA_UINT32;
    ctx->dec_params[index].length = sizeof(scan_type);
    ctx->dec_params[index].value  = &(scan_type);
    index++;

    strcpy(ctx->dec_params_name[index], "latency_logging");
    ctx->dec_params[index].name   = ctx->dec_params_name[index];
    ctx->dec_params[index].type   = XMA_UINT32;
    ctx->dec_params[index].length = sizeof(ctx->latency_logging);
    ctx->dec_params[index].value  = &(ctx->latency_logging);
    index++;

    ctx->dec_params[index].name   = "splitbuff_mode";
    ctx->dec_params[index].type   = XMA_UINT32;
    ctx->dec_params[index].length = sizeof(ctx->splitbuff_mode);
    ctx->dec_params[index].value  = &(ctx->splitbuff_mode);
    index++;

    /*----------------------------------------------------
      Allocate decoder resource from XRM reserved resource
      ----------------------------------------------------*/
    ctx->decode_res_inuse    = 0;
    if (_allocate_xrm_dec_cu(ctx, &dec_props) < 0) {
            av_log(ctx, AV_LOG_ERROR, "xrm_allocation: resource allocation failed\n");
            return XMA_ERROR;
    }


    ctx->dec_session = xma_dec_session_create(&dec_props);
    if (!ctx->dec_session)
        return mpsoc_report_error(ctx, "ERROR: Unable to allocate MPSoC decoder session", AVERROR_EXTERNAL);

    ctx->xma_frame.side_data                  = NULL;
    ctx->xma_frame.frame_props.format         = XMA_VCU_NV12_FMT_TYPE;
    ctx->xma_frame.frame_props.width          = avctx->width;
    ctx->xma_frame.frame_props.height         = avctx->height;
    ctx->xma_frame.frame_props.bits_per_pixel = ctx->bitdepth;
    ctx->xma_frame.frame_rate.numerator       = avctx->framerate.num;
    ctx->xma_frame.frame_rate.denominator     = avctx->framerate.den;

    for (uint32_t i = 0; i < xma_frame_planes_get(&ctx->xma_frame.frame_props); i++) {
        ctx->xma_frame.data[i].buffer = NULL;
        ctx->xma_frame.data[i].buffer_type = XMA_DEVICE_BUFFER_TYPE;
        ctx->xma_frame.data[i].refcount = 1;
        ctx->xma_frame.data[i].is_clone = 1;
    }

    ctx->pkt_fifo = av_fifo_alloc(PKT_FIFO_SIZE * sizeof(AVPacket));
    if (!ctx->pkt_fifo)
       return mpsoc_report_error(ctx, "packet fifo allocation failed", AVERROR(ENOMEM));

    ctx->genpts = 0;
    ctx->pts_q = av_make_q(0, 0);


    return 0;
}

static void set_pts(AVCodecContext *avctx, AVFrame *frame)
{
    AVRational fps;
    mpsoc_vcu_dec_ctx *ctx = avctx->priv_data;

    fps.num = avctx->time_base.den;
    fps.den = avctx->time_base.num * avctx->ticks_per_frame;

    frame->pts = ctx->xma_frame.pts = ctx->genpts;
    ctx->pts_q = av_div_q(av_inv_q(avctx->pkt_timebase), fps);
    frame->pts = (int64_t)(frame->pts * av_q2d(ctx->pts_q));

    ctx->genpts++;
}

static int mpsoc_vcu_decode (AVCodecContext *avctx, void *data, int *got_frame, AVPacket *avpkt)
{
    int send_ret, recv_ret;

    AVFrame *frame	= data;
    int data_used	= 0;
    mpsoc_vcu_dec_ctx *ctx = avctx->priv_data;
    AVPacket *avpkt_clone = NULL;
    AVPacket *buffer_pkt = NULL;

    int retries = 0;
    if (avpkt->data) {
        if (ctx->first_idr_found == 0)
        {
            if ((avctx->codec_id == AV_CODEC_ID_H264) ? mpsoc_decode_is_h264_idr (avpkt) : mpsoc_decode_is_hevc_idr (avpkt))
                ctx->first_idr_found = 1;
            else
            {
                *got_frame = 0;
                return avpkt->size;
            }
        }

        while (av_fifo_size(ctx->pkt_fifo)/sizeof(AVPacket *) > PKT_FIFO_WATERMARK_SIZE) {
            av_fifo_generic_peek(ctx->pkt_fifo, &buffer_pkt, sizeof(AVPacket *), NULL);
retry:
            send_ret = mpsoc_send_data(ctx, buffer_pkt->data, buffer_pkt->size, buffer_pkt->pts, 0);
            if (send_ret == XMA_TRY_AGAIN) {
                if (retries != 2) {
                    retries++;
                    goto retry;
                } else {
                    break;
                }
            } else if (send_ret == XMA_ERROR) {
                *got_frame = 0;
                return mpsoc_report_error(ctx, "failed to transfer data to decoder", AVERROR(EIO));
	    } else {
                av_fifo_generic_read(ctx->pkt_fifo, &buffer_pkt, sizeof(AVPacket *), NULL);
                av_packet_free(&buffer_pkt);
            }
        }

        if (av_fifo_size(ctx->pkt_fifo)) {
             av_fifo_generic_peek(ctx->pkt_fifo, &buffer_pkt, sizeof(AVPacket*), NULL);
             avpkt_clone = av_packet_clone(avpkt);

             if (av_fifo_space(ctx->pkt_fifo) < sizeof(AVPacket*)) {
                 int ret = av_fifo_grow(ctx->pkt_fifo, sizeof(AVPacket*));
                 if (ret < 0)
                     return ret;
             }
             av_fifo_generic_write(ctx->pkt_fifo, &avpkt_clone, sizeof(AVPacket*), NULL);

             send_ret = mpsoc_send_data(ctx, buffer_pkt->data, buffer_pkt->size, buffer_pkt->pts, 0);
        } else {
             send_ret = mpsoc_send_data(ctx, avpkt->data, avpkt->size, avpkt->pts, 0);
        }

        if (send_ret == XMA_ERROR) {
            *got_frame = 0;
            return mpsoc_report_error(ctx, "failed to transfer data to decoder", AVERROR(EIO));
        } else {
            recv_ret = xma_dec_session_recv_frame(ctx->dec_session, &(ctx->xma_frame));
            if (recv_ret != XMA_SUCCESS) {
                *got_frame = 0;
            } else {
                if (vcu_dec_get_out_buffer(avctx, frame, 0)) {
                    *got_frame = 0;
                } else {
                    *got_frame = 1;
                    set_pts(avctx, frame);
                }
            }

            if (send_ret == XMA_TRY_AGAIN) {
                if (!av_fifo_size(ctx->pkt_fifo)) {
                   avpkt_clone = av_packet_clone(avpkt);
                   av_fifo_generic_write(ctx->pkt_fifo, &avpkt_clone, sizeof(AVPacket*), NULL);
                }
            } else {
                if (av_fifo_size(ctx->pkt_fifo)) {
                   av_fifo_generic_read(ctx->pkt_fifo, &buffer_pkt, sizeof(AVPacket*), NULL);
                   av_packet_free(&buffer_pkt);
                }
            }
        }

        data_used =  avpkt->size;

    } else { // EOF

        while(true) {
            if (av_fifo_size(ctx->pkt_fifo)) {
                av_fifo_generic_peek(ctx->pkt_fifo, &buffer_pkt, sizeof(AVPacket*), NULL);
                send_ret = mpsoc_send_data(ctx, buffer_pkt->data, buffer_pkt->size, buffer_pkt->pts, 0);
            } else {
                /* flush not sent and queue is empty. send flush now */
                if (!ctx->flush_sent) {
                    ctx->flush_sent = true;
                    ctx->buffer.is_eof = 1;
                    send_ret = xma_dec_session_send_data(ctx->dec_session, &(ctx->buffer), &data_used);
                } else {
                /* send free output buffer indexes, so that decoding can continue on device side */
                  XmaDataBuffer eos_buff;
                  eos_buff.data.buffer = NULL;
                  eos_buff.alloc_size = 0;
                  eos_buff.is_eof = 0;
                  eos_buff.pts = -1;
                  send_ret = xma_dec_session_send_data(ctx->dec_session, &eos_buff, &data_used);
                }
            }
            if (send_ret == XMA_ERROR) {
                *got_frame = 0;
                return mpsoc_report_error(ctx, "failed to transfer data to decoder", AVERROR_UNKNOWN);
            }
            if (send_ret == XMA_SUCCESS) {
                if (av_fifo_size(ctx->pkt_fifo)) {
                    av_fifo_generic_read(ctx->pkt_fifo, &buffer_pkt, sizeof(AVPacket*), NULL);
                    av_packet_free(&buffer_pkt);
                }
            }

            recv_ret = xma_dec_session_recv_frame(ctx->dec_session, &(ctx->xma_frame));
            if (recv_ret != XMA_SUCCESS) {
                *got_frame = 0;
            } else {
                if (vcu_dec_get_out_buffer(avctx, frame, 0)) {
                    *got_frame = 0;
                } else {
                    *got_frame = 1;
                    set_pts(avctx, frame);
                }
            }

           if (!ctx->flush_sent) {
             if (recv_ret == XMA_SUCCESS)
              break;
             else
               continue;
           } else {
             if (recv_ret == XMA_TRY_AGAIN) {
               continue;
             } else {
               break;
             }
           }
       }
     data_used = 0;

   }

    return data_used;
}

static const AVClass mpsoc_vcu_h264_class = {
    .class_name    =    "MPSOC H.264 decoder",
    .item_name     =    av_default_item_name,
    .option        =    options,
    .version       =    LIBAVUTIL_VERSION_INT,
};

AVCodec ff_h264_vcu_mpsoc_decoder = {
    .name                =    "mpsoc_vcu_h264",
    .long_name           =    NULL_IF_CONFIG_SMALL("MPSOC H.264 Decoder"),
    .type                =    AVMEDIA_TYPE_VIDEO,
    .id                  =    AV_CODEC_ID_H264,
    .init                =    mpsoc_vcu_decode_init,
    .decode              =    mpsoc_vcu_decode,
    .flush               =    mpsoc_vcu_flush,
    .bsfs                =    "h264_mp4toannexb",
    .close               =    mpsoc_vcu_decode_close,
    .priv_data_size      =    sizeof(mpsoc_vcu_dec_ctx),
    .priv_class          =    &mpsoc_vcu_h264_class,
    .capabilities        =    AV_CODEC_CAP_DELAY | AV_CODEC_CAP_AVOID_PROBING,
    .pix_fmts            =    (const enum AVPixelFormat[]) { AV_PIX_FMT_XVBM,
                                                             AV_PIX_FMT_NONE
                                                           },
};

static const AVClass mpsoc_vcu_hevc_class = {
    .class_name    =    "MPSOC HEVC decoder",
    .item_name     =    av_default_item_name,
    .option        =    options,
    .version       =    LIBAVUTIL_VERSION_INT,
};

AVCodec ff_hevc_vcu_mpsoc_decoder = {
    .name                =    "mpsoc_vcu_hevc",
    .long_name           =    NULL_IF_CONFIG_SMALL("MPSOC HEVC Decoder"),
    .type                =    AVMEDIA_TYPE_VIDEO,
    .id                  =    AV_CODEC_ID_HEVC,
    .init                =    mpsoc_vcu_decode_init,
    .decode              =    mpsoc_vcu_decode,
    .flush               =    mpsoc_vcu_flush,
    .bsfs                =    "hevc_mp4toannexb",
    .close               =    mpsoc_vcu_decode_close,
    .priv_data_size      =    sizeof(mpsoc_vcu_dec_ctx),
    .priv_class          =    &mpsoc_vcu_hevc_class,
    .capabilities        =    AV_CODEC_CAP_DELAY | AV_CODEC_CAP_AVOID_PROBING,
    .pix_fmts            =    (const enum AVPixelFormat[]) { AV_PIX_FMT_XVBM,
                                                             AV_PIX_FMT_NONE
                                                           },
};
