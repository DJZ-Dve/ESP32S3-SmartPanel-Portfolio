#ifndef _ESP32_UI_H
#define _ESP32_UI_H

#ifdef __cplusplus
extern "C" {
#endif

#if defined __has_include
  #if __has_include("lvgl.h")
    #include "lvgl.h"
  #elif __has_include("lvgl/lvgl.h")
    #include "lvgl/lvgl.h"
  #else
    #include "lvgl.h"
  #endif
#else
  #include "lvgl.h"
#endif

#include "ui_theme.h"

// SCREENS
#include "ui_MainScreen.h"
#include "ui_AIScreen.h"

// ICON RESOURCES (新图标 - 40x40 导航图标)
LV_IMG_DECLARE(ui_img_icon_ai);
LV_IMG_DECLARE(ui_img_icon_bind);
LV_IMG_DECLARE(ui_img_icon_settings);
// 20x20 设置列表图标
LV_IMG_DECLARE(ui_img_icon_lang);
LV_IMG_DECLARE(ui_img_icon_volume);
LV_IMG_DECLARE(ui_img_icon_info);
LV_IMG_DECLARE(ui_img_icon_network);
// 设备状态图标 (on/off)
LV_IMG_DECLARE(ui_img_shuinuan_on);
LV_IMG_DECLARE(ui_img_shuinuan_off);
LV_IMG_DECLARE(ui_img_kongtiao_on);
LV_IMG_DECLARE(ui_img_kongtiao_off);
LV_IMG_DECLARE(ui_img_fengnuan_on);
LV_IMG_DECLARE(ui_img_fengnuan_off);

// FONTS
LV_FONT_DECLARE(my_font_misans_20);

// GLOBAL STYLES
extern lv_style_t style_base_panel;
extern lv_style_t style_icon_btn;
extern lv_style_t style_icon_btn_pressed;
extern lv_style_t style_icon_btn_focused;
extern lv_style_t style_card;
extern lv_style_t style_glass_list;
extern lv_style_t style_glass_list_focused;

void ui_init(void);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif
