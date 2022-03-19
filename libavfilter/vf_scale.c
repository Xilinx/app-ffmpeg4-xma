/*
 * Copyright (c) 2007 Bobby Bingham
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

/**
 * @file
 * scale video filter
 */

#include <stdio.h>
#include <string.h>

#include "avfilter.h"
#include "formats.h"
#include "internal.h"
#include "scale_eval.h"
#include "video.h"
#include "libavutil/avstring.h"
#include "libavutil/eval.h"
#include "libavutil/internal.h"
#include "libavutil/mathematics.h"
#include "libavutil/opt.h"
#include "libavutil/parseutils.h"
#include "libavutil/pixdesc.h"
#include "libavutil/imgutils.h"
#include "libavutil/avassert.h"
#include "libswscale/swscale.h"

static const char *const var_names[] = {
    "in_w",   "iw",
    "in_h",   "ih",
    "out_w",  "ow",
    "out_h",  "oh",
    "a",
    "sar",
    "dar",
    "hsub",
    "vsub",
    "ohsub",
    "ovsub",
    "n",
    "t",
    "pos",
    "main_w",
    "main_h",
    "main_a",
    "main_sar",
    "main_dar", "mdar",
    "main_hsub",
    "main_vsub",
    "main_n",
    "main_t",
    "main_pos",
    NULL
};

enum var_name {
    VAR_IN_W,   VAR_IW,
    VAR_IN_H,   VAR_IH,
    VAR_OUT_W,  VAR_OW,
    VAR_OUT_H,  VAR_OH,
    VAR_A,
    VAR_SAR,
    VAR_DAR,
    VAR_HSUB,
    VAR_VSUB,
    VAR_OHSUB,
    VAR_OVSUB,
    VAR_N,
    VAR_T,
    VAR_POS,
    VAR_S2R_MAIN_W,
    VAR_S2R_MAIN_H,
    VAR_S2R_MAIN_A,
    VAR_S2R_MAIN_SAR,
    VAR_S2R_MAIN_DAR, VAR_S2R_MDAR,
    VAR_S2R_MAIN_HSUB,
    VAR_S2R_MAIN_VSUB,
    VAR_S2R_MAIN_N,
    VAR_S2R_MAIN_T,
    VAR_S2R_MAIN_POS,
    VARS_NB
};

enum EvalMode {
    EVAL_MODE_INIT,
    EVAL_MODE_FRAME,
    EVAL_MODE_NB
};

typedef struct ScaleContext {
    const AVClass *class;
    struct SwsContext *sws;     ///< software scaler context
    struct SwsContext *isws[2]; ///< software scaler context for interlaced material
    AVDictionary *opts;

    /**
     * New dimensions. Special values are:
     *   0 = original width/height
     *  -1 = keep original aspect
     *  -N = try to keep aspect but make sure it is divisible by N
     */
    int w, h;
    char *size_str;
    unsigned int flags;         ///sws flags
    double param[2];            // sws params

    int hsub, vsub;             ///< chroma subsampling
    int slice_y;                ///< top of current output slice
    int input_is_pal;           ///< set to 1 if the input format is paletted
    int output_is_pal;          ///< set to 1 if the output format is paletted
    int interlaced;

    char *w_expr;               ///< width  expression string
    char *h_expr;               ///< height expression string
    AVExpr *w_pexpr;
    AVExpr *h_pexpr;
    double var_values[VARS_NB];

    char *flags_str;

    char *in_color_matrix;
    char *out_color_matrix;

    int in_range;
    int out_range;

    int out_h_chr_pos;
    int out_v_chr_pos;
    int in_h_chr_pos;
    int in_v_chr_pos;

    int force_original_aspect_ratio;
    int force_divisible_by;

    int nb_slices;

    int eval_mode;              ///< expression evaluation mode

    AVFrame *temp_frame[2];
} ScaleContext;

AVFilter ff_vf_scale2ref;

static int config_props(AVFilterLink *outlink);

static int check_exprs(AVFilterContext *ctx)
{
    ScaleContext *scale = ctx->priv;
    unsigned vars_w[VARS_NB] = { 0 }, vars_h[VARS_NB] = { 0 };

    if (!scale->w_pexpr && !scale->h_pexpr)
        return AVERROR(EINVAL);

    if (scale->w_pexpr)
        av_expr_count_vars(scale->w_pexpr, vars_w, VARS_NB);
    if (scale->h_pexpr)
        av_expr_count_vars(scale->h_pexpr, vars_h, VARS_NB);

    if (vars_w[VAR_OUT_W] || vars_w[VAR_OW]) {
        av_log(ctx, AV_LOG_ERROR, "Width expression cannot be self-referencing: '%s'.\n", scale->w_expr);
        return AVERROR(EINVAL);
    }

    if (vars_h[VAR_OUT_H] || vars_h[VAR_OH]) {
        av_log(ctx, AV_LOG_ERROR, "Height expression cannot be self-referencing: '%s'.\n", scale->h_expr);
        return AVERROR(EINVAL);
    }

    if ((vars_w[VAR_OUT_H] || vars_w[VAR_OH]) &&
        (vars_h[VAR_OUT_W] || vars_h[VAR_OW])) {
        av_log(ctx, AV_LOG_WARNING, "Circular references detected for width '%s' and height '%s' - possibly invalid.\n", scale->w_expr, scale->h_expr);
    }

    if (ctx->filter != &ff_vf_scale2ref &&
        (vars_w[VAR_S2R_MAIN_W]    || vars_h[VAR_S2R_MAIN_W]    ||
         vars_w[VAR_S2R_MAIN_H]    || vars_h[VAR_S2R_MAIN_H]    ||
         vars_w[VAR_S2R_MAIN_A]    || vars_h[VAR_S2R_MAIN_A]    ||
         vars_w[VAR_S2R_MAIN_SAR]  || vars_h[VAR_S2R_MAIN_SAR]  ||
         vars_w[VAR_S2R_MAIN_DAR]  || vars_h[VAR_S2R_MAIN_DAR]  ||
         vars_w[VAR_S2R_MDAR]      || vars_h[VAR_S2R_MDAR]      ||
         vars_w[VAR_S2R_MAIN_HSUB] || vars_h[VAR_S2R_MAIN_HSUB] ||
         vars_w[VAR_S2R_MAIN_VSUB] || vars_h[VAR_S2R_MAIN_VSUB] ||
         vars_w[VAR_S2R_MAIN_N]    || vars_h[VAR_S2R_MAIN_N]    ||
         vars_w[VAR_S2R_MAIN_T]    || vars_h[VAR_S2R_MAIN_T]    ||
         vars_w[VAR_S2R_MAIN_POS]  || vars_h[VAR_S2R_MAIN_POS]) ) {
        av_log(ctx, AV_LOG_ERROR, "Expressions with scale2ref variables are not valid in scale filter.\n");
        return AVERROR(EINVAL);
    }

    if (scale->eval_mode == EVAL_MODE_INIT &&
        (vars_w[VAR_N]            || vars_h[VAR_N]           ||
         vars_w[VAR_T]            || vars_h[VAR_T]           ||
         vars_w[VAR_POS]          || vars_h[VAR_POS]         ||
         vars_w[VAR_S2R_MAIN_N]   || vars_h[VAR_S2R_MAIN_N]  ||
         vars_w[VAR_S2R_MAIN_T]   || vars_h[VAR_S2R_MAIN_T]  ||
         vars_w[VAR_S2R_MAIN_POS] || vars_h[VAR_S2R_MAIN_POS]) ) {
        av_log(ctx, AV_LOG_ERROR, "Expressions with frame variables 'n', 't', 'pos' are not valid in init eval_mode.\n");
        return AVERROR(EINVAL);
    }

    return 0;
}

static int scale_parse_expr(AVFilterContext *ctx, char *str_expr, AVExpr **pexpr_ptr, const char *var, const char *args)
{
    ScaleContext *scale = ctx->priv;
    int ret, is_inited = 0;
    char *old_str_expr = NULL;
    AVExpr *old_pexpr = NULL;

    if (str_expr) {
        old_str_expr = av_strdup(str_expr);
        if (!old_str_expr)
            return AVERROR(ENOMEM);
        av_opt_set(scale, var, args, 0);
    }

    if (*pexpr_ptr) {
        old_pexpr = *pexpr_ptr;
        *pexpr_ptr = NULL;
        is_inited = 1;
    }

    ret = av_expr_parse(pexpr_ptr, args, var_names,
                        NULL, NULL, NULL, NULL, 0, ctx);
    if (ret < 0) {
        av_log(ctx, AV_LOG_ERROR, "Cannot parse expression for %s: '%s'\n", var, args);
        goto revert;
    }

    ret = check_exprs(ctx);
    if (ret < 0)
        goto revert;

    if (is_inited && (ret = config_props(ctx->outputs[0])) < 0)
        goto revert;

    av_expr_free(old_pexpr);
    old_pexpr = NULL;
    av_freep(&old_str_expr);

    return 0;

revert:
    av_expr_free(*pexpr_ptr);
    *pexpr_ptr = NULL;
    if (old_str_expr) {
        av_opt_set(scale, var, old_str_expr, 0);
        av_free(old_str_expr);
    }
    if (old_pexpr)
        *pexpr_ptr = old_pexpr;

    return ret;
}

