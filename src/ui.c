#include "ui.h"

// GLOBAL STYLES
lv_style_t style_base_panel;
lv_style_t style_icon_btn;
lv_style_t style_icon_btn_pressed;
lv_style_t style_icon_btn_focused;
lv_style_t style_card;
lv_style_t style_glass_list;
lv_style_t style_glass_list_focused;

// SCREENS
lv_obj_t * ui_MainScreen;

void ui_init(void)
{
    // ========== 1. Initialize Global Styles ==========
    ui_theme_init_global_styles();

    // ========== 2. Initialize Screens ==========
    ui_theme_apply_display_theme();

    ui_MainScreen_screen_init();
    ui_AIScreen_screen_init();

    // 3. Load Main Screen
    lv_disp_load_scr(ui_MainScreen);
}
