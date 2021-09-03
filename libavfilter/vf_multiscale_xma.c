/*
 * Copyright (c) 2018 Xilinx
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3.0 of the License, or (at your option) any later version.
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

/**
 * @file
 * video Multi Scaler IP (in ABR mode) with Xilinx Media Accelerator
 */

#include <stdio.h>
#include <unistd.h>
#include <xma.h>
#include <xmaplugin.h>
#include <xrm.h>

#include "libavutil/attributes.h"
#include "libavutil/internal.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/imgutils.h"
#include "libavutil/pixfmt.h"

#include "avfilter.h"
#include "formats.h"
#include "internal.h"
#include "video.h"
#include <xvbm.h>
#include <dlfcn.h>

#include "../xmaPropsTOjson.h"

#define MAX_OUTS            8
#define MAX_PARAMS          2
#define VCU_STRIDE_ALIGN    256
#define VCU_HEIGHT_ALIGN    64

#define MAX_INPUT_WIDTH     3840
#define MAX_INPUT_HEIGHT    2160
#define MAX_INPUT_PIXELS    (MAX_INPUT_WIDTH * MAX_INPUT_HEIGHT)
#define XRM_PRECISION_1000000_BIT_MASK(load) ((load << 8))

#define ALIGN(width,align) (((width) + (align) - 1) & ~((align) - 1))

#undef DUMP_OUT_FRAMES
#undef DUMP_FRAME_PARAM

#ifdef DUMP_OUT_FRAMES
FILE *outfp = NULL;
FILE *yfp = NULL;
#endif

typedef enum {
    SC_SESSION_ALL_RATE = 0,
    SC_SESSION_FULL_RATE,
    SC_MAX_SESSIONS
} MultiScalerSessionType;

typedef struct MultiScalerContext {
    const AVClass    *class;
    int               nb_outputs;
    int               out_width[MAX_OUTS];
    int               out_height[MAX_OUTS];
    char             *out_format[MAX_OUTS];
    char             *out_rate[MAX_OUTS];
    unsigned int      fps;
    AVRational        in_frame_rate;
    AVRational        out_frame_rate[MAX_OUTS];
    int              *copyOutLink;
    int               flush;
    int               send_status;
    int               frames_out;
    int               latency_logging;
    int               num_sessions;
    int               session_frame;
    uint64_t          p_mixrate_session;
    int               session_nb_outputs[SC_MAX_SESSIONS];
    char              sc_param_name[MAX_PARAMS][50];
    XmaParameter      sc_params[MAX_PARAMS];
    XmaScalerSession *session[SC_MAX_SESSIONS];
    xrmContext       *xrm_ctx;
    xrmCuResource     scalerCuRes[SC_MAX_SESSIONS];
    int               xrm_scalres_count;
    int               xrm_reserve_id;
    int               xrm_alloc_st[SC_MAX_SESSIONS];
} MultiScalerContext;


static int multiscale_xma_filter_frame(AVFilterLink *link, AVFrame *frame);
static int output_config_props(AVFilterLink *outlink);
static int validate_rate_config(MultiScalerContext *ctx);
static int get_num_scaler_sessions(MultiScalerContext *ctx);
static int get_num_full_rate_outputs(MultiScalerContext *ctx);
static void write_session_log(MultiScalerContext *ctx);

static int mpsoc_report_error(MultiScalerContext *ctx, const char *err_str, int32_t err_type)
{
    if (ctx)
        av_log(NULL, AV_LOG_ERROR, "scaler error: %s: ffmpeg pid %d on device index =  %d cu index = %d\n",
               err_str, getpid(), ctx->scalerCuRes[ctx->session_frame].deviceId,
               ctx->scalerCuRes[ctx->session_frame].cuId);

    return err_type;
}

static int validate_rate_config(MultiScalerContext *ctx)
{
    int i, ret;
    int count = 0;

    //All outputs @half-rate not supported
    for (i = 0; i < ctx->nb_outputs; ++i) {
        if (strcmp(ctx->out_rate[i], "half") == 0) {
            count += 1;
            ctx->out_frame_rate[i].num = ctx->in_frame_rate.num /2 ;
            ctx->out_frame_rate[i].den = ctx->in_frame_rate.den ;
        }
        else if (strcmp(ctx->out_rate[i], "full") == 0)
        {
            ctx->out_frame_rate[i].num = ctx->in_frame_rate.num ;
            ctx->out_frame_rate[i].den = ctx->in_frame_rate.den ;
        }
        else if (strcmp(ctx->out_rate[i], "full") != 0)
        {
          return -2;
        }
    }
    ret = ((ctx->nb_outputs == count) ? -1 : 0);
    return (ret);
}

static int get_num_scaler_sessions(MultiScalerContext *ctx)
{
    int i;
    int count = 1;

    /* default = 1 session - full rate
       However if Mix out_rate is found then 2 sessions will
       be created to allow for frame drops.
    */
    for (i = 0; i < ctx->nb_outputs; ++i) {
        if (strcmp(ctx->out_rate[i], "full") != 0) {
            count = 2;
            break;
        }
    }
    return (count);
}

static int get_num_full_rate_outputs(MultiScalerContext *ctx)
{
    int i;
    int count = 0;

    for (i = 0; i < ctx->nb_outputs; ++i) {
        if (strcmp(ctx->out_rate[i], "full") == 0) {
            count += 1;
        }
    }
    return (count);
}