static av_cold int init_dict(AVFilterContext *ctx, AVDictionary **opts)
{
    ScaleContext *scale = ctx->priv;
    int ret;

    if (scale->size_str && (scale->w_expr || scale->h_expr)) {
        av_log(ctx, AV_LOG_ERROR,
               "Size and width/height expressions cannot be set at the same time.\n");
            return AVERROR(EINVAL);
    }

    if (scale->w_expr && !scale->h_expr)
        FFSWAP(char *, scale->w_expr, scale->size_str);

    if (scale->size_str) {
        char buf[32];
        if ((ret = av_parse_video_size(&scale->w, &scale->h, scale->size_str)) < 0) {
            av_log(ctx, AV_LOG_ERROR,
                   "Invalid size '%s'\n", scale->size_str);
            return ret;
        }
        snprintf(buf, sizeof(buf)-1, "%d", scale->w);
        av_opt_set(scale, "w", buf, 0);
        snprintf(buf, sizeof(buf)-1, "%d", scale->h);
        av_opt_set(scale, "h", buf, 0);
    }
    if (!scale->w_expr)
        av_opt_set(scale, "w", "iw", 0);
    if (!scale->h_expr)
        av_opt_set(scale, "h", "ih", 0);

    ret = scale_parse_expr(ctx, NULL, &scale->w_pexpr, "width", scale->w_expr);
    if (ret < 0)
        return ret;

    ret = scale_parse_expr(ctx, NULL, &scale->h_pexpr, "height", scale->h_expr);
    if (ret < 0)
        return ret;

    av_log(ctx, AV_LOG_VERBOSE, "w:%s h:%s flags:'%s' interl:%d\n",
           scale->w_expr, scale->h_expr, (char *)av_x_if_null(scale->flags_str, ""), scale->interlaced);

    scale->flags = 0;

    if (scale->flags_str) {
        const AVClass *class = sws_get_class();
        const AVOption    *o = av_opt_find(&class, "sws_flags", NULL, 0,
                                           AV_OPT_SEARCH_FAKE_OBJ);
        int ret = av_opt_eval_flags(&class, o, scale->flags_str, &scale->flags);
        if (ret < 0)
            return ret;
    }
    scale->opts = *opts;
    *opts = NULL;

    scale->temp_frame[0] = NULL;
    scale->temp_frame[1] = NULL;

    return 0;
}

static av_cold void uninit(AVFilterContext *ctx)
{
    ScaleContext *scale = ctx->priv;
    av_expr_free(scale->w_pexpr);
    av_expr_free(scale->h_pexpr);
    scale->w_pexpr = scale->h_pexpr = NULL;
    sws_freeContext(scale->sws);
    sws_freeContext(scale->isws[0]);
    sws_freeContext(scale->isws[1]);
    scale->sws = NULL;
    av_dict_free(&scale->opts);
    if (scale->temp_frame[0])
        av_frame_unref(scale->temp_frame[0]);
    if (scale->temp_frame[1])
        av_frame_unref(scale->temp_frame[1]);
}

static int query_formats(AVFilterContext *ctx)
{
    AVFilterFormats *formats;
    enum AVPixelFormat pix_fmt;
    int ret;

    if (ctx->inputs[0]) {
        const AVPixFmtDescriptor *desc = NULL;
        formats = NULL;
        while ((desc = av_pix_fmt_desc_next(desc))) {
            pix_fmt = av_pix_fmt_desc_get_id(desc);
            if ((sws_isSupportedInput(pix_fmt) ||
                 sws_isSupportedEndiannessConversion(pix_fmt))
                && (ret = ff_add_format(&formats, pix_fmt)) < 0) {
                return ret;
            }
        }
        if ((ret = ff_formats_ref(formats, &ctx->inputs[0]->outcfg.formats)) < 0)
            return ret;
    }
    if (ctx->outputs[0]) {
        const AVPixFmtDescriptor *desc = NULL;
        formats = NULL;
        while ((desc = av_pix_fmt_desc_next(desc))) {
            pix_fmt = av_pix_fmt_desc_get_id(desc);
            if ((sws_isSupportedOutput(pix_fmt) || pix_fmt == AV_PIX_FMT_PAL8 ||
                 sws_isSupportedEndiannessConversion(pix_fmt))
                && (ret = ff_add_format(&formats, pix_fmt)) < 0) {
                return ret;
            }
        }
        if ((ret = ff_formats_ref(formats, &ctx->outputs[0]->incfg.formats)) < 0)
            return ret;
    }

    return 0;
}

static const int *parse_yuv_type(const char *s, enum AVColorSpace colorspace)
{
    if (!s)
        s = "bt601";

    if (s && strstr(s, "bt709")) {
        colorspace = AVCOL_SPC_BT709;
    } else if (s && strstr(s, "fcc")) {
        colorspace = AVCOL_SPC_FCC;
    } else if (s && strstr(s, "smpte240m")) {
        colorspace = AVCOL_SPC_SMPTE240M;
    } else if (s && (strstr(s, "bt601") || strstr(s, "bt470") || strstr(s, "smpte170m"))) {
        colorspace = AVCOL_SPC_BT470BG;
    } else if (s && strstr(s, "bt2020")) {
        colorspace = AVCOL_SPC_BT2020_NCL;
    }

    if (colorspace < 1 || colorspace > 10 || colorspace == 8) {
        colorspace = AVCOL_SPC_BT470BG;
    }

    return sws_getCoefficients(colorspace);
}

static int scale_eval_dimensions(AVFilterContext *ctx)
{
    ScaleContext *scale = ctx->priv;
    const char scale2ref = ctx->filter == &ff_vf_scale2ref;
    const AVFilterLink *inlink = scale2ref ? ctx->inputs[1] : ctx->inputs[0];
    const AVFilterLink *outlink = ctx->outputs[0];
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(inlink->format);
    const AVPixFmtDescriptor *out_desc = av_pix_fmt_desc_get(outlink->format);
    char *expr;
    int eval_w, eval_h;
    int ret;
    double res;
    const AVPixFmtDescriptor *main_desc;
    const AVFilterLink *main_link;

    if (scale2ref) {
        main_link = ctx->inputs[0];
        main_desc = av_pix_fmt_desc_get(main_link->format);
    }

    scale->var_values[VAR_IN_W]  = scale->var_values[VAR_IW] = inlink->w;
    scale->var_values[VAR_IN_H]  = scale->var_values[VAR_IH] = inlink->h;
    scale->var_values[VAR_OUT_W] = scale->var_values[VAR_OW] = NAN;
    scale->var_values[VAR_OUT_H] = scale->var_values[VAR_OH] = NAN;
    scale->var_values[VAR_A]     = (double) inlink->w / inlink->h;
    scale->var_values[VAR_SAR]   = inlink->sample_aspect_ratio.num ?
        (double) inlink->sample_aspect_ratio.num / inlink->sample_aspect_ratio.den : 1;
    scale->var_values[VAR_DAR]   = scale->var_values[VAR_A] * scale->var_values[VAR_SAR];
    scale->var_values[VAR_HSUB]  = 1 << desc->log2_chroma_w;
    scale->var_values[VAR_VSUB]  = 1 << desc->log2_chroma_h;
    scale->var_values[VAR_OHSUB] = 1 << out_desc->log2_chroma_w;
    scale->var_values[VAR_OVSUB] = 1 << out_desc->log2_chroma_h;

    if (scale2ref) {
        scale->var_values[VAR_S2R_MAIN_W] = main_link->w;
        scale->var_values[VAR_S2R_MAIN_H] = main_link->h;
        scale->var_values[VAR_S2R_MAIN_A] = (double) main_link->w / main_link->h;
        scale->var_values[VAR_S2R_MAIN_SAR] = main_link->sample_aspect_ratio.num ?
            (double) main_link->sample_aspect_ratio.num / main_link->sample_aspect_ratio.den : 1;
        scale->var_values[VAR_S2R_MAIN_DAR] = scale->var_values[VAR_S2R_MDAR] =
            scale->var_values[VAR_S2R_MAIN_A] * scale->var_values[VAR_S2R_MAIN_SAR];
        scale->var_values[VAR_S2R_MAIN_HSUB] = 1 << main_desc->log2_chroma_w;
        scale->var_values[VAR_S2R_MAIN_VSUB] = 1 << main_desc->log2_chroma_h;
    }

    res = av_expr_eval(scale->w_pexpr, scale->var_values, NULL);
    eval_w = scale->var_values[VAR_OUT_W] = scale->var_values[VAR_OW] = (int) res == 0 ? inlink->w : (int) res;

    res = av_expr_eval(scale->h_pexpr, scale->var_values, NULL);
    if (isnan(res)) {
        expr = scale->h_expr;
        ret = AVERROR(EINVAL);
        goto fail;
    }
    eval_h = scale->var_values[VAR_OUT_H] = scale->var_values[VAR_OH] = (int) res == 0 ? inlink->h : (int) res;

    res = av_expr_eval(scale->w_pexpr, scale->var_values, NULL);
    if (isnan(res)) {
        expr = scale->w_expr;
        ret = AVERROR(EINVAL);
        goto fail;
    }
    eval_w = scale->var_values[VAR_OUT_W] = scale->var_values[VAR_OW] = (int) res == 0 ? inlink->w : (int) res;

    scale->w = eval_w;
    scale->h = eval_h;

    return 0;

fail:
    av_log(ctx, AV_LOG_ERROR,
           "Error when evaluating the expression '%s'.\n", expr);
    return ret;
}

