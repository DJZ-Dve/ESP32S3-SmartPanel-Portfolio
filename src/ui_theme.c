#include "ui_theme.h"
#include "ui.h"

static UiThemeMode g_ui_theme_mode = UI_THEME_DARK;
static bool g_styles_initialized = false;

UiThemeMode ui_theme_get(void) {
    return g_ui_theme_mode;
}

bool ui_theme_is_light(void) {
    return g_ui_theme_mode == UI_THEME_LIGHT;
}

void ui_theme_set(UiThemeMode mode) {
    g_ui_theme_mode = mode == UI_THEME_LIGHT ? UI_THEME_LIGHT : UI_THEME_DARK;
}

lv_color_t ui_theme_color_bg(void) {
    return ui_theme_is_light() ? lv_color_hex(0xFBFDFF) : lv_color_hex(0x0A0E1A);
}

lv_color_t ui_theme_color_bg_grad(void) {
    return ui_theme_is_light() ? lv_color_hex(0xF4FAFF) : lv_color_hex(0x1A1040);
}

lv_color_t ui_theme_color_text(void) {
    return ui_theme_is_light() ? lv_color_hex(0x172331) : lv_color_hex(0xFFFFFF);
}

lv_color_t ui_theme_color_text_muted(void) {
    return ui_theme_is_light() ? lv_color_hex(0x7B8794) : lv_color_hex(0xFFFFFF);
}

lv_color_t ui_theme_color_text_subtle(void) {
    return ui_theme_is_light() ? lv_color_hex(0xA1AEBA) : lv_color_hex(0x94A3B8);
}

lv_color_t ui_theme_color_card(void) {
    return ui_theme_is_light() ? lv_color_hex(0xFFFFFF) : lv_color_hex(0xFFFFFF);
}

lv_color_t ui_theme_color_card_border(void) {
    return ui_theme_is_light() ? lv_color_hex(0xDDE7EF) : lv_color_hex(0xFFFFFF);
}

lv_color_t ui_theme_color_focus(void) {
    return ui_theme_is_light() ? lv_color_hex(0x18B890) : lv_color_hex(0x00D4FF);
}

lv_color_t ui_theme_color_focus_bg(void) {
    return ui_theme_is_light() ? lv_color_hex(0xB7E8D6) : lv_color_hex(0x00D4FF);
}

lv_color_t ui_theme_color_main_shadow(void) {
    return ui_theme_is_light() ? lv_color_hex(0x9FB3C8) : ui_theme_color_focus();
}

lv_color_t ui_theme_color_status_divider(void) {
    return ui_theme_is_light() ? lv_color_hex(0xE2E8F0) : lv_color_hex(0xFFFFFF);
}

lv_color_t ui_theme_color_success(void) {
    return ui_theme_is_light() ? lv_color_hex(0x16A34A) : lv_color_hex(0x4ADE80);
}

lv_color_t ui_theme_color_warning(void) {
    return ui_theme_is_light() ? lv_color_hex(0xF97316) : lv_color_hex(0xFB923C);
}

lv_color_t ui_theme_color_error(void) {
    return ui_theme_is_light() ? lv_color_hex(0xEF4444) : lv_color_hex(0xF87171);
}

lv_color_t ui_theme_color_inactive(void) {
    return ui_theme_is_light() ? lv_color_hex(0x8DA0B6) : lv_color_hex(0x475569);
}

lv_color_t ui_theme_color_overlay(void) {
    return ui_theme_is_light() ? lv_color_hex(0xE2E8F0) : lv_color_hex(0x000000);
}

lv_color_t ui_theme_color_spinner_track(void) {
    return ui_theme_is_light() ? lv_color_hex(0xCBD5E1) : lv_color_hex(0x334155);
}

lv_opa_t ui_theme_overlay_opa(void) {
    return ui_theme_is_light() ? 175 : 185;
}

