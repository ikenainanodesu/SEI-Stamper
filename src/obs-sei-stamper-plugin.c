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

// 前向声明 - 编码器
extern struct obs_encoder_info sei_stamper_h264_encoder_info;
extern struct obs_encoder_info sei_stamper_h265_encoder_info;
extern struct obs_encoder_info sei_stamper_av1_encoder_info;

// 前向声明 - 源
extern struct obs_source_info sei_receiver_source_info;

// 模块加载
bool obs_module_load(void) {
  blog(LOG_INFO, "[SEI Stamper] Plugin loaded (version 1.0.0)");
  blog(LOG_INFO,
       "[SEI Stamper] Developed for frame-level video synchronization");

  // 注册编码器
  obs_register_encoder(&sei_stamper_h264_encoder_info);
  blog(LOG_INFO, "[SEI Stamper] Registered H.264 encoder");

#ifdef ENABLE_VPL
  extern struct obs_encoder_info qsv_encoder_info;
  obs_register_encoder(&qsv_encoder_info);
  blog(LOG_INFO,
       "[SEI Stamper] Registered Intel QuickSync (VPL Native) encoder");
#endif

  obs_register_encoder(&sei_stamper_h265_encoder_info);
  blog(LOG_INFO, "[SEI Stamper] Registered H.265/HEVC encoder");

  obs_register_encoder(&sei_stamper_av1_encoder_info);
  blog(LOG_INFO, "[SEI Stamper] Registered AV1 encoder");

  // 注册源
  obs_register_source(&sei_receiver_source_info);
  blog(LOG_INFO, "[SEI Stamper] Registered SEI Receiver source");

  return true;
}

// 模块卸载
void obs_module_unload(void) {
  blog(LOG_INFO, "[SEI Stamper] Plugin unloaded");
}