static void write_session_log(MultiScalerContext *ctx)
{
    int i, count;

    av_log(NULL, AV_LOG_DEBUG, "  Multi-Scaler Session Configuration\n");
    av_log(NULL, AV_LOG_DEBUG, "---------------------------------------\n");
    av_log(NULL, AV_LOG_DEBUG, "Num Sessions = %d\n\n", ctx->num_sessions);

    for (count = 0; count < ctx->num_sessions; ++count) {
        av_log(NULL, AV_LOG_DEBUG, "Session:  %d\n", count);
        if (ctx->num_sessions > 1) {
            av_log(NULL, AV_LOG_DEBUG, "Type   :  %s\n", ((count) ? "FULL RATE ONLY" : "HALF RATE"));
        } else {
            av_log(NULL, AV_LOG_DEBUG, "Type   :  %s\n", "ALL RATE");
        }
        av_log(NULL, AV_LOG_DEBUG, "Num Out:  %d\n", ctx->session_nb_outputs[count]);
        for (i = 0; i < ctx->session_nb_outputs[count]; ++i) {
            av_log(NULL, AV_LOG_DEBUG, "out_%d :  (%4d x %4d) @%d fps\n", i, ctx->out_width[i], ctx->out_height[i], ctx->fps);
        }
        av_log(NULL, AV_LOG_DEBUG, "--------------------------\n");
    }
}
///////////////////////////////////////


static XmaFormatType get_xma_format (enum AVPixelFormat av_format)
{
    switch (av_format) {
        case AV_PIX_FMT_NV12:
        case AV_PIX_FMT_XVBM:
            return XMA_VCU_NV12_FMT_TYPE;
        case AV_PIX_FMT_BGR24:
            return XMA_RGB888_FMT_TYPE;
        default:
            av_log (NULL, AV_LOG_ERROR, "[%s][%d]ERROR : unsupported format %d\n", __func__, __LINE__, av_format);
            return XMA_NONE_FMT_TYPE;
    }
}

static int get_stride (enum AVPixelFormat av_format, int width)
{
    switch (av_format) {
        case AV_PIX_FMT_XVBM:
        case AV_PIX_FMT_NV12:
            return ALIGN(width, VCU_STRIDE_ALIGN);
        case AV_PIX_FMT_BGR24:
            return width * 3;
        default:
            av_log (NULL, AV_LOG_ERROR, "[%s][%d]ERROR : unsupported format %d\n", __func__, __LINE__, av_format);
            return XMA_NONE_FMT_TYPE;
    }
}

//XRM scaler plugin load calculation
static int _calc_scal_load(AVFilterContext *ctx, xrmContext *xrm_ctx, XmaScalerProperties *props, int32_t func_id, int32_t *scal_load)
{
    char pluginName[XRM_MAX_NAME_LEN];
    char *err;

    xrmPluginFuncParam param;
    void *handle;
    void (*convertXmaPropsToJson)(void* props, char* funcName, char* jsonJob);

    memset(&param, 0, sizeof(xrmPluginFuncParam));
    handle = dlopen("/opt/xilinx/xrm/plugin/libxmaPropsTOjson.so", RTLD_NOW );
    if (!handle) {
        av_log(ctx, AV_LOG_ERROR, "Unable to load libxmaPropsTOjson.so  - %s\n", dlerror());
        return XMA_ERROR;
    }
    dlerror(); /* clear error code */
    convertXmaPropsToJson = dlsym(handle, "convertXmaPropsToJson");
    if ((err = dlerror()) != NULL) {
         av_log(ctx, AV_LOG_ERROR, "convertXmaPropsToJson symbol not found\n");
         return XMA_ERROR;
    }
    (*convertXmaPropsToJson) (props, (char *)"SCALER", param.input);
    dlclose(handle);

    strcpy(pluginName, "xrmU30ScalPlugin");
    if (xrmExecPluginFunc(xrm_ctx, pluginName, func_id, &param) != XRM_SUCCESS)
    {
        av_log(ctx, AV_LOG_ERROR, "xrm_load_calculation: scaler plugin function %d, fail to run the function\n", func_id);
        return XMA_ERROR;
    }
    else {
         *scal_load = atoi((char*)(strtok(param.output, " ")));
         if (*scal_load <= 0)
         {
            av_log(NULL, AV_LOG_ERROR, "xrm_load_calculation: scaler plugin function %d, calculated wrong load %d .\n", *scal_load);
            return XMA_ERROR;
         }
         else if (*scal_load > XRM_MAX_CU_LOAD_GRANULARITY_1000000)
         {
            av_log(NULL, AV_LOG_ERROR, "xrm_load_calculation: scaler plugin function %d, calculated load %d is greater than maximum supported.\n", *scal_load);
            return XMA_ERROR;
         }

    }

    return 0;

}