static int config_props(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    AVFilterLink *inlink0 = outlink->src->inputs[0];
    AVFilterLink *inlink  = ctx->filter == &ff_vf_scale2ref ?
                            outlink->src->inputs[1] :
                            outlink->src->inputs[0];
    enum AVPixelFormat infmt = inlink0->format;
    enum AVPixelFormat outfmt = outlink->format;
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(inlink->format);
    ScaleContext *scale = ctx->priv;
    int ret;

    if (infmt == AV_PIX_FMT_XV15)
        infmt = AV_PIX_FMT_YUV420P10LE;
    if (outfmt == AV_PIX_FMT_XV15)
        outfmt = AV_PIX_FMT_YUV420P10LE;
    if ((ret = scale_eval_dimensions(ctx)) < 0)
        goto fail;

    ff_scale_adjust_dimensions(inlink, &scale->w, &scale->h,
                               scale->force_original_aspect_ratio,
                               scale->force_divisible_by);

    if (scale->w > INT_MAX ||
        scale->h > INT_MAX ||
        (scale->h * inlink->w) > INT_MAX ||
        (scale->w * inlink->h) > INT_MAX)
        av_log(ctx, AV_LOG_ERROR, "Rescaled value for width or height is too big.\n");

    outlink->w = scale->w;
    outlink->h = scale->h;

    /* TODO: make algorithm configurable */

    scale->input_is_pal = desc->flags & AV_PIX_FMT_FLAG_PAL;
    if (outfmt == AV_PIX_FMT_PAL8) outfmt = AV_PIX_FMT_BGR8;
    scale->output_is_pal = av_pix_fmt_desc_get(outfmt)->flags & AV_PIX_FMT_FLAG_PAL ||
                           av_pix_fmt_desc_get(outfmt)->flags & FF_PSEUDOPAL;

    if (scale->sws)
        sws_freeContext(scale->sws);
    if (scale->isws[0])
        sws_freeContext(scale->isws[0]);
    if (scale->isws[1])
        sws_freeContext(scale->isws[1]);
    scale->isws[0] = scale->isws[1] = scale->sws = NULL;
    if (inlink0->w == outlink->w &&
        inlink0->h == outlink->h &&
        !scale->out_color_matrix &&
        scale->in_range == scale->out_range &&
        inlink0->format == outlink->format)
        ;
    else {
        struct SwsContext **swscs[3] = {&scale->sws, &scale->isws[0], &scale->isws[1]};
        int i;

        for (i = 0; i < 3; i++) {
            int in_v_chr_pos = scale->in_v_chr_pos, out_v_chr_pos = scale->out_v_chr_pos;
            struct SwsContext **s = swscs[i];
            *s = sws_alloc_context();
            if (!*s)
                return AVERROR(ENOMEM);

            av_opt_set_int(*s, "srcw", inlink0 ->w, 0);
            av_opt_set_int(*s, "srch", inlink0 ->h >> !!i, 0);
            av_opt_set_int(*s, "src_format", infmt, 0);
            av_opt_set_int(*s, "dstw", outlink->w, 0);
            av_opt_set_int(*s, "dsth", outlink->h >> !!i, 0);
            av_opt_set_int(*s, "dst_format", outfmt, 0);
            av_opt_set_int(*s, "sws_flags", scale->flags, 0);
            av_opt_set_int(*s, "param0", scale->param[0], 0);
            av_opt_set_int(*s, "param1", scale->param[1], 0);
            if (scale->in_range != AVCOL_RANGE_UNSPECIFIED)
                av_opt_set_int(*s, "src_range",
                               scale->in_range == AVCOL_RANGE_JPEG, 0);
            if (scale->out_range != AVCOL_RANGE_UNSPECIFIED)
                av_opt_set_int(*s, "dst_range",
                               scale->out_range == AVCOL_RANGE_JPEG, 0);

            if (scale->opts) {
                AVDictionaryEntry *e = NULL;
                while ((e = av_dict_get(scale->opts, "", e, AV_DICT_IGNORE_SUFFIX))) {
                    if ((ret = av_opt_set(*s, e->key, e->value, 0)) < 0)
                        return ret;
                }
            }
            /* Override YUV420P default settings to have the correct (MPEG-2) chroma positions
             * MPEG-2 chroma positions are used by convention
             * XXX: support other 4:2:0 pixel formats */
            if (infmt == AV_PIX_FMT_YUV420P && scale->in_v_chr_pos == -513) {
                in_v_chr_pos = (i == 0) ? 128 : (i == 1) ? 64 : 192;
            }

            if (outfmt == AV_PIX_FMT_YUV420P && scale->out_v_chr_pos == -513) {
                out_v_chr_pos = (i == 0) ? 128 : (i == 1) ? 64 : 192;
            }

            av_opt_set_int(*s, "src_h_chr_pos", scale->in_h_chr_pos, 0);
            av_opt_set_int(*s, "src_v_chr_pos", in_v_chr_pos, 0);
            av_opt_set_int(*s, "dst_h_chr_pos", scale->out_h_chr_pos, 0);
            av_opt_set_int(*s, "dst_v_chr_pos", out_v_chr_pos, 0);

            if ((ret = sws_init_context(*s, NULL, NULL)) < 0)
                return ret;
            if (!scale->interlaced)
                break;
        }
    }

    if (inlink0->sample_aspect_ratio.num){
        outlink->sample_aspect_ratio = av_mul_q((AVRational){outlink->h * inlink0->w, outlink->w * inlink0->h}, inlink0->sample_aspect_ratio);
    } else
        outlink->sample_aspect_ratio = inlink0->sample_aspect_ratio;

    av_log(ctx, AV_LOG_VERBOSE, "w:%d h:%d fmt:%s sar:%d/%d -> w:%d h:%d fmt:%s sar:%d/%d flags:0x%0x\n",
           inlink ->w, inlink ->h, av_get_pix_fmt_name( inlink0->format),
           inlink->sample_aspect_ratio.num, inlink->sample_aspect_ratio.den,
           outlink->w, outlink->h, av_get_pix_fmt_name(outlink->format),
           outlink->sample_aspect_ratio.num, outlink->sample_aspect_ratio.den,
           scale->flags);
    return 0;

fail:
    return ret;
}

static int config_props_ref(AVFilterLink *outlink)
{
    AVFilterLink *inlink = outlink->src->inputs[1];

    outlink->w = inlink->w;
    outlink->h = inlink->h;
    outlink->sample_aspect_ratio = inlink->sample_aspect_ratio;
    outlink->time_base = inlink->time_base;
    outlink->frame_rate = inlink->frame_rate;

    return 0;
}

static int request_frame(AVFilterLink *outlink)
{
    return ff_request_frame(outlink->src->inputs[0]);
}

static int request_frame_ref(AVFilterLink *outlink)
{
    return ff_request_frame(outlink->src->inputs[1]);
}

#if CONFIG_LIBXMA2API
/**
 * Extracts a 10 bit pixel from a vcu word and stores it in a 16 bit word
 * @param pixel_index Which pixel of the vcu word to take (0-2)
 * @param vcu_word The source vcu word containing 3 pixels
 * @param out_word Where to store the first (LSB) pixel
 * @return void
 */