void ui_theme_init_global_styles(void) {
    if (!g_styles_initialized) {
        lv_style_init(&style_base_panel);
        lv_style_init(&style_icon_btn);
        lv_style_init(&style_icon_btn_pressed);
        lv_style_init(&style_icon_btn_focused);
        lv_style_init(&style_card);
        lv_style_init(&style_glass_list);
        lv_style_init(&style_glass_list_focused);
        g_styles_initialized = true;
    } else {
        lv_style_reset(&style_base_panel);
        lv_style_reset(&style_icon_btn);
        lv_style_reset(&style_icon_btn_pressed);
        lv_style_reset(&style_icon_btn_focused);
        lv_style_reset(&style_card);
        lv_style_reset(&style_glass_list);
        lv_style_reset(&style_glass_list_focused);
    }

    lv_style_set_bg_opa(&style_base_panel, LV_OPA_TRANSP);
    lv_style_set_border_width(&style_base_panel, 0);
    lv_style_set_radius(&style_base_panel, 0);
    lv_style_set_pad_all(&style_base_panel, 0);

    lv_style_set_bg_color(&style_icon_btn, ui_theme_is_light() ? ui_theme_color_card() : lv_color_hex(0xFFFFFF));
    lv_style_set_bg_opa(&style_icon_btn, ui_theme_is_light() ? LV_OPA_COVER : 20);
    lv_style_set_border_color(&style_icon_btn, ui_theme_color_card_border());
    lv_style_set_border_opa(&style_icon_btn, ui_theme_is_light() ? LV_OPA_COVER : 30);
    lv_style_set_border_width(&style_icon_btn, 1);
    lv_style_set_radius(&style_icon_btn, 18);
    lv_style_set_shadow_color(&style_icon_btn, ui_theme_color_main_shadow());
    lv_style_set_shadow_width(&style_icon_btn, ui_theme_is_light() ? 5 : 0);
    lv_style_set_shadow_opa(&style_icon_btn, ui_theme_is_light() ? 28 : 0);
    lv_style_set_shadow_ofs_y(&style_icon_btn, ui_theme_is_light() ? 2 : 0);
    lv_style_set_translate_y(&style_icon_btn, 0);

    static lv_style_transition_dsc_t trans_nav;
    static const lv_style_prop_t trans_props[] = {
        LV_STYLE_BORDER_COLOR,
        LV_STYLE_BORDER_OPA,
        LV_STYLE_BG_COLOR,
        LV_STYLE_BG_OPA,
        LV_STYLE_SHADOW_WIDTH,
        0,
    };
    lv_style_transition_dsc_init(&trans_nav, trans_props, lv_anim_path_ease_out, 250, 0, NULL);
    lv_style_set_transition(&style_icon_btn, &trans_nav);

    lv_style_set_bg_opa(&style_icon_btn_pressed, ui_theme_is_light() ? 235 : 40);
    lv_style_set_transform_width(&style_icon_btn_pressed, -2);
    lv_style_set_transform_height(&style_icon_btn_pressed, -2);

    lv_style_set_border_color(&style_icon_btn_focused, ui_theme_color_focus());
    lv_style_set_border_opa(&style_icon_btn_focused, ui_theme_is_light() ? LV_OPA_COVER : 150);
    lv_style_set_border_width(&style_icon_btn_focused, 2);
    lv_style_set_bg_color(&style_icon_btn_focused, ui_theme_color_focus_bg());
    lv_style_set_bg_opa(&style_icon_btn_focused, ui_theme_is_light() ? LV_OPA_COVER : 25);
    lv_style_set_shadow_color(&style_icon_btn_focused, ui_theme_color_focus());
    lv_style_set_shadow_width(&style_icon_btn_focused, ui_theme_is_light() ? 8 : 15);
    lv_style_set_shadow_opa(&style_icon_btn_focused, ui_theme_is_light() ? 55 : 50);
    lv_style_set_shadow_ofs_y(&style_icon_btn_focused, ui_theme_is_light() ? 3 : 0);

    lv_style_set_bg_color(&style_card, ui_theme_color_card());
    lv_style_set_bg_opa(&style_card, ui_theme_is_light() ? LV_OPA_COVER : 15);
    lv_style_set_border_width(&style_card, 1);
    lv_style_set_border_color(&style_card, ui_theme_color_card_border());
    lv_style_set_border_opa(&style_card, ui_theme_is_light() ? LV_OPA_COVER : 20);
    lv_style_set_radius(&style_card, 12);
    lv_style_set_shadow_width(&style_card, 0);
    lv_style_set_pad_all(&style_card, 4);

    lv_style_set_bg_color(&style_glass_list, ui_theme_color_card());
    lv_style_set_bg_opa(&style_glass_list, ui_theme_is_light() ? LV_OPA_COVER : 15);
    lv_style_set_border_width(&style_glass_list, 1);
    lv_style_set_border_color(&style_glass_list, ui_theme_color_card_border());
    lv_style_set_border_opa(&style_glass_list, ui_theme_is_light() ? LV_OPA_COVER : 20);
    lv_style_set_radius(&style_glass_list, 14);
    lv_style_set_shadow_width(&style_glass_list, 0);
    lv_style_set_pad_hor(&style_glass_list, 12);
    lv_style_set_pad_ver(&style_glass_list, 10);

    static lv_style_transition_dsc_t trans_list;
    static const lv_style_prop_t trans_list_props[] = {
        LV_STYLE_BORDER_COLOR,
        LV_STYLE_BORDER_OPA,
        LV_STYLE_BG_COLOR,
        LV_STYLE_BG_OPA,
        0,
    };
    lv_style_transition_dsc_init(&trans_list, trans_list_props, lv_anim_path_ease_out, 200, 0, NULL);
    lv_style_set_transition(&style_glass_list, &trans_list);

    lv_style_set_border_color(&style_glass_list_focused, ui_theme_color_focus());
    lv_style_set_border_opa(&style_glass_list_focused, ui_theme_is_light() ? LV_OPA_COVER : 130);
    lv_style_set_bg_color(&style_glass_list_focused, ui_theme_color_focus_bg());
    lv_style_set_bg_opa(&style_glass_list_focused, ui_theme_is_light() ? LV_OPA_COVER : 20);
}

void ui_theme_apply_display_theme(void) {
    lv_disp_t* dispp = lv_disp_get_default();
    if (!dispp) {
        return;
    }

    lv_theme_t* theme = lv_theme_default_init(dispp,
                                              ui_theme_color_focus(),
                                              ui_theme_is_light() ? lv_color_hex(0x3B82F6) : lv_color_hex(0x7B68EE),
                                              !ui_theme_is_light(),
                                              LV_FONT_DEFAULT);
    lv_disp_set_theme(dispp, theme);
}