static int _allocate_xrm_scaler_cu(AVFilterContext *ctx, XmaScalerProperties *props)
{
    int32_t scal_load=0, func_id = 0;
    int ret = -1;

    MultiScalerContext  *s = ctx->priv;
    xrmCuProperty scalerCuProp;

    ret = _calc_scal_load(ctx, s->xrm_ctx, props, func_id, &scal_load);
    if (ret < 0) return ret;

    //XRM scaler cu allocation
    memset(&scalerCuProp,                        0, sizeof(xrmCuProperty));
    memset(&s->scalerCuRes[s->xrm_scalres_count], 0, sizeof(xrmCuResource));

    strcpy(scalerCuProp.kernelName,  "scaler");
    strcpy(scalerCuProp.kernelAlias, "SCALER_MPSOC");

    scalerCuProp.devExcl     = false;
    scalerCuProp.requestLoad = XRM_PRECISION_1000000_BIT_MASK(scal_load);

    if (s->xrm_reserve_id > 0) {
        scalerCuProp.poolId      = s->xrm_reserve_id;
        ret = xrmCuAlloc(s->xrm_ctx, &scalerCuProp, &s->scalerCuRes[s->xrm_scalres_count]);
        if (ret != 0) {
            av_log(ctx, AV_LOG_ERROR, "xrm_allocation: fail (err_code=%d) to allocate scaler cu from reserve id %d\n",
                            ret, s->xrm_reserve_id);
            return XMA_ERROR;
        }
    }
    else
    {
        int hw_xdevice_id =  atoi(getenv("XRM_DEVICE_ID"));

        ret = xrmCuAllocFromDev(s->xrm_ctx, hw_xdevice_id, &scalerCuProp,  &s->scalerCuRes[s->xrm_scalres_count]);

        if (ret != 0)
        {
            av_log(NULL, AV_LOG_ERROR, "xrm_allocation: failing to allocate scaler resources on device id %d\n", hw_xdevice_id );
            return XMA_ERROR;
        }

    }

    //Set XMA plugin SO and device index
    props->plugin_lib     = s->scalerCuRes[s->xrm_scalres_count].kernelPluginFileName;
    props->dev_index      = s->scalerCuRes[s->xrm_scalres_count].deviceId;
    props->cu_index       = s->scalerCuRes[s->xrm_scalres_count].cuId;
    props->channel_id     = s->scalerCuRes[s->xrm_scalres_count].channelId;
    props->ddr_bank_index = -1;//XMA to select the ddr bank based on xclbin meta data

    s->xrm_alloc_st[s->xrm_scalres_count]= 1;

    av_log(NULL, AV_LOG_DEBUG, "---scaler[%d] xrm out: scal_load=%d, plugin=%s, device=%d, cu=%d, ch=%d  \n",
    s->xrm_scalres_count, scal_load, props->plugin_lib, props->dev_index, props->cu_index, props->channel_id);

    return 0;
}

static av_cold int multiscale_xma_init(AVFilterContext *ctx)
{
    int i = 0;
    MultiScalerContext *s = ctx->priv;
    s->frames_out = 0;

    memset (s->xrm_alloc_st, 0, SC_MAX_SESSIONS*sizeof(int));
#ifdef DUMP_OUT_FRAMES
    outfp = fopen ("outframes.yuv", "w+");
    yfp = fopen ("outframes_y.yuv", "w+");
#endif

    for (i = 0; i < s->nb_outputs; i++) {
        char name[32];
        AVFilterPad pad = { 0 };

        snprintf(name, sizeof(name), "output%d", i);
        pad.type = ctx->filter->inputs[0].type;
        pad.name = av_strdup(name);
        if (!pad.name) {
            av_log(ctx, AV_LOG_ERROR, "out of memory\n");
            return AVERROR(ENOMEM);
        }
        pad.config_props = output_config_props;
        ff_insert_outpad(ctx, i, &pad);
    }
    return 0;
}

static av_cold void multiscale_xma_uninit(AVFilterContext *ctx)
{
    int i;
    MultiScalerContext *s = ctx->priv;

#ifdef DUMP_OUT_FRAMES
    fclose (outfp);
    fclose (yfp);
#endif
    for (i = 0; i < ctx->nb_outputs; i++)
        av_freep(&ctx->output_pads[i].name);

       for (int idx=0; idx < s->num_sessions; idx++) {
          if (s->session[idx])
            xma_scaler_session_destroy(s->session[idx]);
       }
    //if (getenv("XRM_RESERVE_ID") && s->xrm_ctx) {
    if (s->xrm_ctx) {

       //XRM scaler de-allocation
       for (int idx=0; idx <= s->xrm_scalres_count; idx++) {
             if (s->xrm_alloc_st[idx]==1) //Release only when resource is allocated
             {
                if (!(xrmCuRelease(s->xrm_ctx, &s->scalerCuRes[idx])))
                   av_log(NULL, AV_LOG_ERROR, "XRM: fail to release scaler HW cu idx=%d\n",idx);
             }
       }

       if (xrmDestroyContext(s->xrm_ctx) != XRM_SUCCESS)
        av_log(NULL, AV_LOG_ERROR, "XRM : scaler destroy context failed\n");
    }
}

int output_config_props(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    MultiScalerContext *s = ctx->priv;
    const int outlink_idx = FF_OUTLINK_IDX(outlink);
    AVFilterLink *out = outlink->src->outputs[outlink_idx];

    out->w = s->out_width[outlink_idx];
    out->h = s->out_height[outlink_idx];
    outlink->sample_aspect_ratio = (AVRational) {1, 1};

    //Set correct out fps for each channel
    outlink->frame_rate.num = s->out_frame_rate[outlink_idx].num;
    outlink->frame_rate.den = s->out_frame_rate[outlink_idx].den;
    //av_log(ctx, AV_LOG_INFO, "---channelid[%d]:  fps set as %d/%d\n", outlink_idx, outlink->frame_rate.num,outlink->frame_rate.den );

    return 0;
}

static int multiscale_xma_config_props(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->dst;
    AVFilterLink *inlink = outlink->dst->inputs[0];
    XmaScalerProperties props;
    MultiScalerContext *s = ctx->priv;
    int n = 0, count=0, ret;
    int chan_id = 0;
    s->fps = 25;
    s->p_mixrate_session = 0;

    memset((void*)&props, 0, sizeof(XmaScalerProperties));
    props.hwscaler_type = XMA_POLYPHASE_SCALER_TYPE;
    strcpy(props.hwvendor_string, "Xilinx");
    props.num_outputs = s->nb_outputs;

    props.input.format = get_xma_format(inlink->format);
    if ((props.input.format)==XMA_NONE_FMT_TYPE)
       return XMA_ERROR;

    props.input.width  = inlink->w;
    props.input.height = inlink->h;
    props.input.stride = get_stride(inlink->format, inlink->w);
    if ((props.input.stride)==XMA_NONE_FMT_TYPE)
       return XMA_ERROR;

    //Validate input resolution against MAX supported
    if ((inlink->w > MAX_INPUT_WIDTH) || //for landscape use-case
        (inlink->h > MAX_INPUT_WIDTH) || //for portrait use-case
        ((inlink->w * inlink->h) > MAX_INPUT_PIXELS)) {
        av_log (ctx, AV_LOG_ERROR, "MultiScaler Input %4dx%4d exceeds max supported resolution %4dx%4d (or %4dx%4d portrait mode)\n",
                inlink->w, inlink->h, MAX_INPUT_WIDTH, MAX_INPUT_HEIGHT, MAX_INPUT_HEIGHT, MAX_INPUT_WIDTH);
       return XMA_ERROR;
    }

    if (outlink->time_base.den > 0) {
        int fps = outlink->frame_rate.num/outlink->frame_rate.den;
        av_log(NULL, AV_LOG_DEBUG, "fps set as %d/%d=%d\n", outlink->frame_rate.num,outlink->frame_rate.den, fps);
        s->fps = fps;
        s->in_frame_rate.num = outlink->frame_rate.num;
        s->in_frame_rate.den = outlink->frame_rate.den;
    }

    props.input.framerate.numerator   = s->fps;
    props.input.framerate.denominator = 1;

    //When coeffLoad is set to 2, app expects a FilterCoeff.txt to load coefficients from
    for (n=0; n < MAX_OUTS; n++) {
        if (props.output[n].coeffLoad==2) {
            sprintf(props.input.coeffFile, "FilterCoeff.txt");
            break;
        }
    }

    //run-time parameter configuration
    strcpy(s->sc_param_name[0], "MixRate");            s->sc_params[0].name  = s->sc_param_name[0];    s->sc_params[0].type = XMA_UINT64;    s->sc_params[0].length = sizeof(s->p_mixrate_session);  s->sc_params[0].value  = &(s->p_mixrate_session);
    strcpy(s->sc_param_name[1], "latency_logging");    s->sc_params[1].name  = s->sc_param_name[1];    s->sc_params[1].type = XMA_UINT32;    s->sc_params[1].length = sizeof(s->latency_logging);    s->sc_params[1].value  = &(s->latency_logging);
    props.params           = s->sc_params;
    props.param_cnt        = MAX_PARAMS;

    //validate rate configuration params
    ret = validate_rate_config(s);
    if(ret ==-1) {
        av_log (ctx, AV_LOG_ERROR, "Multi Scaler Configuration - All outputs at half-rate not supported\n");
        return XMA_ERROR;
    }
    else if (ret ==-2) {
        av_log (ctx, AV_LOG_ERROR, "Multi Scaler Configuration -outputs rate config shall be given 'half' or 'full' only and all outputs at half rate is not supported.\n");
        return XMA_ERROR;
    }

    //determine num sessions to create
    s->num_sessions = get_num_scaler_sessions(s);

    //All-rate session includes all outputs
    s->session_nb_outputs[SC_SESSION_ALL_RATE] = s->nb_outputs;

     if (s->num_sessions > 1) {
        s->session_nb_outputs[SC_SESSION_FULL_RATE] = get_num_full_rate_outputs(s);
        // 2 sessions with half input frame-rate each
        s->fps /= 2;
        props.input.framerate.numerator     = s->fps;
        props.input.framerate.denominator   = 1;
    }
    //log session configuration
    write_session_log(s);

    //create XRM local context
    s->xrm_ctx = (xrmContext *)xrmCreateContext(XRM_API_VERSION_1);
    if (ctx == NULL) {
        av_log (ctx, AV_LOG_ERROR, "create local XRM context failed\n");
        return XMA_ERROR;
    }

    //Get XRM Reservation Id
    if (getenv("XRM_RESERVE_ID")) {
       s->xrm_reserve_id = atoi(getenv("XRM_RESERVE_ID"));
    } else {
        s->xrm_reserve_id = -1;
    }

    for (count = 0; count < s->num_sessions; ++count) {
        props.num_outputs = s->session_nb_outputs[count];

        for (chan_id = 0; chan_id < props.num_outputs; chan_id++) {
            props.output[chan_id].format         =  get_xma_format(av_get_pix_fmt(s->out_format[chan_id]));
            if ((props.output[chan_id].format)==XMA_NONE_FMT_TYPE)
               return XMA_ERROR;

            props.output[chan_id].bits_per_pixel = av_get_bits_per_pixel(av_pix_fmt_desc_get(av_get_pix_fmt(s->out_format[chan_id])));
            props.output[chan_id].width          = s->out_width[chan_id];
            props.output[chan_id].height         = s->out_height[chan_id];

            props.output[chan_id].stride         = get_stride(av_get_pix_fmt(s->out_format[chan_id]), s->out_width[chan_id]);
            if ((props.output[chan_id].stride)==XMA_NONE_FMT_TYPE)
               return XMA_ERROR;

            props.output[chan_id].coeffLoad      = 0;
            props.output[chan_id].framerate.numerator   = props.input.framerate.numerator;
            props.output[chan_id].framerate.denominator = props.input.framerate.denominator;

           if ((s->out_width[chan_id] > MAX_INPUT_WIDTH) || //for landscape use-case
                (s->out_height[chan_id] > MAX_INPUT_WIDTH) || //for portrait use-case
                ((s->out_width[chan_id] * s->out_height[chan_id] ) > MAX_INPUT_PIXELS))
           {
                av_log (ctx, AV_LOG_ERROR, "MultiScaler Output %4dx%4d exceeds max supported resolution %4dx%4d (or %4dx%4d portrait mode)\n",
                s->out_width[chan_id], s->out_height[chan_id], MAX_INPUT_WIDTH, MAX_INPUT_HEIGHT, MAX_INPUT_HEIGHT, MAX_INPUT_WIDTH);
                return XMA_ERROR;
           }
        }

        /*----------------------------------------------------
          Allocate scaler resource from XRM reserved resource
         ----------------------------------------------------*/
        s->xrm_scalres_count = count;
        if(_allocate_xrm_scaler_cu(ctx, &props) < 0) {
            av_log(ctx, AV_LOG_ERROR, "XRM_ALLOCATION: resource allocation failed\n");
            return XMA_ERROR;
        }

        s->session[count] = xma_scaler_session_create(&props);
        if (!s->session[count]) {
            av_log(ctx, AV_LOG_ERROR, "session %d creation failed.\n", count);
            return XMA_ERROR;
        }
        s->p_mixrate_session = (uint64_t)s->session[count]; //send first session handle to next session
    }
    s->session_frame = 0; //start with even frame

    return 0;
}

void xma_multiscaler_filter_flush(AVFilterLink *link)
{
    AVFilterLink *inlink = link->dst->inputs[0];
    AVFilterContext *ctx = link->dst;
    MultiScalerContext *s = ctx->priv;
    int ret = s->send_status;
    int rtt = -1;
    int count = 0;
    int *outLink = (int *)link;
    AVFrame *nframe = av_frame_alloc();

    nframe->format = AV_PIX_FMT_NV12; // sending dummy fixed format
    nframe->width  = inlink->w;
    nframe->height = inlink->h;

    /* creating dummy AVFrame */
    rtt =  av_frame_get_buffer(nframe, VCU_STRIDE_ALIGN);
    if (rtt<0) {
        av_log(ctx, AV_LOG_ERROR, "failed to create dummy AV frame\n");
        return;
    }

    if (outLink == s->copyOutLink) {
        s->flush        = 1;
        nframe->data[0] = NULL;
        nframe->data[1] = NULL;
        nframe->data[2] = NULL;

        //flush pipeline for all sessions
        for (count = 0; count < s->num_sessions; ++count) {
            while (ret != XMA_EOS) {
                multiscale_xma_filter_frame(link, nframe);
                ret = s->send_status;
            }
            //reset status for last frame in last session
            ret = XMA_SUCCESS;
        }
    }
    if (nframe->data[0])
        av_freep(&nframe->data[0]) ;
    if (nframe->data[1])
        av_freep(&nframe->data[1]);
    if (nframe->data[2])
        av_freep(&nframe->data[2]);
    av_frame_free(&nframe);
}

static int
multiscale_xma_filter_frame(AVFilterLink *link, AVFrame *in_frame)
{
    AVFilterContext *ctx = link->dst;
    MultiScalerContext *s = ctx->priv;
    XmaFrame *xframe = NULL;
    int ret = 0;
    int i = 0;
    int plane_id;
    int session_num_out = 0;
    AVFrame *a_frame_list[MAX_OUTS] = {0};
    XmaFrame *x_frame_list[MAX_OUTS] = {0};
    XmaFrameData frame_data = {0, };
    XmaFrameProperties frame_props = {0, };
    XmaScalerSession    *curr_session;
    MultiScalerSessionType  session_type;

    s->copyOutLink = (int*)link;

    if (s->num_sessions > 1) {
        //Odd Frame = SC_SESSION_FULL_RATE, Even Frame = SC_SESSION_ALL_RATE
        session_type = ((s->session_frame & 0x01) ? SC_SESSION_FULL_RATE : SC_SESSION_ALL_RATE);
        s->session_frame = (s->session_frame + 1) % SC_MAX_SESSIONS;
    } else {
        session_type = SC_SESSION_ALL_RATE;
    }
    curr_session    = s->session[session_type];
    session_num_out = s->session_nb_outputs[session_type];


    if (AV_PIX_FMT_XVBM == in_frame->format) {
        xframe = av_frame_get_xma_frame (in_frame);
        xvbm_buffer_refcnt_inc (xframe->data[0].buffer);
        xframe->pts = in_frame->pts; // Not required if previous elements packs pts
    } else {
        // Clone input frame from an AVFrame to an XmaFrame
        frame_props.format = get_xma_format(in_frame->format);
        frame_props.width  = in_frame->width;
        frame_props.height = in_frame->height;
        frame_props.linesize[0] = in_frame->linesize[0];
        frame_props.linesize[1] = in_frame->linesize[1];
        frame_props.bits_per_pixel = 8;

        for (plane_id = 0; plane_id < av_pix_fmt_count_planes (in_frame->format); plane_id++) {
            frame_data.data[plane_id] = in_frame->data[plane_id];
        }

        xframe = xma_frame_from_buffers_clone(&frame_props, &frame_data);
        xframe->pts = in_frame->pts;
    }

#ifdef DUMP_FRAME_PARAM
    av_log(NULL, AV_LOG_INFO, "MultiScaler Input : w = %d, h = %d, fmt = %d, bps = %d, pts = %lld, data[0] = %p, data[1] = %p, data[2] = %p\n",
        frame_props.width, frame_props.height, frame_props.format, frame_props.bits_per_pixel, in_frame->pts,
        frame_data.data[0], frame_data.data[1], frame_data.data[2]);
#endif

    s->send_status = xma_scaler_session_send_frame(curr_session, xframe);

    /* only receive output frame after XMA_SUCESS or XMA_FLUSH_AGAIN */
    if((s->send_status== XMA_SUCCESS) || (s->send_status == XMA_FLUSH_AGAIN)) {
        int xma_ret = XMA_SUCCESS;
        /* Create output frames */
        for (i = 0; i < session_num_out; i++) {
            XmaFrameProperties fprops;
            XmaFrameData fdata;

            ctx->outputs[i]->format = av_get_pix_fmt(s->out_format[i]);

            if (AV_PIX_FMT_XVBM == av_get_pix_fmt(s->out_format[i])) {
                a_frame_list[i] = av_frame_alloc();
                if (a_frame_list[i] == NULL) {
                    av_log (ctx, AV_LOG_ERROR, "failed to allocate memory...\n");
                    ret = AVERROR(ENOMEM);
                    goto error;
                }
                a_frame_list[i]->data[0] = NULL;

                fprops.format = get_xma_format(av_get_pix_fmt(s->out_format[i]));
                fprops.width = ctx->outputs[i]->w;
                fprops.height = ctx->outputs[i]->h;
                fprops.bits_per_pixel = av_get_bits_per_pixel(av_pix_fmt_desc_get(av_get_pix_fmt(s->out_format[i])));
                fdata.data[0] = a_frame_list[i]->data[0];
                x_frame_list[i] = xma_frame_from_buffers_clone(&fprops, &fdata);
                x_frame_list[i]->data[0].buffer_type = XMA_DEVICE_BUFFER_TYPE;
            } else {
                a_frame_list[i] = ff_get_video_buffer(ctx->outputs[i], FFALIGN(ctx->outputs[i]->w, VCU_STRIDE_ALIGN), FFALIGN(ctx->outputs[i]->h, VCU_HEIGHT_ALIGN));
                if (a_frame_list[i] == NULL) {
                    av_log (ctx, AV_LOG_ERROR, "failed to allocate output frame...\n");
                    ret = AVERROR(ENOMEM);
                    goto error;
                }

                a_frame_list[i]->width = ctx->outputs[i]->w;
                a_frame_list[i]->height = ctx->outputs[i]->h;

                fprops.format = get_xma_format(av_get_pix_fmt(s->out_format[i]));
                fprops.width = FFALIGN(ctx->outputs[i]->w, VCU_STRIDE_ALIGN);
                fprops.height = FFALIGN(ctx->outputs[i]->h, VCU_HEIGHT_ALIGN);
                fprops.bits_per_pixel = av_get_bits_per_pixel(av_pix_fmt_desc_get(av_get_pix_fmt(s->out_format[i])));

                for (plane_id = 0; plane_id < av_pix_fmt_count_planes (av_get_pix_fmt(s->out_format[i])); plane_id++) {
                    fdata.data[plane_id] = a_frame_list[i]->data[plane_id];
                }
                x_frame_list[i] = xma_frame_from_buffers_clone(&fprops, &fdata);
            }
        }

        xma_ret = xma_scaler_session_recv_frame_list(curr_session, x_frame_list);
        if (xma_ret != XMA_SUCCESS) {
            av_log (ctx, AV_LOG_ERROR, "failed to receive frame list from XMA plugin\n");
            ret = AVERROR_UNKNOWN;
            goto error;
        }

        for (i = 0; i < session_num_out; i++) {
            av_frame_copy_props(a_frame_list[i], in_frame);
            a_frame_list[i]->width = ctx->outputs[i]->w;
            a_frame_list[i]->height = ctx->outputs[i]->h;
            a_frame_list[i]->pts = x_frame_list[i]->pts;
            a_frame_list[i]->format  = av_get_pix_fmt(s->out_format[i]);

            if (AV_PIX_FMT_XVBM == av_get_pix_fmt(s->out_format[i])) {
                a_frame_list[i]->linesize[0] = a_frame_list[i]->linesize[1] = ALIGN(a_frame_list[i]->width, VCU_STRIDE_ALIGN);
                ret = av_frame_clone_xma_frame (a_frame_list[i], x_frame_list[i]);
                if (ret)
                    goto error;
            } else {
                //set the stride
                for (plane_id = 0; plane_id < av_pix_fmt_count_planes (av_get_pix_fmt(s->out_format[i])); plane_id++)
                    a_frame_list[i]->linesize[plane_id] = x_frame_list[i]->frame_props.width;
            }

#ifdef DUMP_OUT_FRAMES
            {
              int written = fwrite (a_frame_list[i]->data[0], 1, (2048*1088*3)>>1, outfp);
              av_log(NULL, AV_LOG_INFO, "written %d bytes\n", written);
              //written = fwrite (a_frame_list[i]->data[1], 1, (2048*1080) >> 1, outfp);
              //printf ("written %d bytes\n", written);
            }
#endif
#ifdef DUMP_FRAME_PARAM
            av_log(NULL, AV_LOG_INFO,  "Output[%d] : w = %d, h = %d, fmt = %d, pts = %lld, linesize[0] = %d,"
                "linesize[1] = %d, linesize[2] = %d, data[0] = %p, data[1]= %p, data[2] = %p\n",
                i, a_frame_list[i]->width, a_frame_list[i]->height, a_frame_list[i]->format, a_frame_list[i]->pts,
                a_frame_list[i]->linesize[0], a_frame_list[i]->linesize[1], a_frame_list[i]->linesize[2],
                a_frame_list[i]->data[0], a_frame_list[i]->data[1], a_frame_list[i]->data[2]);
#endif

            ret = ff_filter_frame(ctx->outputs[i], a_frame_list[i]);
            if (ret < 0) {
                av_log(ctx, AV_LOG_ERROR, "ff_filter_frame failed: ret=%d\n", ret);
                goto error;
            }
            xma_frame_free(x_frame_list[i]);
        }
        s->frames_out++;
    }

    xma_frame_free (xframe);
    if (s->flush == 0)
        av_frame_free(&in_frame);

    return 0;

error:
    if (xframe)
        xma_frame_free (xframe);

    if (s->flush == 0)
        av_frame_free(&in_frame);

    for (i = 0; i < session_num_out; i++)
        if (x_frame_list[i])
            xma_frame_free(x_frame_list[i]);

    return mpsoc_report_error(s, "multiscaler filter_frame failed", ret);
}