static void extract_pixel_from_xv15_word(uint8_t pixel_index, uint32_t vcu_word,
                                       uint16_t** out_word)
{
    if(pixel_index == 0) {
        *(*out_word)++ = (uint16_t) (vcu_word & 0x3FF);
    } else if(pixel_index == 1) {
        *(*out_word)++ = (uint16_t) ((vcu_word & 0xFFC00)    >> 10);
    } else {
        *(*out_word)++ = (uint16_t) ((vcu_word & 0x3FF00000) >> 20);
    }
}

/**
 * Converts an xv15 word into yuv420p10le words stored in the y plane.
 * @param num_pxls_to_xtrct The number of pixels to extract from the source
 * word
 * @param xv15_word The source xv15 word containing 3 pixels of data
 * @param y_plane The output y plane
 * @return void
 */
static void y_xv15_wrd_10le_wrds(uint8_t num_pxls_to_xtrct, uint32_t xv15_word,
                                 uint16_t** y_plane)
{
    switch(num_pxls_to_xtrct) {
        case 3:
            extract_pixel_from_xv15_word(0, xv15_word, y_plane);
            extract_pixel_from_xv15_word(1, xv15_word, y_plane);
            extract_pixel_from_xv15_word(2, xv15_word, y_plane);
            break;
        case 2:
            extract_pixel_from_xv15_word(0, xv15_word, y_plane);
            extract_pixel_from_xv15_word(1, xv15_word, y_plane);
            break;
        case 1:
            extract_pixel_from_xv15_word(0, xv15_word, y_plane);
            break;
        default:
            return;
    }
}

/**
 * Converts 1-2 xv15 words into yuv420p10le words stored in their respective u
 * & v planes.
 * @param num_pxls_to_xtrct The number of pixels to extract from the source
 * words
 * @param xv15_word1 The first xv15 source word
 * @param xv15_word2 The second xv15 source word
 * @param u_plane The output u plane
 * @param v_plane The output v plane
 * @return void
 */
static void uv_xv15_wrd_to_10le_wrds(uint8_t num_pxls_to_xtrct, 
                                     uint32_t xv15_word1, uint32_t xv15_word2,
                                     uint16_t** u_plane, uint16_t** v_plane)
{
    switch(num_pxls_to_xtrct) {
        case 6:
            extract_pixel_from_xv15_word(0, xv15_word1, u_plane);
            extract_pixel_from_xv15_word(1, xv15_word1, v_plane);
            extract_pixel_from_xv15_word(2, xv15_word1, u_plane);

            extract_pixel_from_xv15_word(0, xv15_word2, v_plane);
            extract_pixel_from_xv15_word(1, xv15_word2, u_plane);
            extract_pixel_from_xv15_word(2, xv15_word2, v_plane);
            break;
        case 5:
            extract_pixel_from_xv15_word(0, xv15_word1, u_plane);
            extract_pixel_from_xv15_word(1, xv15_word1, v_plane);
            extract_pixel_from_xv15_word(2, xv15_word1, u_plane);

            extract_pixel_from_xv15_word(0, xv15_word2, v_plane);
            extract_pixel_from_xv15_word(1, xv15_word2, u_plane);
            break;
        case 4:
            extract_pixel_from_xv15_word(0, xv15_word1, u_plane);
            extract_pixel_from_xv15_word(1, xv15_word1, v_plane);
            extract_pixel_from_xv15_word(2, xv15_word1, u_plane);

            extract_pixel_from_xv15_word(0, xv15_word2, v_plane);
            break;
        case 3:
            extract_pixel_from_xv15_word(0, xv15_word1, u_plane);
            extract_pixel_from_xv15_word(1, xv15_word1, v_plane);
            extract_pixel_from_xv15_word(2, xv15_word1, u_plane);
            break;
        case 2:
            extract_pixel_from_xv15_word(0, xv15_word1, u_plane);
            extract_pixel_from_xv15_word(1, xv15_word1, v_plane);
            break;
        case 1:
            extract_pixel_from_xv15_word(0, xv15_word1, u_plane);
            break;
        default:
            return;
    }
}

/**
 * Get the buffer from the fpga and format it into yuv420p10le format.
 * @param xframe The frame which is used to get the buffer from device
 * @param in The input AVFrame containing frame info
 * @param out The AVFrame into which the yuv420p10le frame will be stored.
 * @return 0 on success or -1 on error
 */
static int conv_xv15_to_yuv420p10le(AVFrame* in, AVFrame* out)
{
    out->linesize[0] = out->width * 2;
    out->linesize[1] = out->linesize[0] / 2;
    out->linesize[2] = out->linesize[1];
    uint16_t* y_plane       = (uint16_t*)out->data[0];
    uint32_t* current_line  = (uint32_t*)(in->data[0]);
    uint16_t total_words_in_line = in->linesize[0] / sizeof(uint32_t);
    uint16_t valid_words_in_line = in->width / 3;
    uint8_t  leftover_pixels     = in->width % 3;
    uint16_t num_rows_in_plane   = in->height;
    uint16_t w, h;
    for (h = 0; h < num_rows_in_plane; h++) {
        for (w = 0; w < valid_words_in_line; w++) {
            y_xv15_wrd_10le_wrds(3, current_line[w], &y_plane);
        }
        y_xv15_wrd_10le_wrds(leftover_pixels, current_line[w],
                                             &y_plane);
        current_line += total_words_in_line;
    }
    uint16_t* u_plane = (uint16_t*)out->data[1];
    uint16_t* v_plane = (uint16_t*)out->data[2];
    current_line      = (uint32_t*)(in->data[1]);
    num_rows_in_plane   = in->height / 2;
    valid_words_in_line = in->width / 6; // Reading 2 words at a time
    leftover_pixels     = in->width % 6;
    size_t word_index;
    for (h = 0; h < num_rows_in_plane; h++) {
        word_index = 0;
        for (w = 0; w < valid_words_in_line; w++) {
            uv_xv15_wrd_to_10le_wrds(6, current_line[word_index],
                                     current_line[word_index+1], &u_plane,
                                     &v_plane);
            word_index += 2;
        }
        uv_xv15_wrd_to_10le_wrds(leftover_pixels,
                                 current_line[word_index],
                                 current_line[word_index+1], &u_plane,
                                 &v_plane);
        current_line += total_words_in_line;
    }
    return 0;
}

/**
 * Write the values of 3 pixels into the next word of the xv15 (aka nv12_10le32)
 * buffer and increment the buffer to the next 32 bit WORD.
 * @param p1 The first pixel to be written (LSB)
 * @param p2 The second pixel to be written
 * @param p3 The third pixel to be written
 * @param xv15_buffer A pointer to the output xv15 (aka nv12_10le32)
 * buffer
 * @return void
 */
static void yuv10b_pixls_to_xv15_wrd(uint16_t p1, uint16_t p2,
                                     uint16_t p3,
                                     uint32_t** xv15_buffer) {
    *(*xv15_buffer)++ = 0x3FFFFFFF & (p1 | (p2 << 10) | (p3 << 20));
}

/**
 * Write up to 3 pixels from the source y buffer into the xv15 (aka nv12_10le32)
 * buffer
 * @param num_pixels_to_write The number of pixels to write. 1-3
 * @param y_buffer A pointer to the source y plane buffer
 * @param xv15_buffer A pointer to the output xv15 (aka nv12_10le32) buffer
 * @return void
 */
static void y_10b_seg_to_xv15_wrd(uint8_t num_pixels_to_write,
                                  uint16_t** y_buffer, uint32_t** xv15_buffer)
{
    switch(num_pixels_to_write) {
        case 3:
            yuv10b_pixls_to_xv15_wrd((*y_buffer)[0], (*y_buffer)[1],
                                    (*y_buffer)[2], xv15_buffer);
            break;
        case 2:
            yuv10b_pixls_to_xv15_wrd((*y_buffer)[0], (*y_buffer)[1], 0,
                                     xv15_buffer);
            break;
        case 1:
            yuv10b_pixls_to_xv15_wrd((*y_buffer)[0], 0, 0, xv15_buffer);
            break;
        default:
            return;
    }
    *y_buffer += num_pixels_to_write;
}

/**
 * Write up to 6 pixels from the source u & v buffers into the xv15
 * (aka nv12_10le32) buffer
 * @param num_pixels_to_write The number of pixels to write. 1-6
 * @param u_buffer A pointer to the source u plane buffer
 * @param v_buffer A pointer to the source v plane buffer
 * @param xv15_buffer A pointer to the output xv15 (aka nv12_10le32) buffer
 * @return void
 */
static void uv_10b_seg_to_xv15_wrd(uint8_t num_pixels_to_write,
                                   uint16_t** u_buffer, uint16_t** v_buffer,
                                   uint32_t** xv15_buffer)
{
    switch(num_pixels_to_write) {
        case 6:
            yuv10b_pixls_to_xv15_wrd((*u_buffer)[0], (*v_buffer)[0],
                                    (*u_buffer)[1], xv15_buffer);
            yuv10b_pixls_to_xv15_wrd((*v_buffer)[1], (*u_buffer)[2],
                                    (*v_buffer)[2], xv15_buffer);
            break;
        case 5:
            yuv10b_pixls_to_xv15_wrd((*u_buffer)[0], (*v_buffer)[0],
                                    (*u_buffer)[1], xv15_buffer);
            yuv10b_pixls_to_xv15_wrd((*v_buffer)[1], (*u_buffer)[2], 0,
                                     xv15_buffer);
            break;
        case 4:
            yuv10b_pixls_to_xv15_wrd((*u_buffer)[0], (*v_buffer)[0],
                                    (*u_buffer)[1], xv15_buffer);
            yuv10b_pixls_to_xv15_wrd((*v_buffer)[1], 0, 0, xv15_buffer);
            break;
        case 3:
            yuv10b_pixls_to_xv15_wrd((*u_buffer)[0], (*v_buffer)[0],
                                    (*u_buffer)[1], xv15_buffer);
            break;
        case 2:
            yuv10b_pixls_to_xv15_wrd((*u_buffer)[0], (*v_buffer)[0], 0,
                                     xv15_buffer);
            break;
        case 1:
            yuv10b_pixls_to_xv15_wrd((*u_buffer)[0], 0, 0, xv15_buffer);
            break;
        default:
            return;
    }
    *u_buffer += (num_pixels_to_write + 1) / 2;
    *v_buffer += num_pixels_to_write / 2;
}

/**
 * Convert the input yuv420p10le frame into the xv15 (nv12_10le32) format
 * @param in The input AVFrame containing frame info + the yuv420p10le frame
 * @param out The AVFrame into which the vcu formatted frame will be stored
 * @return 0 on success or -1 on error
 */
static int32_t conv_yuv420p10le_to_xv15(const AVFrame* in, AVFrame* out)
{
    out->linesize[0] = ((in->width + 2) / 3) * 4;
    out->linesize[1] = out->linesize[0];
    out->data[0] = out->buf[0]->data;
    out->data[1] = out->buf[1]->data;

    uint16_t  pixels_per_word   = 3;
    uint16_t* y_buffer;
    uint32_t* current_buffer    = (uint32_t*)(out->data[0]);
    uint16_t  rows_in_plane     = in->height;
    uint16_t  words_in_line     = in->width / pixels_per_word;
    uint8_t   leftover_pixels   = in->width % pixels_per_word;
    for(uint16_t h = 0; h < rows_in_plane; h++) {
        y_buffer                = (uint16_t*)((uint8_t*)in->data[0] + (h * in->linesize[0]));
        for(uint16_t w = 0; w < words_in_line; w++) {
            y_10b_seg_to_xv15_wrd(pixels_per_word, &y_buffer, &current_buffer);
        }
        if(leftover_pixels) {
            y_10b_seg_to_xv15_wrd(leftover_pixels, &y_buffer, &current_buffer);
        }
    }

    pixels_per_word     = 6;
    uint16_t* u_buffer;
    uint16_t* v_buffer;
    current_buffer      = (uint32_t*)(out->data[1]);
    words_in_line       = in->width / pixels_per_word;
    leftover_pixels     = in->width % pixels_per_word;
    rows_in_plane       = in->height / 2;
    for(uint16_t h = 0; h < rows_in_plane; h++) {
        u_buffer                = (uint16_t*)((uint8_t*)in->data[1] + (h * in->linesize[1]));
        v_buffer                = (uint16_t*)((uint8_t*)in->data[2] + (h * in->linesize[2]));
        for(uint16_t w = 0; w < words_in_line; w++) {
            uv_10b_seg_to_xv15_wrd(pixels_per_word, &u_buffer, &v_buffer, &current_buffer);
        }
        if(leftover_pixels) {
            uv_10b_seg_to_xv15_wrd(leftover_pixels, &u_buffer, &v_buffer,
                                    &current_buffer);
        }
    }
    return 0;
}
#endif

static int alloc_temp_frame(AVFrame *pic, int format, AVFrame **frame)
{
    ptrdiff_t linesizes[4];
    size_t sizes[4];
    int i, ret = 0, padded_height;

    *frame = av_frame_alloc();
    (*frame)->format = format;
    (*frame)->width = pic->width;
    (*frame)->height = pic->height;
    for(i=1; i<=32; i+=i) {
        ret = av_image_fill_linesizes((*frame)->linesize, format,
                                      FFALIGN(pic->width, i));
        if (ret < 0)
            return ret;
        if (!((*frame)->linesize[0] & 31))
            break;
    }

    for(i = 0; i < 4 && (*frame)->linesize[i]; i++)
        (*frame)->linesize[i] = FFALIGN((*frame)->linesize[i], 32);

    for(i = 0; i < 4; i++)
        linesizes[i] = (*frame)->linesize[i];

    padded_height = FFALIGN((*frame)->height, 32);
    if ((ret = av_image_fill_plane_sizes(sizes, format,
                                         padded_height, linesizes)) < 0)
        return ret;

    for(i = 0; i < 4; i++) {
        if(sizes[i] > INT_MAX - 32)
            return AVERROR(EINVAL);
        if (sizes[i] > 0) {
            (*frame)->buf[i] = av_buffer_alloc(sizes[i]);
            if (!(*frame)->buf[i])
                return AVERROR(ENOMEM);
            (*frame)->data[i] = (*frame)->buf[i]->data;
        } else {
            (*frame)->buf[i] = NULL;
            (*frame)->data[i] = NULL;
        }
    }

    return ret;
}

static void free_side_data(AVFrameSideData **ptr_sd)
{
    AVFrameSideData *sd = *ptr_sd;

    av_buffer_unref(&sd->buf);
    av_dict_free(&sd->metadata);
    av_freep(ptr_sd);
}

static void wipe_side_data(AVFrame *frame)
{
    int i;

    for (i = 0; i < frame->nb_side_data; i++) {
        free_side_data(&frame->side_data[i]);
    }
    frame->nb_side_data = 0;

    av_freep(&frame->side_data);
}

static int frame_copy_props(AVFrame *dst, const AVFrame *src, int force_copy)
{
    int ret, i;

    dst->key_frame              = src->key_frame;
    dst->pict_type              = src->pict_type;
    dst->sample_aspect_ratio    = src->sample_aspect_ratio;
    dst->crop_top               = src->crop_top;
    dst->crop_bottom            = src->crop_bottom;
    dst->crop_left              = src->crop_left;
    dst->crop_right             = src->crop_right;
    dst->pts                    = src->pts;
    dst->repeat_pict            = src->repeat_pict;
    dst->interlaced_frame       = src->interlaced_frame;
    dst->top_field_first        = src->top_field_first;
    dst->palette_has_changed    = src->palette_has_changed;
    dst->sample_rate            = src->sample_rate;
    dst->opaque                 = src->opaque;
#if FF_API_PKT_PTS
FF_DISABLE_DEPRECATION_WARNINGS
    dst->pkt_pts                = src->pkt_pts;
FF_ENABLE_DEPRECATION_WARNINGS
#endif
    dst->pkt_dts                = src->pkt_dts;
    dst->pkt_pos                = src->pkt_pos;
    dst->pkt_size               = src->pkt_size;
    dst->pkt_duration           = src->pkt_duration;
    dst->reordered_opaque       = src->reordered_opaque;
    dst->quality                = src->quality;
    dst->best_effort_timestamp  = src->best_effort_timestamp;
    dst->coded_picture_number   = src->coded_picture_number;
    dst->display_picture_number = src->display_picture_number;
    dst->flags                  = src->flags;
    dst->decode_error_flags     = src->decode_error_flags;
    dst->color_primaries        = src->color_primaries;
    dst->color_trc              = src->color_trc;
    dst->colorspace             = src->colorspace;
    dst->color_range            = src->color_range;
    dst->chroma_location        = src->chroma_location;

    av_dict_copy(&dst->metadata, src->metadata, 0);

#if FF_API_ERROR_FRAME
FF_DISABLE_DEPRECATION_WARNINGS
    memcpy(dst->error, src->error, sizeof(dst->error));
FF_ENABLE_DEPRECATION_WARNINGS
#endif

    for (i = 0; i < src->nb_side_data; i++) {
        const AVFrameSideData *sd_src = src->side_data[i];
        AVFrameSideData *sd_dst;
        if (   sd_src->type == AV_FRAME_DATA_PANSCAN
            && (src->width != dst->width || src->height != dst->height))
            continue;
        if (force_copy) {
            sd_dst = av_frame_new_side_data(dst, sd_src->type,
                                            sd_src->size);
            if (!sd_dst) {
                wipe_side_data(dst);
                return AVERROR(ENOMEM);
            }
            memcpy(sd_dst->data, sd_src->data, sd_src->size);
        } else {
            AVBufferRef *ref = av_buffer_ref(sd_src->buf);
            sd_dst = av_frame_new_side_data_from_buf(dst, sd_src->type, ref);
            if (!sd_dst) {
                av_buffer_unref(&ref);
                wipe_side_data(dst);
                return AVERROR(ENOMEM);
            }
        }
        av_dict_copy(&sd_dst->metadata, sd_src->metadata, 0);
    }

#if FF_API_FRAME_QP
FF_DISABLE_DEPRECATION_WARNINGS
    dst->qscale_table = NULL;
    dst->qstride      = 0;
    dst->qscale_type  = 0;
    av_buffer_replace(&dst->qp_table_buf, src->qp_table_buf);
    if (dst->qp_table_buf) {
        dst->qscale_table = dst->qp_table_buf->data;
        dst->qstride      = src->qstride;
        dst->qscale_type  = src->qscale_type;
    }
FF_ENABLE_DEPRECATION_WARNINGS
#endif

    ret = av_buffer_replace(&dst->opaque_ref, src->opaque_ref);
    ret |= av_buffer_replace(&dst->private_ref, src->private_ref);
    return ret;
}

