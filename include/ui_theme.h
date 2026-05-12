#ifndef UI_THEME_H
#define UI_THEME_H

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

typedef enum {
    UI_THEME_DARK = 0,
    UI_THEME_LIGHT = 1,
} UiThemeMode;

void ui_theme_set(UiThemeMode mode);
UiThemeMode ui_theme_get(void);
bool ui_theme_is_light(void);
void ui_theme_init_global_styles(void);
void ui_theme_apply_display_theme(void);

lv_color_t ui_theme_color_bg(void);
lv_color_t ui_theme_color_bg_grad(void);
lv_color_t ui_theme_color_text(void);
lv_color_t ui_theme_color_text_muted(void);
lv_color_t ui_theme_color_text_subtle(void);
lv_color_t ui_theme_color_card(void);
lv_color_t ui_theme_color_card_border(void);
lv_color_t ui_theme_color_focus(void);
lv_color_t ui_theme_color_focus_bg(void);
lv_color_t ui_theme_color_main_shadow(void);
lv_color_t ui_theme_color_status_divider(void);
lv_color_t ui_theme_color_success(void);
lv_color_t ui_theme_color_warning(void);
lv_color_t ui_theme_color_error(void);
lv_color_t ui_theme_color_inactive(void);
lv_color_t ui_theme_color_overlay(void);
lv_color_t ui_theme_color_spinner_track(void);
lv_opa_t ui_theme_overlay_opa(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif
