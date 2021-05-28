/*
* Copyright (c) 2020 Xilinx Inc
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

#ifndef _XMAPROPSTOJSON_H_
#define _XMAPROPSTOJSON_H_

//#include <string.h>
#include <stdio.h>
#include <syslog.h>
//#include <vector>
//#include <tuple>
//#include <string>

#include "/opt/xilinx/xrm/include/xrm.h"
#include "/opt/xilinx/xrm/include/xrm_error.h"
#include "/opt/xilinx/xrm/include/xrm_limits.h"
#include <xma.h>

#define MAX_CH_SIZE 4096

#ifdef __cplusplus
extern "C" {
#endif

  void convertDecPropsToJson(void* props, char* funcName, char* jsonJob);
  void convertDecPropsToJson1(XmaDecoderProperties* props, char* funcName, char* jsonJob);

#ifdef __cplusplus
}
#endif

#endif //_XRM_U30_CALC_PERCENT_PLUGIN_HPP_
