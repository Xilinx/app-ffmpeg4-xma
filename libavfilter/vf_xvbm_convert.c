/*
 * Copyright (c) 2020 Xilinx
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
 * Xilinx Video Buffer Manager frame format to AV Frame format converter
 */
#include <sys/prctl.h>
#include <xma.h>
#include <xvbm.h>
#include <pthread.h>
#include <libavutil/threadmessage.h>
#include "libavutil/pixfmt.h"
#include "libavutil/fifo.h"
#include "libavutil/time.h"
#include "avfilter.h"
#include "formats.h"
#include "internal.h"


#define MAX_REQ_MSGQ_SIZE     20
#define MAX_RSP_MSGQ_SIZE     20

typedef enum _XVBM_DMA_STATE {
    XVBM_DMA_REQ_NEW = 0,
    XVBM_DMA_REQ_PROCESSING,
    XVBM_DMA_REQ_DONE,
    XVBM_DMA_REQ_FLUSH,
    XVBM_DMA_REQ_FLUSH_COMPLETE,
    XVBM_DMA_REQ_END
}XVBM_DMA_STATE;


typedef struct _XVBM_CONV_REQ_MSG {
    AVFrame         *pFrame;
    XVBM_DMA_STATE  state;
} XVBM_CONV_REQ_MSG;

typedef struct _XVBM_CONV_RSP_MSG {
    AVFrame         *pFrame;
    XVBM_DMA_STATE  state;
} XVBM_CONV_RSP_MSG;

typedef struct XvbmConvertContext {
    pthread_t             thread;
    AVThreadMessageQueue  *ReqMsgQ;
    AVThreadMessageQueue  *RspMsgQ;
    AVFilterLink          *xvbm_filterLink;
}XvbmConvertContext;

static int xvbm_convert_filter_frame(AVFilterLink *link, AVFrame *in);
static enum AVPixelFormat xvbm_conv_get_av_format(XmaFormatType xmaFormat);
static size_t xvbm_conv_get_plane_size(int32_t       width,
                                       int32_t       height,
                                       XmaFormatType format,
                                       int32_t       plane_id);
static void* xvbm_conv_thread(void *xvbmConvCtx);
static AVFrame* conv_xmaframe2avframe(AVFrame *frame_in);

static enum AVPixelFormat xvbm_conv_get_av_format(XmaFormatType xmaFormat)
{
    enum AVPixelFormat avformat;

    switch(xmaFormat) {
        case XMA_YUV420_FMT_TYPE:    avformat = AV_PIX_FMT_YUV420P; break;
        case XMA_YUV422_FMT_TYPE:    avformat = AV_PIX_FMT_YUV422P; break;
        case XMA_YUV444_FMT_TYPE:    avformat = AV_PIX_FMT_YUV444P; break;
        case XMA_RGBP_FMT_TYPE:      avformat = AV_PIX_FMT_GBRP;    break;
        case XMA_VCU_NV12_FMT_TYPE:  avformat = AV_PIX_FMT_NV12;    break;
        default:                     avformat = AV_PIX_FMT_NONE;    break;
    }
    return(avformat);
}

static size_t xvbm_conv_get_plane_size(int32_t       width,
                                       int32_t       height,
                                       XmaFormatType format,
                                       int32_t       plane_id)
{
    size_t p_size;

    switch (format) {
        case XMA_YUV420_FMT_TYPE:
            switch(plane_id) {
                case 0:  p_size = width * height;        break;
                case 1:  p_size = ((width * height)>>2); break;
                case 2:  p_size = ((width * height)>>2); break;
                default: p_size = 0;                     break;
            }
            break;

        case XMA_YUV422_FMT_TYPE:
            switch(plane_id) {
                case 0:  p_size = width * height;        break;
                case 1:  p_size = ((width * height)>>1); break;
                case 2:  p_size = ((width * height)>>1); break;
                default: p_size = 0;                     break;
            }
            break;

        case XMA_YUV444_FMT_TYPE:
        case XMA_RGBP_FMT_TYPE:
            p_size = (width * height);
            break;

        default:
              av_log(NULL, AV_LOG_ERROR, "xvbm_conv:: Unsupported format...\n");
              p_size = 0;
              break;
    }
    return(p_size);
}

