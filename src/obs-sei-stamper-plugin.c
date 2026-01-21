/******************************************************************************
    OBS SEI Stamper Plugin
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
******************************************************************************/

#include <obs-module.h>
#include <util/platform.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-sei-stamper", "en-US")

// 模块描述
MODULE_EXPORT const char *obs_module_description(void) {
  return "SEI Stamper Plugin - Add NTP timestamp SEI to video streams for "
         "frame-level synchronization";
}

// 模块名称
MODULE_EXPORT const char *obs_module_name(void) { return "OBS SEI Stamper"; }

// 前向声明 - 统一编码器（三个独立的codec版本）
extern struct obs_encoder_info unified_encoder_info_h264;
extern struct obs_encoder_info unified_encoder_info_h265;
extern struct obs_encoder_info unified_encoder_info_av1;

// 前向声明 - 源
extern struct obs_source_info sei_receiver_source_info;

// 模块加载
bool obs_module_load(void) {
  blog(LOG_INFO, "OBS SEI Stamper Plugin loaded");

  /* 注册三个独立的SEI Stamper编码器（每种codec一个） */
  blog(LOG_INFO, "Registering SEI Stamper H.264 encoder");
  obs_register_encoder(&unified_encoder_info_h264);

  blog(LOG_INFO, "Registering SEI Stamper H.265 encoder");
  obs_register_encoder(&unified_encoder_info_h265);

  blog(LOG_INFO, "Registering SEI Stamper AV1 encoder");
  obs_register_encoder(&unified_encoder_info_av1);

  /* 注册SEI接收器源 */
  blog(LOG_INFO, "Registering SEI Receiver source");
  obs_register_source(&sei_receiver_source_info);

  return true;
}

// 模块卸载
void obs_module_unload(void) {
  blog(LOG_INFO, "[SEI Stamper] Plugin unloaded");
}
