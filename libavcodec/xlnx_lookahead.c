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

#include <stdlib.h>
#include <string.h>
#include "xlnx_lookahead.h"
#include "libavutil/internal.h"
#include "xvbm.h"
#include <xrm.h>
#include <dlfcn.h>
#include <errno.h>
#include "../xmaPropsTOjson.h"

//From #include "xlnx_la_plg_ext.h"
#define XLNX_LA_PLG_NUM_EXT_PARAMS 10
#define XRM_PRECISION_1000000_BIT_MASK(load) ((load << 8))

typedef enum
{
    EParamIntraPeriod,
    EParamLADepth,
    EParamEnableHwInBuf,
    EParamSpatialAQMode,
    EParamTemporalAQMode,
    EParamRateControlMode,
    EParamSpatialAQGain,
    EParamNumBFrames,
    EParamCodecType,
    EParamLatencyLogging
} xlnx_la_ext_params_t;

static const char *XLNX_LA_EXT_PARAMS[] = {
    "ip",
    "lookahead_depth",
    "enable_hw_in_buf",
    "spatial_aq_mode",
    "temporal_aq_mode",
    "rate_control_mode",
    "spatial_aq_gain",
    "num_b_frames",
    "codec_type",
    "latency_logging"
};

///////////////////////////////////////////////////////////////////////////////

#define SCLEVEL1 2
#define XLNX_MAX_LOOKAHEAD_DEPTH 20
#define XLNX_ALIGN(x,LINE_SIZE) (((((size_t)x) + ((size_t)LINE_SIZE - 1)) & (~((size_t)LINE_SIZE - 1))))

static const char *XLNX_LOOKAHEAD_NAME = "xlnx_lookahead";

#define XLNX_LA_LOG(LOG_TYPE, ...)                               \
    do {                                                         \
        xma_logmsg(LOG_TYPE, XLNX_LOOKAHEAD_NAME, __VA_ARGS__);  \
    } while (0)

typedef struct xlnx_la_ctx
{
    XmaFilterSession *filter_session;
    uint8_t           bypass;
    uint32_t          enableHwInBuf;
    uint32_t          lookahead_depth;
    uint32_t          spatial_aq_mode;
    uint32_t          temporal_aq_mode;
    uint32_t          rate_control_mode;
    uint32_t          spatial_aq_gain;
    XmaFormatType     fmt_type;
    xlnx_codec_type_t codec_type;
    XmaParameter      extn_params[XLNX_LA_PLG_NUM_EXT_PARAMS];
    XmaFrame         *out_frame;
    xrmContext       *xrm_ctx;
    xrmCuResourceV2   lookahead_cu_res;
    bool              lookahead_res_inuse;
    int               lxlnx_hwdev;
} xlnx_la_ctx;

static void free_frame(XmaFrame *xframe)
{
    XvbmBufferHandle handle;
    int32_t num_planes;
    if (xframe == NULL) {
        return;
    }
    if (xframe->data[0].buffer_type == XMA_DEVICE_BUFFER_TYPE) {
        handle = (XvbmBufferHandle)(xframe->data[0].buffer);
        if (handle) {
            xvbm_buffer_pool_entry_free(handle);
        }
        xframe->data[0].buffer = NULL;
    } else {
        num_planes = xma_frame_planes_get(&xframe->frame_props);

        for (int32_t i = 0; i < num_planes; i++) {
            xframe->data[i].refcount--;
        }

        if (xframe->data[0].refcount > 0) {
            return;
        }

        for (int32_t i = 0; i < num_planes && !xframe->data[i].is_clone; i++) {
            if (xframe->data[i].buffer) {
                free(xframe->data[i].buffer);
                xframe->data[i].buffer = NULL;
            }
        }
    }
    xma_frame_clear_all_side_data(xframe);
    free(xframe);
}

static int32_t free_res(xlnx_la_ctx *la_ctx)
{
    char* endptr;

    if (!la_ctx) {
        XLNX_LA_LOG(XMA_ERROR_LOG, "free_res : free_res la_ctx = NULL\n");
        return XMA_ERROR;
    }

    // Close lookahead session
    if (la_ctx->filter_session) {
        xma_filter_session_destroy(la_ctx->filter_session);
        la_ctx->filter_session = NULL;
    }
    free_frame(la_ctx->out_frame);
    la_ctx->out_frame = NULL;

    if (getenv("XRM_RESERVE_ID")) {
        errno=0;
        int xrm_reserve_id =  strtol(getenv("XRM_RESERVE_ID"), &endptr, 0);      
        if (errno != 0)
        {
           av_log(NULL, AV_LOG_ERROR, "Fail to use XRM_RESERVE_ID in lookahead plugin\n");
           return -1;
        }

        //XRM lookahead de-allocation
        if (la_ctx->lookahead_res_inuse) {
            if (!(xrmCuReleaseV2(la_ctx->xrm_ctx, &la_ctx->lookahead_cu_res))) {
                av_log(NULL, AV_LOG_ERROR, "XRM: failed to release lookahead resources\n");
            }
            if (xrmDestroyContext(la_ctx->xrm_ctx) != XRM_SUCCESS) {
                av_log(NULL, AV_LOG_ERROR, "XRM : lookahead destroy context failed\n");
            }
        }
    }

    return XMA_SUCCESS;
}

static int _calc_la_load(xrmContext *xrm_ctx, XmaFilterProperties *filter_props,
                         int32_t func_id, int32_t *la_load)
{
    char pluginName[XRM_MAX_NAME_LEN];
    int skip_value=0;
    xrmPluginFuncParam param;
    char *err;
    void *handle;
    void (*convertXmaPropsToJson)(void *props, char *funcName, char *jsonJob);

    memset(&param, 0, sizeof(xrmPluginFuncParam));
    handle = dlopen("/opt/xilinx/xrm/plugin/libxmaPropsTOjson.so", RTLD_NOW );
    if (!handle) {
        av_log(NULL, AV_LOG_ERROR, "Unable to load libxmaPropsTOjson.so  - %s\n",
               dlerror());
        return XMA_ERROR;
    }
    dlerror(); /* clear error code */

    convertXmaPropsToJson = dlsym(handle, "convertXmaPropsToJson");
    if ((err = dlerror()) != NULL) {
        av_log(NULL, AV_LOG_ERROR, "convertXmaPropsToJson symbol not found\n");
        return XMA_ERROR;
    }

    (*convertXmaPropsToJson) (filter_props, "LOOKAHEAD", param.input);
    dlclose(handle);

    strcpy(pluginName, "xrmU30EncPlugin");
    if (xrmExecPluginFunc(xrm_ctx, pluginName, func_id, &param) != XRM_SUCCESS) {
        av_log(NULL, AV_LOG_ERROR,
               "xrm_load_calculation: lookahead plugin function %d, failed to run the function\n",
               func_id);
        return XMA_ERROR;
    } else {
        skip_value = atoi((char *)(strtok(param.output, " ")));
        skip_value = atoi((char *)(strtok(NULL, " ")));
        *la_load = atoi((char *)(strtok(NULL, " ")));

        if (*la_load <= 0) {
            av_log(NULL, AV_LOG_ERROR,
                   "xrm_load_calculation: enc plugin function %d, calculated wrong lookahead load %d .\n",
                   *la_load);
            return XMA_ERROR;
        } else if (*la_load > XRM_MAX_CU_LOAD_GRANULARITY_1000000) {
            av_log(NULL, AV_LOG_ERROR,
                   "xrm_load_calculation: enc plugin function %d, calculated lookahead load %d is greater than maximum supported.\n",
                   *la_load);
            return XMA_ERROR;
        }
    }

    return 0;
}

static int _allocate_xrm_la_cu(xlnx_la_ctx *ctx,
                               XmaFilterProperties *filter_props)
{
    int xrm_reserve_id = -1;
    int ret =-1;
    char pluginName[XRM_MAX_NAME_LEN];
    uint64_t deviceInfoDeviceIndex = 0;
    uint64_t deviceInfoContraintType = XRM_DEVICE_INFO_CONSTRAINT_TYPE_HARDWARE_DEVICE_INDEX;
    char* endptr;    

    //create XRM local context
    ctx->xrm_ctx = (xrmContext *)xrmCreateContext(XRM_API_VERSION_1);
    if (ctx->xrm_ctx == NULL) {
        av_log(NULL, AV_LOG_ERROR, "create local XRM context failed\n");
        return XMA_ERROR;
    }

    //XRM encoder plugin load calculation
    int32_t func_id = 0, la_load=0;
    ret = _calc_la_load(ctx->xrm_ctx, filter_props, func_id, &la_load);
    if (ret < 0) {
        return ret;
    }
	
    //XRM lookahead allocation
    xrmCuPropertyV2 lookahead_cu_prop;

    memset(&lookahead_cu_prop, 0, sizeof(xrmCuPropertyV2));
    memset(&ctx->lookahead_cu_res, 0, sizeof(xrmCuResourceV2));

    strcpy(lookahead_cu_prop.kernelName, "lookahead");
    strcpy(lookahead_cu_prop.kernelAlias, "LOOKAHEAD_MPSOC");

    if (getenv("XRM_RESERVE_ID")) {
       errno = 0;
       xrm_reserve_id =  strtol(getenv("XRM_RESERVE_ID"), &endptr, 0);     
       if (errno != 0)
       {
          av_log(NULL, AV_LOG_ERROR, "Fail to use XRM_RESERVE_ID in lookahead plugin\n");
          return -1;
       }           
    }
	 
    lookahead_cu_prop.devExcl = false;
    lookahead_cu_prop.requestLoad = XRM_PRECISION_1000000_BIT_MASK(la_load);

    if ((ctx->lxlnx_hwdev > -1) && (xrm_reserve_id > -1)) //2dev mode launcher
    {
        deviceInfoDeviceIndex = ctx->lxlnx_hwdev;
        lookahead_cu_prop.deviceInfo = (deviceInfoDeviceIndex << XRM_DEVICE_INFO_DEVICE_INDEX_SHIFT) | (deviceInfoContraintType << XRM_DEVICE_INFO_CONSTRAINT_TYPE_SHIFT);
        lookahead_cu_prop.poolId = xrm_reserve_id;
    }
    else if (xrm_reserve_id > -1) //1dev mode launcher
    {
        lookahead_cu_prop.poolId = xrm_reserve_id;	
    }		
    else if (ctx->lxlnx_hwdev > -1) //explicit ffmpeg local device command
    {
        deviceInfoDeviceIndex = ctx->lxlnx_hwdev;
        lookahead_cu_prop.deviceInfo = (deviceInfoDeviceIndex << XRM_DEVICE_INFO_DEVICE_INDEX_SHIFT) | (deviceInfoContraintType << XRM_DEVICE_INFO_CONSTRAINT_TYPE_SHIFT);	
    }
    else 
    {
        errno=0;    
        deviceInfoDeviceIndex =  strtol(getenv("XRM_DEVICE_ID"), &endptr, 0);      
        if (errno != 0)
        {
            av_log(NULL, AV_LOG_ERROR, "Fail to use XRM_DEVICE_ID in lookahead plugin\n");
            return -1;
        }

        lookahead_cu_prop.deviceInfo = (deviceInfoDeviceIndex << XRM_DEVICE_INFO_DEVICE_INDEX_SHIFT) | (deviceInfoContraintType << XRM_DEVICE_INFO_CONSTRAINT_TYPE_SHIFT);	
    }

    ret = xrmCuAllocV2(ctx->xrm_ctx, &lookahead_cu_prop, &ctx->lookahead_cu_res);

    if (ret != 0) {
        av_log(NULL, AV_LOG_ERROR,
                "xrm_allocation: failed to allocate lookahead resources from reserve id=%d or device=%d\n",
                 xrm_reserve_id, deviceInfoDeviceIndex);
        return XMA_ERROR;
    } else {
        ctx->lookahead_res_inuse = true;
    }
     
    //Set XMA plugin SO and device index
    filter_props->plugin_lib = ctx->lookahead_cu_res.kernelPluginFileName;
    filter_props->dev_index = ctx->lookahead_cu_res.deviceId;
    filter_props->ddr_bank_index =
        -1;//XMA to select the ddr bank based on xclbin meta data
    filter_props->cu_index = ctx->lookahead_cu_res.cuId;
    filter_props->channel_id = ctx->lookahead_cu_res.channelId;

    av_log(NULL, AV_LOG_DEBUG,
           "---lookahead xrm out: la_load=%d, plugin=%s, device=%d, cu=%d, ch=%d  \n",
           la_load, filter_props->plugin_lib, filter_props->dev_index,
           filter_props->cu_index, filter_props->channel_id);

    return ret;
}

xlnx_lookahead_t create_xlnx_la(xlnx_la_cfg_t *cfg)
{
    XmaFilterProperties filter_props;
    XmaFilterPortProperties *in_props;
    XmaFilterPortProperties *out_props;
    uint32_t param_cnt = 0;
    //XmaFrameProperties f_in_props;
    //XmaFrameProperties f_out_props;
    xlnx_la_ctx *la_ctx;
    XmaParameter *extn_params = NULL;
    int ret = -1;

    if (!cfg) {
        XLNX_LA_LOG(XMA_ERROR_LOG, "No config received\n");
        return NULL;
    }
    if ((cfg->lookahead_depth == 0) && (cfg->temporal_aq_mode == 1)) {
        XLNX_LA_LOG(XMA_ERROR_LOG, "Invalid params: Lookahead = 0, temporal aq=%u\n",
                    cfg->temporal_aq_mode);
        return NULL;
    }

    la_ctx = calloc(1, sizeof(xlnx_la_ctx));
    if (!la_ctx) {
        XLNX_LA_LOG(XMA_ERROR_LOG, "OOM la_ctx\n");
        return NULL;
    }
    la_ctx->lookahead_depth = cfg->lookahead_depth;
    if ((cfg->lookahead_depth == 0) && (cfg->spatial_aq_mode == 0)) {
        la_ctx->bypass = 1;
        return la_ctx;
    }
    la_ctx->spatial_aq_mode = cfg->spatial_aq_mode;
    la_ctx->temporal_aq_mode = cfg->temporal_aq_mode;
    la_ctx->spatial_aq_gain = cfg->spatial_aq_gain;
    la_ctx->enableHwInBuf = cfg->enableHwInBuf;
    la_ctx->fmt_type = cfg->fmt_type;
    la_ctx->rate_control_mode = cfg->rate_control_mode;
    la_ctx->bypass = 0;
    la_ctx->codec_type = cfg->codec_type;
    la_ctx->lxlnx_hwdev = cfg->lxlnx_hwdev;

    // Setup lookahead properties
    //filter_props = &la_ctx->filter_props;
    memset(&filter_props, 0, sizeof(XmaFilterProperties));
    filter_props.hwfilter_type = XMA_2D_FILTER_TYPE;
    strcpy(filter_props.hwvendor_string, "Xilinx");

    // Setup lookahead input port properties
    in_props = &filter_props.input;
    memset(in_props, 0, sizeof(XmaFilterPortProperties));
    in_props->format = cfg->fmt_type;
    in_props->bits_per_pixel = cfg->bits_per_pixel;
    in_props->width = cfg->width;
    in_props->height = cfg->height;
    in_props->stride = cfg->stride;
    in_props->framerate.numerator = cfg->framerate.numerator;
    in_props->framerate.denominator = cfg->framerate.denominator;

    // Setup lookahead output port properties
    out_props = &filter_props.output;
    memset(out_props, 0, sizeof(XmaFilterPortProperties));
    out_props->format = cfg->fmt_type;
    out_props->bits_per_pixel = cfg->bits_per_pixel;
    out_props->width = XLNX_ALIGN((in_props->width), 64)>>SCLEVEL1;
    out_props->height = XLNX_ALIGN((in_props->height), 64)>>SCLEVEL1;
    out_props->framerate.numerator = cfg->framerate.numerator;
    out_props->framerate.denominator = cfg->framerate.denominator;

    extn_params = &la_ctx->extn_params[0];
    extn_params[param_cnt].name = (char *)XLNX_LA_EXT_PARAMS[EParamIntraPeriod];
    extn_params[param_cnt].user_type = EParamIntraPeriod;
    extn_params[param_cnt].type = XMA_UINT32;
    extn_params[param_cnt].length = sizeof(int);
    extn_params[param_cnt].value = &cfg->gop_size;
    param_cnt++;

    extn_params[param_cnt].name = (char *)XLNX_LA_EXT_PARAMS[EParamLADepth];
    extn_params[param_cnt].user_type = EParamLADepth;
    extn_params[param_cnt].type = XMA_UINT32;
    extn_params[param_cnt].length = sizeof(int);
    extn_params[param_cnt].value = &la_ctx->lookahead_depth;
    param_cnt++;

    extn_params[param_cnt].name = (char *)XLNX_LA_EXT_PARAMS[EParamEnableHwInBuf];
    extn_params[param_cnt].user_type = EParamEnableHwInBuf;
    extn_params[param_cnt].type = XMA_UINT32;
    extn_params[param_cnt].length = sizeof(int);
    extn_params[param_cnt].value = &la_ctx->enableHwInBuf;
    param_cnt++;

    extn_params[param_cnt].name = (char *)XLNX_LA_EXT_PARAMS[EParamSpatialAQMode];
    extn_params[param_cnt].user_type = EParamSpatialAQMode;
    extn_params[param_cnt].type = XMA_UINT32;
    extn_params[param_cnt].length = sizeof(int);
    extn_params[param_cnt].value = &la_ctx->spatial_aq_mode;
    param_cnt++;

    extn_params[param_cnt].name = (char *)XLNX_LA_EXT_PARAMS[EParamTemporalAQMode];
    extn_params[param_cnt].user_type = EParamTemporalAQMode;
    extn_params[param_cnt].type = XMA_UINT32;
    extn_params[param_cnt].length = sizeof(int);
    extn_params[param_cnt].value = &la_ctx->temporal_aq_mode;
    param_cnt++;

    extn_params[param_cnt].name = (char *)XLNX_LA_EXT_PARAMS[EParamRateControlMode];
    extn_params[param_cnt].user_type = EParamRateControlMode;
    extn_params[param_cnt].type = XMA_UINT32;
    extn_params[param_cnt].length = sizeof(int);
    extn_params[param_cnt].value = &la_ctx->rate_control_mode;
    param_cnt++;

    extn_params[param_cnt].name = (char *)XLNX_LA_EXT_PARAMS[EParamSpatialAQGain];
    extn_params[param_cnt].user_type = EParamSpatialAQGain;
    extn_params[param_cnt].type = XMA_UINT32;
    extn_params[param_cnt].length = sizeof(int);
    extn_params[param_cnt].value = &la_ctx->spatial_aq_gain;
    param_cnt++;

    extn_params[param_cnt].name = (char *)XLNX_LA_EXT_PARAMS[EParamNumBFrames];
    extn_params[param_cnt].user_type = EParamNumBFrames;
    extn_params[param_cnt].type = XMA_UINT32;
    extn_params[param_cnt].length = sizeof(int);
    extn_params[param_cnt].value = &cfg->b_frames;
    param_cnt++;

    extn_params[param_cnt].name = (char *)XLNX_LA_EXT_PARAMS[EParamCodecType];
    extn_params[param_cnt].user_type = EParamCodecType;
    extn_params[param_cnt].type = XMA_UINT32;
    extn_params[param_cnt].length = sizeof(int);
    extn_params[param_cnt].value = &la_ctx->codec_type;
    param_cnt++;

    extn_params[param_cnt].name = (char *)XLNX_LA_EXT_PARAMS[EParamLatencyLogging];
    extn_params[param_cnt].user_type = EParamLatencyLogging;
    extn_params[param_cnt].type = XMA_UINT32;
    extn_params[param_cnt].length = sizeof(int);
    extn_params[param_cnt].value = &cfg->latency_logging;
    param_cnt++;

    filter_props.param_cnt = param_cnt;
    filter_props.params = &extn_params[0];

    /*----------------------------------------------------
      Allocate lookahead resource from XRM reserved resource
      ----------------------------------------------------*/
    la_ctx->lookahead_res_inuse = false;
    ret = _allocate_xrm_la_cu(la_ctx, &filter_props);
    if (ret < 0) {
        av_log(la_ctx, AV_LOG_ERROR, "xrm_allocation: resource allocation failed\n");
        return XMA_ERROR;
    }

    // Create lookahead session based on the requested properties
    la_ctx->filter_session = xma_filter_session_create(&filter_props);
    if (!la_ctx->filter_session) {
        XLNX_LA_LOG(XMA_ERROR_LOG, "Failed to create lookahead session\n");
        destroy_xlnx_la(la_ctx);
        return NULL;
    }
    la_ctx->out_frame = (XmaFrame *) calloc(1, sizeof(XmaFrame));
    if (la_ctx->out_frame == NULL) {
        XLNX_LA_LOG(XMA_ERROR_LOG, "OOM la_ctx\n");
        destroy_xlnx_la(la_ctx);
        return NULL;
    }
    return (xlnx_lookahead_t)la_ctx;
}