static AVFrame* conv_xmaframe2avframe(AVFrame *in)
{
    XmaFrame *xframe  = NULL;
    AVFrame *out;
    uint8_t *hostbuf;
    int ret;
    int aw, ah;
    uint8_t plane_id;
    size_t  size;
    int isframeType_vcu_nv12 = 0;

    out = av_frame_alloc();
    if (!out) {
        av_log(NULL, AV_LOG_ERROR, "xvbm_conv:: unable to allocate AVFrame\n");
        return NULL;
    }
    ret = av_frame_copy_props(out, in);
    if (ret < 0) {
        av_frame_free(&out);
        av_log(NULL, AV_LOG_ERROR, "xvbm_conv:: unable to copy AVFrame properties (%d)\n", ret);
        return NULL;
    }

    xframe = in->data[0];
    if (!xframe) {
        av_log(NULL, AV_LOG_ERROR, "xvbm_conv:: Invalid input frame\n", ret);
        av_frame_free(&out);
        return NULL;
    }

    out->format = xvbm_conv_get_av_format(xframe->frame_props.format);
    out->width  = xframe->frame_props.width;
    out->height = xframe->frame_props.height;

    isframeType_vcu_nv12 = (XMA_VCU_NV12_FMT_TYPE == xframe->frame_props.format);
    if (isframeType_vcu_nv12) {
        //host buffer alocation needs to be aligned to VCU frame specs
        aw = FFALIGN(in->width, 256);
        ah = FFALIGN(in->height, 64);

        out->buf[0]  = av_buffer_alloc(aw*ah);
        out->buf[1]  = av_buffer_alloc((aw*ah)/2);
        if ((out->buf[0] == NULL) ||(out->buf[1] == NULL)) {
            av_frame_free(&out);
            av_log(NULL, AV_LOG_ERROR, "xvbm_conv:: Out of memory\n");
            return NULL;
        }
        out->data[0] = out->buf[0]->data;
        out->data[1] = out->buf[1]->data;

        out->linesize[0] = aw;
        out->linesize[1] = aw;
    } else {
        out->linesize[0] = xframe->frame_props.width * ((xframe->frame_props.bits_per_pixel + 7) >> 3);
        switch(xframe->frame_props.format)
        {
            case XMA_YUV420_FMT_TYPE:
            case XMA_YUV422_FMT_TYPE:
                {
                    int div_factor = ((XMA_YUV422_FMT_TYPE == xframe->frame_props.format) ? 2 : 4);
                    out->buf[0] = av_buffer_alloc(in->width*in->height);
                    out->buf[1] = av_buffer_alloc((in->width*in->height)/div_factor);
                    out->buf[2] = av_buffer_alloc((in->width*in->height)/div_factor);
                    out->linesize[1] = out->linesize[0] / 2;
                    out->linesize[2] = out->linesize[1];
                }
                break;

            case XMA_YUV444_FMT_TYPE:
            case XMA_RGBP_FMT_TYPE:
                out->buf[0] = av_buffer_alloc (in->width*in->height);
                out->buf[1] = av_buffer_alloc (in->width*in->height);
                out->buf[2] = av_buffer_alloc (in->width*in->height);
                out->linesize[1] = out->linesize[0];
                out->linesize[2] = out->linesize[1];
                break;

           default:
                av_frame_free(&out);
                av_log(NULL, AV_LOG_ERROR, "xvbm_conv:: Unsupported format...\n");
                return NULL;
        }
        if ((out->buf[0] == NULL) ||(out->buf[1] == NULL) || (out->buf[2] == NULL)) {
            av_frame_free(&out);
            av_log(NULL, AV_LOG_ERROR, "xvbm_conv:: Out of memory\n");
            return NULL;
        }
        out->data[0] = out->buf[0]->data;
        out->data[1] = out->buf[1]->data;
        out->data[2] = out->buf[2]->data;
    }
    //DMA device buffer to host
    if (isframeType_vcu_nv12) { //[special case]: xvbm single buffer contains Y+UV data
        size = aw*ah;
        hostbuf = (uint8_t *)xvbm_buffer_get_host_ptr(xframe->data[0].buffer);
        ret = xvbm_buffer_read(xframe->data[0].buffer, hostbuf, (size+size/2), 0); //read Y+U/V plane data
        if (ret) {
            av_frame_free(&out);
            av_log(NULL, AV_LOG_ERROR, "xvbm_conv:: xvbm_buffer_read failed\n");
            return(NULL);
        }
        memcpy (out->data[0], hostbuf,        size);   //extract Y Plane
        memcpy (out->data[1], (hostbuf+size), size/2); //extract U/V Plane
    } else {
        //Planar Buffers
        for (plane_id = 0; plane_id < xma_frame_planes_get(&xframe->frame_props); plane_id++) {
            size = xvbm_conv_get_plane_size(out->width, out->height, xframe->frame_props.format, plane_id);
            hostbuf = (uint8_t *)xvbm_buffer_get_host_ptr(xframe->data[plane_id].buffer);
            ret = xvbm_buffer_read(xframe->data[plane_id].buffer, hostbuf, size, 0);
            if (ret) {
                av_frame_free(&out);
                av_log(NULL, AV_LOG_ERROR, "xvbm_conv:: xvbm_buffer_read failed\n");
                return(NULL);
            }
            memcpy (out->data[plane_id], hostbuf, size);
        }
    }
    return out;
}


static void* xvbm_conv_thread(void *xvbmConvCtx)
{
    XvbmConvertContext *ctx = (XvbmConvertContext *)xvbmConvCtx;
    XVBM_CONV_REQ_MSG reqMsg;
    XVBM_CONV_RSP_MSG rspMsg;

    av_log(NULL, AV_LOG_DEBUG, "xvbm_conv:: Starting xvbm_conv thread\n");
    prctl(PR_SET_NAME, "xvbm_thread");

    while (1) {
        av_thread_message_queue_recv(ctx->ReqMsgQ, &reqMsg, 0);

        switch (reqMsg.state) {
            case XVBM_DMA_REQ_NEW:
                //Initiate DMA Tx
                rspMsg.state  = XVBM_DMA_REQ_PROCESSING;
                rspMsg.pFrame = conv_xmaframe2avframe(reqMsg.pFrame);
                av_frame_free(&reqMsg.pFrame);
                //DMA Tx complete - send response
                rspMsg.state  = XVBM_DMA_REQ_DONE;
                av_thread_message_queue_send(ctx->RspMsgQ, &rspMsg, 0);
                break;

            case XVBM_DMA_REQ_FLUSH:
                rspMsg.state = XVBM_DMA_REQ_FLUSH_COMPLETE;
                av_thread_message_queue_send(ctx->RspMsgQ, &rspMsg, 0);
                break;

            case XVBM_DMA_REQ_END:
                goto exit;
        }
    }
exit:
    av_log(NULL, AV_LOG_DEBUG, "xvbm_conv:: Exiting xvbm_conv thread\n");

    return NULL;
}

void xvbm_convert_filter_flush(AVFilterLink *link)
{
    AVFilterContext *ctx  = link->dst;
    AVFilterLink *outlink = ctx->outputs[0];
    XvbmConvertContext *s = ctx->priv;
    XVBM_CONV_REQ_MSG reqMsg;
    XVBM_CONV_RSP_MSG rspMsg;
    AVFrame *out;
    int ret;

   if (link == s->xvbm_filterLink) {
        //send flush request to thread
        reqMsg.state = XVBM_DMA_REQ_FLUSH;
        av_thread_message_queue_send(s->ReqMsgQ, &reqMsg, 0);

        do {
            ret = av_thread_message_queue_recv(s->RspMsgQ, &rspMsg, 0); //blocking call
            if(rspMsg.state != XVBM_DMA_REQ_FLUSH_COMPLETE) {
                if(!rspMsg.pFrame) {
                    av_log(ctx, AV_LOG_ERROR, "xvbm_conv:: conversion failed\n");
                    return;
                }
                out = rspMsg.pFrame;
                ret = ff_filter_frame(outlink, out);
                if (ret < 0) {
                    av_log(NULL, AV_LOG_ERROR, "%s():: ff_filter_frame failed: ret=%d\n", __func__,ret);
                    return;
                }
            }
        } while(rspMsg.state != XVBM_DMA_REQ_FLUSH_COMPLETE);
   } else {
       av_log(NULL, AV_LOG_ERROR, "%s():: filterlink mismatch (ctx: %p   in: %p)\n", __func__,s->xvbm_filterLink, link);
   }
}

static int xvbm_convert_filter_frame(AVFilterLink *link, AVFrame *in)
{
    AVFilterContext *ctx  = link->dst;
    AVFilterLink *outlink = ctx->outputs[0];
    XvbmConvertContext *s = ctx->priv;
    XVBM_CONV_REQ_MSG reqMsg;
    XVBM_CONV_RSP_MSG rspMsg;
    AVFrame *out;
    int ret;

    s->xvbm_filterLink = link;

    if (AV_PIX_FMT_XVBM == in->format) {
        reqMsg.pFrame = in;
        reqMsg.state  = XVBM_DMA_REQ_NEW;
        av_thread_message_queue_send(s->ReqMsgQ, &reqMsg, 0);
        ret = av_thread_message_queue_recv(s->RspMsgQ, &rspMsg, AV_THREAD_MESSAGE_NONBLOCK);
        if (ret == AVERROR(EAGAIN)) {
            av_log(ctx, AV_LOG_INFO, "xvbm_conv:: wait for conversion to finish...\n");
            return 0;
        }

        if(!rspMsg.pFrame) {
            av_log(ctx, AV_LOG_ERROR, "xvbm_conv:: conversion failed\n");
            return AVERROR_EXIT;
        }
        out = rspMsg.pFrame;
    } else {
        //clone input frame to output
        out = in;
    }
    ret = ff_filter_frame(outlink, out);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "%s():: ff_filter_frame failed: ret=%d\n", __func__,ret);
        return ret;
    }
    return 0;
}