static int query_formats(AVFilterContext *ctx)
{
    static const enum AVPixelFormat pix_fmts[] = {
        AV_PIX_FMT_XVBM,
        AV_PIX_FMT_NV12, // 25
        AV_PIX_FMT_NONE,
    };
    int res, chan_id;
    MultiScalerContext *s = ctx->priv;
    AVFilterFormats *formats = ff_make_format_list(pix_fmts);

    if (!formats)
        return AVERROR(ENOMEM);
    if (av_get_pix_fmt(s->out_format[0]) == AV_PIX_FMT_NONE)
        return ff_set_common_formats(ctx, formats);
    res = ff_formats_ref(formats, &ctx->inputs[0]->out_formats);
    if (res < 0)
        return res;

    for (chan_id = 0; chan_id < s->nb_outputs; chan_id++) {
        formats = NULL;
        ff_add_format(&formats, av_get_pix_fmt(s->out_format[chan_id]));
        res = ff_formats_ref(formats, &ctx->outputs[chan_id]->in_formats);
        if (res < 0)
            return res;
    }
    return res;
}

#define OFFSET(x) offsetof(MultiScalerContext, x)
#define FLAGS (AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_FILTERING_PARAM)
static const AVOption options[] = {
    { "outputs", "set number of outputs", OFFSET(nb_outputs), AV_OPT_TYPE_INT, { .i64 = 8 }, 1, MAX_OUTS, FLAGS },
    { "out_1_width", "set width of output 1 (should be multiple of 4)", OFFSET(out_width[0]), AV_OPT_TYPE_INT, { .i64 = 1600 }, 128, 3840, FLAGS },
    { "out_1_height", "set height of output 1 (should be multiple of 4)", OFFSET(out_height[0]), AV_OPT_TYPE_INT, { .i64 = 900 }, 128, 3840, FLAGS },
    { "out_1_pix_fmt", "set format of output 1", OFFSET(out_format[0]), AV_OPT_TYPE_STRING, { .str = "xlnx_xvbm"}, CHAR_MIN, CHAR_MAX, FLAGS },
    { "out_1_rate", "set rate of output 1", OFFSET(out_rate[0]), AV_OPT_TYPE_STRING, { .str = "full" }, CHAR_MIN, CHAR_MAX, FLAGS},
    { "out_2_width", "set width of output 2 (should be multiple of 4)", OFFSET(out_width[1]), AV_OPT_TYPE_INT, { .i64 = 1280 }, 128, 3840, FLAGS },
    { "out_2_height", "set height of output 2 (should be multiple of 4)", OFFSET(out_height[1]), AV_OPT_TYPE_INT, { .i64 = 720 }, 128, 3840, FLAGS },
    { "out_2_pix_fmt", "set format of output 2", OFFSET(out_format[1]), AV_OPT_TYPE_STRING, { .str = "xlnx_xvbm"}, CHAR_MIN, CHAR_MAX, FLAGS },
    { "out_2_rate", "set rate of output 2", OFFSET(out_rate[1]), AV_OPT_TYPE_STRING, { .str = "full" }, CHAR_MIN, CHAR_MAX, FLAGS},
    { "out_3_width", "set width of output 3 (should be multiple of 4)", OFFSET(out_width[2]), AV_OPT_TYPE_INT, { .i64 = 800 }, 128, 3840, FLAGS },
    { "out_3_height", "set height of output 3 (should be multiple of 4)", OFFSET(out_height[2]), AV_OPT_TYPE_INT, { .i64 = 600 }, 128, 3840, FLAGS },
    { "out_3_pix_fmt", "set format of output 3", OFFSET(out_format[2]), AV_OPT_TYPE_STRING, { .str = "xlnx_xvbm"}, CHAR_MIN, CHAR_MAX, FLAGS },
    { "out_3_rate", "set rate of output 3", OFFSET(out_rate[2]), AV_OPT_TYPE_STRING, { .str = "full" }, CHAR_MIN, CHAR_MAX, FLAGS},
    { "out_4_width", "set width of output 4 (should be multiple of 4)", OFFSET(out_width[3]), AV_OPT_TYPE_INT, { .i64 = 832 }, 128, 3840, FLAGS },
    { "out_4_height", "set height of output 4 (should be multiple of 4)", OFFSET(out_height[3]), AV_OPT_TYPE_INT, { .i64 = 480 }, 128, 3840, FLAGS },
    { "out_4_pix_fmt", "set format of output 4", OFFSET(out_format[3]), AV_OPT_TYPE_STRING, { .str = "xlnx_xvbm"}, CHAR_MIN, CHAR_MAX, FLAGS },
    { "out_4_rate", "set rate of output 4", OFFSET(out_rate[3]), AV_OPT_TYPE_STRING, { .str = "full" }, CHAR_MIN, CHAR_MAX, FLAGS},
    { "out_5_width", "set width of output 5 (should be multiple of 4)", OFFSET(out_width[4]), AV_OPT_TYPE_INT, { .i64 = 640 }, 128, 3840, FLAGS },
    { "out_5_height", "set height of output 5 (should be multiple of 4)", OFFSET(out_height[4]), AV_OPT_TYPE_INT, { .i64 = 480 }, 128, 3840, FLAGS },
    { "out_5_pix_fmt", "set format of output 5", OFFSET(out_format[4]), AV_OPT_TYPE_STRING, { .str = "xlnx_xvbm"}, CHAR_MIN, CHAR_MAX, FLAGS },
    { "out_5_rate", "set rate of output 5", OFFSET(out_rate[4]), AV_OPT_TYPE_STRING, { .str = "full" }, CHAR_MIN, CHAR_MAX, FLAGS},
    { "out_6_width", "set width of output 6 (should be multiple of 4)", OFFSET(out_width[5]), AV_OPT_TYPE_INT, { .i64 = 480 }, 128, 3840, FLAGS },
    { "out_6_height", "set height of output 6 (should be multiple of 4)", OFFSET(out_height[5]), AV_OPT_TYPE_INT, { .i64 = 320 }, 128, 3840, FLAGS },
    { "out_6_pix_fmt", "set format of output 6", OFFSET(out_format[5]), AV_OPT_TYPE_STRING, { .str = "xlnx_xvbm"}, CHAR_MIN, CHAR_MAX, FLAGS },
    { "out_6_rate", "set rate of output 6", OFFSET(out_rate[5]), AV_OPT_TYPE_STRING, { .str = "full" }, CHAR_MIN, CHAR_MAX, FLAGS},
    { "out_7_width", "set width of output 7 (should be multiple of 4)", OFFSET(out_width[6]), AV_OPT_TYPE_INT, { .i64 = 320 }, 128, 3840, FLAGS },
    { "out_7_height", "set height of output 7 (should be multiple of 4)", OFFSET(out_height[6]), AV_OPT_TYPE_INT, { .i64 = 240 }, 128, 3840, FLAGS },
    { "out_7_pix_fmt", "set format of output 7", OFFSET(out_format[6]), AV_OPT_TYPE_STRING, { .str = "xlnx_xvbm"}, CHAR_MIN, CHAR_MAX, FLAGS },
    { "out_7_rate", "set rate of output 7", OFFSET(out_rate[6]), AV_OPT_TYPE_STRING, { .str = "full" }, CHAR_MIN, CHAR_MAX, FLAGS},
    { "out_8_width", "set width of output 8 (should be multiple of 4)", OFFSET(out_width[7]), AV_OPT_TYPE_INT, { .i64 = 224 }, 128, 3840, FLAGS },
    { "out_8_height", "set height of output 8 (should be multiple of 4)", OFFSET(out_height[7]), AV_OPT_TYPE_INT, { .i64 = 224 }, 128, 3840, FLAGS },
    { "out_8_pix_fmt", "set format of output 8", OFFSET(out_format[7]), AV_OPT_TYPE_STRING, { .str = "xlnx_xvbm"}, CHAR_MIN, CHAR_MAX, FLAGS },
    { "out_8_rate", "set rate of output 8", OFFSET(out_rate[7]), AV_OPT_TYPE_STRING, { .str = "full" }, CHAR_MIN, CHAR_MAX, FLAGS},
    { "latency_logging", "Log latency information to syslog", OFFSET(latency_logging), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 1, FLAGS, "latency_logging" },
    { NULL }
};

#define multiscale_xma_options options
AVFILTER_DEFINE_CLASS(multiscale_xma);

static const AVFilterPad avfilter_vf_multiscale_xma_inputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_VIDEO,
        .filter_frame = multiscale_xma_filter_frame,
        .config_props = multiscale_xma_config_props,
    },
    { NULL }
};

AVFilter ff_vf_multiscale_xma = {
    .name = "multiscale_xma",
    .description = NULL_IF_CONFIG_SMALL("Xilinx Multi Scaler (in ABR mode) using XMA APIs"),
    .priv_size = sizeof(MultiScalerContext),
    .priv_class = &multiscale_xma_class,
    .query_formats = query_formats,
    .init = multiscale_xma_init,
    .uninit = multiscale_xma_uninit,
    .inputs = avfilter_vf_multiscale_xma_inputs,
    .outputs = NULL,
    .flags = AVFILTER_FLAG_DYNAMIC_OUTPUTS,
};