int32_t destroy_xlnx_la(xlnx_lookahead_t la)
{
    xlnx_la_ctx *la_ctx;
    if (!la) {
        return XMA_ERROR;
    }
    la_ctx = (xlnx_la_ctx *)la;
    if (la_ctx->bypass == 0) {
        free_res(la_ctx);
    }
    free(la_ctx);
    return XMA_SUCCESS;
}

static int32_t xlnx_la_send_frame(xlnx_la_ctx *la_ctx, XmaFrame *in_frame)
{
    int32_t rc;
    if (!la_ctx) {
        XLNX_LA_LOG(XMA_ERROR_LOG, "xlnx_la_send_frame : XMA_ERROR\n");
        return XMA_ERROR;
    }

    if (in_frame && in_frame->do_not_encode) {
        if (in_frame->data[0].buffer) {
            if (in_frame->data[0].buffer_type == XMA_DEVICE_BUFFER_TYPE) {
                XvbmBufferHandle handle = (XvbmBufferHandle)(in_frame->data[0].buffer);
                if (handle) {
                    xvbm_buffer_pool_entry_free(handle);
                }
            }
        }
        rc = XMA_SUCCESS;
    } else {
        rc = xma_filter_session_send_frame(la_ctx->filter_session,
                                           in_frame);
    }
    if (rc <= XMA_ERROR) {
        XLNX_LA_LOG(XMA_ERROR_LOG,
                    "xlnx_la_send_frame : Send frame to LA xma plg Failed!!\n");
        rc = XMA_ERROR;
    }
    return rc;
}

int32_t xlnx_la_send_recv_frame(xlnx_lookahead_t la, XmaFrame *in_frame,
                                XmaFrame **out_frame)
{
    int32_t ret = 0;
    xlnx_la_ctx *la_ctx = (xlnx_la_ctx *)la;
    if (out_frame == NULL) {
        return XMA_ERROR;
    }
    if (la_ctx->bypass == 1) {
        *out_frame = in_frame;
        return XMA_SUCCESS;
    }
    if (la_ctx->out_frame == NULL) {
        return XMA_ERROR;
    }

    ret = xlnx_la_send_frame(la, in_frame);
    switch (ret) {
        case XMA_SUCCESS:
            ret = xma_filter_session_recv_frame(la_ctx->filter_session, la_ctx->out_frame);
            if (ret == XMA_TRY_AGAIN) {
                ret = XMA_SEND_MORE_DATA;
            }
            break;
        case XMA_SEND_MORE_DATA:
            break;
        case XMA_TRY_AGAIN:
            // If the user is receiving output, this condition should not be hit.
            ret = xma_filter_session_recv_frame(la_ctx->filter_session, la_ctx->out_frame);
            if (ret == XMA_SUCCESS) {
                ret = xlnx_la_send_frame(la, in_frame);
            }
            break;
        case XMA_ERROR:
        default:
            *out_frame = NULL;
            break;
    }
    if (ret == XMA_SUCCESS) {
        *out_frame = la_ctx->out_frame;
        la_ctx->out_frame = NULL;
    }
    return ret;
}

int32_t xlnx_la_release_frame(xlnx_lookahead_t la, XmaFrame *received_frame)
{
    if (!la) {
        return XMA_ERROR;
    }

    xlnx_la_ctx *la_ctx = (xlnx_la_ctx *)la;
    if (la_ctx->bypass) {
        return XMA_SUCCESS;
    }
    if (!received_frame || la_ctx->out_frame) {
        return XMA_ERROR;
    }
    la_ctx->out_frame = received_frame;
    XmaSideDataHandle *side_data = la_ctx->out_frame->side_data;
    memset(la_ctx->out_frame, 0, sizeof(XmaFrame));
    la_ctx->out_frame->side_data = side_data;
    return XMA_SUCCESS;
}

int32_t xlnx_la_in_bypass_mode(xlnx_lookahead_t la)
{
    int32_t ret = 0;
    if (!la) {
        return XMA_ERROR;
    }
    xlnx_la_ctx *la_ctx = (xlnx_la_ctx *)la;
    ret = la_ctx->bypass;
    return ret;
}