static int xvbm_convert_query_formats(AVFilterContext *ctx)
{
    static const enum AVPixelFormat in_fmts[] = {
        AV_PIX_FMT_XVBM,
        AV_PIX_FMT_NONE
    };

    static const enum AVPixelFormat out_fmts[] = {
        AV_PIX_FMT_NV12,
        AV_PIX_FMT_NONE
    };

    int ret;
    AVFilterFormats *inpix_formats  = ff_make_format_list(in_fmts);
    AVFilterFormats *outpix_formats = ff_make_format_list(out_fmts);

    if (!inpix_formats || !outpix_formats)
        return AVERROR(ENOMEM);

    if(((ret = ff_formats_ref(inpix_formats,  &ctx->inputs[0]->out_formats)) < 0) ||
       ((ret = ff_formats_ref(outpix_formats, &ctx->outputs[0]->in_formats)) < 0))
       goto fail;

    return 0;
fail:
    if(inpix_formats)
        av_freep(&inpix_formats->formats);
    if(outpix_formats)
        av_freep(&outpix_formats->formats);

    av_freep(&inpix_formats);
    av_freep(&outpix_formats);
    return ret;
}

static int config_props(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->dst;
    AVFilterLink *inlink = outlink->src->inputs[0];

    if (inlink->w % 2 || inlink->h % 2) {
        av_log(ctx, AV_LOG_ERROR, "Invalid odd size (%dx%d)\n",
               inlink->w, inlink->h);
        return AVERROR_EXIT;
    }

    outlink->w = inlink->w;
    outlink->h = inlink->h;
    outlink->sample_aspect_ratio = inlink->sample_aspect_ratio;
    outlink->time_base = inlink->time_base;

    return 0;
}

static av_cold int xvbm_conv_init(AVFilterContext *ctx)
{
    XvbmConvertContext *xc = ctx->priv;
    int ret;

    ret  = av_thread_message_queue_alloc(&xc->ReqMsgQ, MAX_REQ_MSGQ_SIZE, sizeof(XVBM_CONV_REQ_MSG));
    ret |= av_thread_message_queue_alloc(&xc->RspMsgQ, MAX_RSP_MSGQ_SIZE, sizeof(XVBM_CONV_RSP_MSG));
    if (ret < 0) {
        av_log(ctx, AV_LOG_ERROR, "xvbm_conv:: Failed to allocate message queue\n");
        exit(1);
    }

    av_log(ctx, AV_LOG_DEBUG, "xvbm_conv:: Creating xvbm_conv thread\n");
    ret = pthread_create(&xc->thread, NULL, xvbm_conv_thread, xc);
    if (ret) {
        av_log(ctx, AV_LOG_ERROR, "pthread_create failed : %s\n", av_err2str(ret));
        exit(1);
    }

    return 0;
}

static av_cold void xvbm_conv_uninit(AVFilterContext *ctx)
{
    int ret;
    XvbmConvertContext *xc = ctx->priv;
    XVBM_CONV_REQ_MSG reqMsg;

    //trigger thread exit
    reqMsg.state = XVBM_DMA_REQ_END;
    av_thread_message_queue_send(xc->ReqMsgQ, &reqMsg, 0);

    //join with ffmpeg thread
    ret = pthread_join(xc->thread, NULL);
    if (ret != 0) {
        av_log(ctx, AV_LOG_ERROR, "pthread_join failed : %s\n", av_err2str(ret));
    }
    //free queues
    XVBM_CONV_RSP_MSG rspMsg;
    while (av_thread_message_queue_nb_elems(xc->ReqMsgQ)) {
        av_thread_message_queue_recv(xc->ReqMsgQ, &reqMsg, 0);
        av_frame_free(&reqMsg.pFrame);
    }
    while (av_thread_message_queue_nb_elems(xc->RspMsgQ)) {
        av_thread_message_queue_recv(xc->RspMsgQ, &rspMsg, 0);
        av_frame_free(&rspMsg.pFrame);
    }
    av_thread_message_queue_free(&xc->ReqMsgQ);
    av_thread_message_queue_free(&xc->RspMsgQ);
}


static const AVFilterPad inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = xvbm_convert_filter_frame,
    },
    { NULL }
};

static const AVFilterPad outputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = config_props,
    },
    { NULL }
};

AVFilter ff_vf_xvbm_convert = {
    .name            = "xvbm_convert",
    .description     = NULL_IF_CONFIG_SMALL("convert xvbm frame to av frame"),
    .priv_size       = sizeof(XvbmConvertContext),
    .query_formats   = xvbm_convert_query_formats,
    .init            = xvbm_conv_init,
    .uninit          = xvbm_conv_uninit,
    .inputs          = inputs,
    .outputs         = outputs,
};
