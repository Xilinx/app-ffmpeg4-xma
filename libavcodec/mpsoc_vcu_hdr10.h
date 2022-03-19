/*
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

#ifndef MPSOC_VCU_HDR10
#define MPSOC_VCU_HDR10

#include <sys/types.h>

//HDR10 VUI parameters
typedef struct HDR10_VUI_Params
{
    char    ColorDesc[30];
    char    TxChar[30];
    char    ColorMatrix[30];
    uint8_t isInitialized;
}HDR10_VUI_Params;

//Global singleton for HDR VUI data, that is populated by the decoder
//and can be accessed by any element in transcode pipeline
static HDR10_VUI_Params g_hdr10_vui_params;

void init_hdr10_vui_params();
void print_hdr10_vui_params();
HDR10_VUI_Params* get_hdr10_vui_params();

#endif /* MPSOC_VCU_HDR10 */