static int scale_slice(AVFilterLink *link, AVFrame *out_buf, AVFrame *cur_pic, struct SwsContext *sws, int y, int h, int mul, int field)
{
    ScaleContext *scale = link->dst->priv;
    const uint8_t *in[4];
    uint8_t *out[4];
    int in_stride[4],out_stride[4];
    int i, ret;
    AVFrame* temp;

    #if CONFIG_LIBXMA2API
    if ((cur_pic->width == out_buf->width) &&
        (cur_pic->height == out_buf->height) &&
        (!scale->out_color_matrix) &&
        (scale->in_range == scale->out_range)) {
        if ((cur_pic->format == AV_PIX_FMT_XV15) &&
            (out_buf->format == AV_PIX_FMT_YUV420P10LE)) {
            return conv_xv15_to_yuv420p10le(cur_pic, out_buf);
        } else if ((cur_pic->format == AV_PIX_FMT_YUV420P10LE) &&
            (out_buf->format == AV_PIX_FMT_XV15)) {
            out_buf->linesize[0] = ((cur_pic->width + 2) / 3) * 4;
            out_buf->linesize[1] = out_buf->linesize[0];
            return conv_yuv420p10le_to_xv15(cur_pic, out_buf);
        }
    }
    if(cur_pic->format == AV_PIX_FMT_XV15) {
        if(scale->temp_frame[0] == NULL) {
            ret = alloc_temp_frame(cur_pic, AV_PIX_FMT_YUV420P10LE, &scale->temp_frame[0]);
            if (ret < 0)
                return ret;
        }
        ret = frame_copy_props(scale->temp_frame[0], cur_pic, 0);
        if (ret < 0)
            return ret;
        scale->temp_frame[0]->extended_data = scale->temp_frame[0]->data;

        conv_xv15_to_yuv420p10le(cur_pic, scale->temp_frame[0]);
        
        temp = scale->temp_frame[0];
        scale->temp_frame[0] = cur_pic;
        cur_pic = temp;
    }
    if(out_buf->format == AV_PIX_FMT_XV15) {
        if(scale->temp_frame[1] == NULL) {
            ret = alloc_temp_frame(out_buf, AV_PIX_FMT_YUV422P10LE, &scale->temp_frame[1]);
            if (ret < 0)
                return ret;
        }
        scale->temp_frame[1]->extended_data = scale->temp_frame[1]->data;

        temp = scale->temp_frame[1];
        scale->temp_frame[1] = out_buf;
        out_buf = temp;
    }
    #endif

    for (i=0; i<4; i++) {
        int vsub= ((i+1)&2) ? scale->vsub : 0;
         in_stride[i] = cur_pic->linesize[i] * mul;
        out_stride[i] = out_buf->linesize[i] * mul;
         in[i] = FF_PTR_ADD(cur_pic->data[i], ((y>>vsub)+field) * cur_pic->linesize[i]);
        out[i] = FF_PTR_ADD(out_buf->data[i],            field  * out_buf->linesize[i]);
    }
    if (scale->input_is_pal)
         in[1] = cur_pic->data[1];
    if (scale->output_is_pal)
        out[1] = out_buf->data[1];

    ret = sws_scale(sws, in, in_stride, y/mul, h,
                    out,out_stride);
    if (ret < 0)
        return ret;

    #if CONFIG_LIBXMA2API
    if(scale->temp_frame[0]) {
        temp = scale->temp_frame[0];
        scale->temp_frame[0] = cur_pic;
        cur_pic = temp;
    }
    if(scale->temp_frame[1]) {
        ret = conv_yuv420p10le_to_xv15(out_buf, scale->temp_frame[1]);
        if (ret < 0)
            return ret;
        ret = frame_copy_props(out_buf, scale->temp_frame[1], 0);
        if (ret < 0)
            return ret;
        
        temp = scale->temp_frame[1];
        scale->temp_frame[1] = out_buf;
        out_buf = temp;
    }
    #endif

    return 0;
}

