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

#ifndef XLNX_LOOKAHEAD_H
#define XLNX_LOOKAHEAD_H

#include <inttypes.h>
#include <xma.h>

typedef void *xlnx_lookahead_t;

typedef enum
{
    EXlnxAvc,
    EXlnxHevc
} xlnx_codec_type_t;

typedef struct
{
    int32_t            width; /**< width in pixels of data */
    int32_t            height; /**< height in pixels of data */
    int32_t            stride;
    int32_t            bits_per_pixel; /**< bits per pixel of video format */
    int32_t            gop_size;
    uint32_t           lookahead_depth;
    uint32_t           spatial_aq_mode;
    uint32_t           temporal_aq_mode;
    uint32_t           rate_control_mode;
    uint32_t           spatial_aq_gain;
    uint32_t           b_frames;
    XmaFormatType      fmt_type;
    XmaFraction        framerate;
    xlnx_codec_type_t  codec_type;
    uint8_t            enableHwInBuf;
    int32_t            latency_logging;
    int               lxlnx_hwdev;
} xlnx_la_cfg_t;

xlnx_lookahead_t create_xlnx_la(xlnx_la_cfg_t *cfg);
int32_t destroy_xlnx_la(xlnx_lookahead_t la);
int32_t xlnx_la_send_recv_frame(xlnx_lookahead_t la, XmaFrame *in_frame,
                                XmaFrame **out_frame);
int32_t xlnx_la_release_frame(xlnx_lookahead_t la, XmaFrame *received_frame);
int32_t xlnx_la_in_bypass_mode(xlnx_lookahead_t la);
#endif //XLNX_LOOKAHEAD_H