static int scale_frame(AVFilterLink *link, AVFrame *in, AVFrame **frame_out)
{
    AVFilterContext *ctx = link->dst;
    ScaleContext *scale = ctx->priv;
    AVFilterLink *outlink = ctx->outputs[0];
    AVFrame *out;
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(link->format);
    char buf[32];
    int in_range;
    int frame_changed;

    *frame_out = NULL;
    if (in->colorspace == AVCOL_SPC_YCGCO)
        av_log(link->dst, AV_LOG_WARNING, "Detected unsupported YCgCo colorspace.\n");

    frame_changed = in->width  != link->w ||
                    in->height != link->h ||
                    in->format != link->format ||
                    in->sample_aspect_ratio.den != link->sample_aspect_ratio.den ||
                    in->sample_aspect_ratio.num != link->sample_aspect_ratio.num;

    if (scale->eval_mode == EVAL_MODE_FRAME || frame_changed) {
        int ret;
        unsigned vars_w[VARS_NB] = { 0 }, vars_h[VARS_NB] = { 0 };

        av_expr_count_vars(scale->w_pexpr, vars_w, VARS_NB);
        av_expr_count_vars(scale->h_pexpr, vars_h, VARS_NB);

        if (scale->eval_mode == EVAL_MODE_FRAME &&
            !frame_changed &&
            ctx->filter != &ff_vf_scale2ref &&
            !(vars_w[VAR_N] || vars_w[VAR_T] || vars_w[VAR_POS]) &&
            !(vars_h[VAR_N] || vars_h[VAR_T] || vars_h[VAR_POS]) &&
            scale->w && scale->h)
            goto scale;

        if (scale->eval_mode == EVAL_MODE_INIT) {
            snprintf(buf, sizeof(buf)-1, "%d", outlink->w);
            av_opt_set(scale, "w", buf, 0);
            snprintf(buf, sizeof(buf)-1, "%d", outlink->h);
            av_opt_set(scale, "h", buf, 0);

            ret = scale_parse_expr(ctx, NULL, &scale->w_pexpr, "width", scale->w_expr);
            if (ret < 0)
                return ret;

            ret = scale_parse_expr(ctx, NULL, &scale->h_pexpr, "height", scale->h_expr);
            if (ret < 0)
                return ret;
        }

        if (ctx->filter == &ff_vf_scale2ref) {
            scale->var_values[VAR_S2R_MAIN_N] = link->frame_count_out;
            scale->var_values[VAR_S2R_MAIN_T] = TS2T(in->pts, link->time_base);
            scale->var_values[VAR_S2R_MAIN_POS] = in->pkt_pos == -1 ? NAN : in->pkt_pos;
        } else {
            scale->var_values[VAR_N] = link->frame_count_out;
            scale->var_values[VAR_T] = TS2T(in->pts, link->time_base);
            scale->var_values[VAR_POS] = in->pkt_pos == -1 ? NAN : in->pkt_pos;
        }

        link->dst->inputs[0]->format = in->format;
        link->dst->inputs[0]->w      = in->width;
        link->dst->inputs[0]->h      = in->height;

        link->dst->inputs[0]->sample_aspect_ratio.den = in->sample_aspect_ratio.den;
        link->dst->inputs[0]->sample_aspect_ratio.num = in->sample_aspect_ratio.num;

        if ((ret = config_props(outlink)) < 0)
            return ret;
    }

scale:
    if (!scale->sws) {
        *frame_out = in;
        return 0;
    }

    scale->hsub = desc->log2_chroma_w;
    scale->vsub = desc->log2_chroma_h;

    out = ff_get_video_buffer(outlink, outlink->w, outlink->h);
    if (!out) {
        av_frame_free(&in);
        return AVERROR(ENOMEM);
    }
    *frame_out = out;

    av_frame_copy_props(out, in);
    out->width  = outlink->w;
    out->height = outlink->h;

    if (scale->output_is_pal)
        avpriv_set_systematic_pal2((uint32_t*)out->data[1], outlink->format == AV_PIX_FMT_PAL8 ? AV_PIX_FMT_BGR8 : outlink->format);

    in_range = in->color_range;

    if (   scale->in_color_matrix
        || scale->out_color_matrix
        || scale-> in_range != AVCOL_RANGE_UNSPECIFIED
        || in_range != AVCOL_RANGE_UNSPECIFIED
        || scale->out_range != AVCOL_RANGE_UNSPECIFIED) {
        int in_full, out_full, brightness, contrast, saturation;
        const int *inv_table, *table;

        sws_getColorspaceDetails(scale->sws, (int **)&inv_table, &in_full,
                                 (int **)&table, &out_full,
                                 &brightness, &contrast, &saturation);

        if (scale->in_color_matrix)
            inv_table = parse_yuv_type(scale->in_color_matrix, in->colorspace);
        if (scale->out_color_matrix)
            table     = parse_yuv_type(scale->out_color_matrix, AVCOL_SPC_UNSPECIFIED);
        else if (scale->in_color_matrix)
            table = inv_table;

        if (scale-> in_range != AVCOL_RANGE_UNSPECIFIED)
            in_full  = (scale-> in_range == AVCOL_RANGE_JPEG);
        else if (in_range != AVCOL_RANGE_UNSPECIFIED)
            in_full  = (in_range == AVCOL_RANGE_JPEG);
        if (scale->out_range != AVCOL_RANGE_UNSPECIFIED)
            out_full = (scale->out_range == AVCOL_RANGE_JPEG);

        sws_setColorspaceDetails(scale->sws, inv_table, in_full,
                                 table, out_full,
                                 brightness, contrast, saturation);
        if (scale->isws[0])
            sws_setColorspaceDetails(scale->isws[0], inv_table, in_full,
                                     table, out_full,
                                     brightness, contrast, saturation);
        if (scale->isws[1])
            sws_setColorspaceDetails(scale->isws[1], inv_table, in_full,
                                     table, out_full,
                                     brightness, contrast, saturation);

        out->color_range = out_full ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
    }

    av_reduce(&out->sample_aspect_ratio.num, &out->sample_aspect_ratio.den,
              (int64_t)in->sample_aspect_ratio.num * outlink->h * link->w,
              (int64_t)in->sample_aspect_ratio.den * outlink->w * link->h,
              INT_MAX);

    if (scale->interlaced>0 || (scale->interlaced<0 && in->interlaced_frame)) {
        scale_slice(link, out, in, scale->isws[0], 0, (link->h+1)/2, 2, 0);
        scale_slice(link, out, in, scale->isws[1], 0,  link->h   /2, 2, 1);
    } else if (scale->nb_slices) {
        int i, slice_h, slice_start, slice_end = 0;
        const int nb_slices = FFMIN(scale->nb_slices, link->h);
        for (i = 0; i < nb_slices; i++) {
            slice_start = slice_end;
            slice_end   = (link->h * (i+1)) / nb_slices;
            slice_h     = slice_end - slice_start;
            scale_slice(link, out, in, scale->sws, slice_start, slice_h, 1, 0);
        }
    } else {
        if(scale_slice(link, out, in, scale->sws, 0, link->h, 1, 0) == AVERROR(EINVAL)) {
            av_frame_free(&in);
            av_frame_free(&out);
            *frame_out = NULL;
            return AVERROR(EINVAL);
        }
    }

    av_frame_free(&in);
    return 0;
}

static int filter_frame(AVFilterLink *link, AVFrame *in)
{
    AVFilterContext *ctx = link->dst;
    AVFilterLink *outlink = ctx->outputs[0];
    AVFrame *out;
    int ret;

    ret = scale_frame(link, in, &out);
    if (out)
        return ff_filter_frame(outlink, out);

    return ret;
}

static int filter_frame_ref(AVFilterLink *link, AVFrame *in)
{
    ScaleContext *scale = link->dst->priv;
    AVFilterLink *outlink = link->dst->outputs[1];
    int frame_changed;

    frame_changed = in->width  != link->w ||
                    in->height != link->h ||
                    in->format != link->format ||
                    in->sample_aspect_ratio.den != link->sample_aspect_ratio.den ||
                    in->sample_aspect_ratio.num != link->sample_aspect_ratio.num;

    if (frame_changed) {
        link->format = in->format;
        link->w = in->width;
        link->h = in->height;
        link->sample_aspect_ratio.num = in->sample_aspect_ratio.num;
        link->sample_aspect_ratio.den = in->sample_aspect_ratio.den;

        config_props_ref(outlink);
    }

    if (scale->eval_mode == EVAL_MODE_FRAME) {
        scale->var_values[VAR_N] = link->frame_count_out;
        scale->var_values[VAR_T] = TS2T(in->pts, link->time_base);
        scale->var_values[VAR_POS] = in->pkt_pos == -1 ? NAN : in->pkt_pos;
    }

    return ff_filter_frame(outlink, in);
}

static int process_command(AVFilterContext *ctx, const char *cmd, const char *args,
                           char *res, int res_len, int flags)
{
    ScaleContext *scale = ctx->priv;
    char *str_expr;
    AVExpr **pexpr_ptr;
    int ret, w, h;

    w = !strcmp(cmd, "width")  || !strcmp(cmd, "w");
    h = !strcmp(cmd, "height")  || !strcmp(cmd, "h");

    if (w || h) {
        str_expr = w ? scale->w_expr : scale->h_expr;
        pexpr_ptr = w ? &scale->w_pexpr : &scale->h_pexpr;

        ret = scale_parse_expr(ctx, str_expr, pexpr_ptr, cmd, args);
    } else
        ret = AVERROR(ENOSYS);

    if (ret < 0)
        av_log(ctx, AV_LOG_ERROR, "Failed to process command. Continuing with existing parameters.\n");

    return ret;
}

#if FF_API_CHILD_CLASS_NEXT
static const AVClass *child_class_next(const AVClass *prev)
{
    return prev ? NULL : sws_get_class();
}
#endif

static const AVClass *child_class_iterate(void **iter)
{
    const AVClass *c = *iter ? NULL : sws_get_class();
    *iter = (void*)(uintptr_t)c;
    return c;
}

#define OFFSET(x) offsetof(ScaleContext, x)
#define FLAGS AV_OPT_FLAG_VIDEO_PARAM|AV_OPT_FLAG_FILTERING_PARAM
#define TFLAGS AV_OPT_FLAG_VIDEO_PARAM|AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_RUNTIME_PARAM

static const AVOption scale_options[] = {
    { "w",     "Output video width",          OFFSET(w_expr),    AV_OPT_TYPE_STRING,        .flags = TFLAGS },
    { "width", "Output video width",          OFFSET(w_expr),    AV_OPT_TYPE_STRING,        .flags = TFLAGS },
    { "h",     "Output video height",         OFFSET(h_expr),    AV_OPT_TYPE_STRING,        .flags = TFLAGS },
    { "height","Output video height",         OFFSET(h_expr),    AV_OPT_TYPE_STRING,        .flags = TFLAGS },
    { "flags", "Flags to pass to libswscale", OFFSET(flags_str), AV_OPT_TYPE_STRING, { .str = "bilinear" }, .flags = FLAGS },
    { "interl", "set interlacing", OFFSET(interlaced), AV_OPT_TYPE_BOOL, {.i64 = 0 }, -1, 1, FLAGS },
    { "size",   "set video size",          OFFSET(size_str), AV_OPT_TYPE_STRING, {.str = NULL}, 0, FLAGS },
    { "s",      "set video size",          OFFSET(size_str), AV_OPT_TYPE_STRING, {.str = NULL}, 0, FLAGS },
    {  "in_color_matrix", "set input YCbCr type",   OFFSET(in_color_matrix),  AV_OPT_TYPE_STRING, { .str = "auto" }, .flags = FLAGS, "color" },
    { "out_color_matrix", "set output YCbCr type",  OFFSET(out_color_matrix), AV_OPT_TYPE_STRING, { .str = NULL }, .flags = FLAGS,  "color"},
        { "auto",        NULL, 0, AV_OPT_TYPE_CONST, { .str = "auto" },      0, 0, FLAGS, "color" },
        { "bt601",       NULL, 0, AV_OPT_TYPE_CONST, { .str = "bt601" },     0, 0, FLAGS, "color" },
        { "bt470",       NULL, 0, AV_OPT_TYPE_CONST, { .str = "bt470" },     0, 0, FLAGS, "color" },
        { "smpte170m",   NULL, 0, AV_OPT_TYPE_CONST, { .str = "smpte170m" }, 0, 0, FLAGS, "color" },
        { "bt709",       NULL, 0, AV_OPT_TYPE_CONST, { .str = "bt709" },     0, 0, FLAGS, "color" },
        { "fcc",         NULL, 0, AV_OPT_TYPE_CONST, { .str = "fcc" },       0, 0, FLAGS, "color" },
        { "smpte240m",   NULL, 0, AV_OPT_TYPE_CONST, { .str = "smpte240m" }, 0, 0, FLAGS, "color" },
        { "bt2020",      NULL, 0, AV_OPT_TYPE_CONST, { .str = "bt2020" },    0, 0, FLAGS, "color" },
    {  "in_range", "set input color range",  OFFSET( in_range), AV_OPT_TYPE_INT, {.i64 = AVCOL_RANGE_UNSPECIFIED }, 0, 2, FLAGS, "range" },
    { "out_range", "set output color range", OFFSET(out_range), AV_OPT_TYPE_INT, {.i64 = AVCOL_RANGE_UNSPECIFIED }, 0, 2, FLAGS, "range" },
    { "auto",   NULL, 0, AV_OPT_TYPE_CONST, {.i64 = AVCOL_RANGE_UNSPECIFIED }, 0, 0, FLAGS, "range" },
    { "unknown", NULL, 0, AV_OPT_TYPE_CONST, {.i64 = AVCOL_RANGE_UNSPECIFIED }, 0, 0, FLAGS, "range" },
    { "full",   NULL, 0, AV_OPT_TYPE_CONST, {.i64 = AVCOL_RANGE_JPEG}, 0, 0, FLAGS, "range" },
    { "limited",NULL, 0, AV_OPT_TYPE_CONST, {.i64 = AVCOL_RANGE_MPEG}, 0, 0, FLAGS, "range" },
    { "jpeg",   NULL, 0, AV_OPT_TYPE_CONST, {.i64 = AVCOL_RANGE_JPEG}, 0, 0, FLAGS, "range" },
    { "mpeg",   NULL, 0, AV_OPT_TYPE_CONST, {.i64 = AVCOL_RANGE_MPEG}, 0, 0, FLAGS, "range" },
    { "tv",     NULL, 0, AV_OPT_TYPE_CONST, {.i64 = AVCOL_RANGE_MPEG}, 0, 0, FLAGS, "range" },
    { "pc",     NULL, 0, AV_OPT_TYPE_CONST, {.i64 = AVCOL_RANGE_JPEG}, 0, 0, FLAGS, "range" },
    { "in_v_chr_pos",   "input vertical chroma position in luma grid/256"  ,   OFFSET(in_v_chr_pos),  AV_OPT_TYPE_INT, { .i64 = -513}, -513, 512, FLAGS },
    { "in_h_chr_pos",   "input horizontal chroma position in luma grid/256",   OFFSET(in_h_chr_pos),  AV_OPT_TYPE_INT, { .i64 = -513}, -513, 512, FLAGS },
    { "out_v_chr_pos",   "output vertical chroma position in luma grid/256"  , OFFSET(out_v_chr_pos), AV_OPT_TYPE_INT, { .i64 = -513}, -513, 512, FLAGS },
    { "out_h_chr_pos",   "output horizontal chroma position in luma grid/256", OFFSET(out_h_chr_pos), AV_OPT_TYPE_INT, { .i64 = -513}, -513, 512, FLAGS },
    { "force_original_aspect_ratio", "decrease or increase w/h if necessary to keep the original AR", OFFSET(force_original_aspect_ratio), AV_OPT_TYPE_INT, { .i64 = 0}, 0, 2, FLAGS, "force_oar" },
    { "disable",  NULL, 0, AV_OPT_TYPE_CONST, {.i64 = 0 }, 0, 0, FLAGS, "force_oar" },
    { "decrease", NULL, 0, AV_OPT_TYPE_CONST, {.i64 = 1 }, 0, 0, FLAGS, "force_oar" },
    { "increase", NULL, 0, AV_OPT_TYPE_CONST, {.i64 = 2 }, 0, 0, FLAGS, "force_oar" },
    { "force_divisible_by", "enforce that the output resolution is divisible by a defined integer when force_original_aspect_ratio is used", OFFSET(force_divisible_by), AV_OPT_TYPE_INT, { .i64 = 1}, 1, 256, FLAGS },
    { "param0", "Scaler param 0",             OFFSET(param[0]),  AV_OPT_TYPE_DOUBLE, { .dbl = SWS_PARAM_DEFAULT  }, INT_MIN, INT_MAX, FLAGS },
    { "param1", "Scaler param 1",             OFFSET(param[1]),  AV_OPT_TYPE_DOUBLE, { .dbl = SWS_PARAM_DEFAULT  }, INT_MIN, INT_MAX, FLAGS },
    { "nb_slices", "set the number of slices (debug purpose only)", OFFSET(nb_slices), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, INT_MAX, FLAGS },
    { "eval", "specify when to evaluate expressions", OFFSET(eval_mode), AV_OPT_TYPE_INT, {.i64 = EVAL_MODE_INIT}, 0, EVAL_MODE_NB-1, FLAGS, "eval" },
         { "init",  "eval expressions once during initialization", 0, AV_OPT_TYPE_CONST, {.i64=EVAL_MODE_INIT},  .flags = FLAGS, .unit = "eval" },
         { "frame", "eval expressions during initialization and per-frame", 0, AV_OPT_TYPE_CONST, {.i64=EVAL_MODE_FRAME}, .flags = FLAGS, .unit = "eval" },
    { NULL }
};

static const AVClass scale_class = {
    .class_name       = "scale",
    .item_name        = av_default_item_name,
    .option           = scale_options,
    .version          = LIBAVUTIL_VERSION_INT,
    .category         = AV_CLASS_CATEGORY_FILTER,
#if FF_API_CHILD_CLASS_NEXT
    .child_class_next = child_class_next,
#endif
    .child_class_iterate = child_class_iterate,
};

static const AVFilterPad avfilter_vf_scale_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = filter_frame,
    },
    { NULL }
};

static const AVFilterPad avfilter_vf_scale_outputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = config_props,
    },
    { NULL }
};

AVFilter ff_vf_scale = {
    .name            = "scale",
    .description     = NULL_IF_CONFIG_SMALL("Scale the input video size and/or convert the image format."),
    .init_dict       = init_dict,
    .uninit          = uninit,
    .query_formats   = query_formats,
    .priv_size       = sizeof(ScaleContext),
    .priv_class      = &scale_class,
    .inputs          = avfilter_vf_scale_inputs,
    .outputs         = avfilter_vf_scale_outputs,
    .process_command = process_command,
};

static const AVClass scale2ref_class = {
    .class_name       = "scale2ref",
    .item_name        = av_default_item_name,
    .option           = scale_options,
    .version          = LIBAVUTIL_VERSION_INT,
    .category         = AV_CLASS_CATEGORY_FILTER,
#if FF_API_CHILD_CLASS_NEXT
    .child_class_next = child_class_next,
#endif
    .child_class_iterate = child_class_iterate,
};

static const AVFilterPad avfilter_vf_scale2ref_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = filter_frame,
    },
    {
        .name         = "ref",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = filter_frame_ref,
    },
    { NULL }
};

static const AVFilterPad avfilter_vf_scale2ref_outputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = config_props,
        .request_frame= request_frame,
    },
    {
        .name         = "ref",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = config_props_ref,
        .request_frame= request_frame_ref,
    },
    { NULL }
};

AVFilter ff_vf_scale2ref = {
    .name            = "scale2ref",
    .description     = NULL_IF_CONFIG_SMALL("Scale the input video size and/or convert the image format to the given reference."),
    .init_dict       = init_dict,
    .uninit          = uninit,
    .query_formats   = query_formats,
    .priv_size       = sizeof(ScaleContext),
    .priv_class      = &scale2ref_class,
    .inputs          = avfilter_vf_scale2ref_inputs,
    .outputs         = avfilter_vf_scale2ref_outputs,
    .process_command = process_command,
};
