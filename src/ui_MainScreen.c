#include "ui.h"

extern lv_obj_t * ui_MainScreen;  // Defined in ui.c, use extern here
lv_obj_t * ui_PanelTopTitle;
// 4G UI ("4G" 文本，柱状信号条已移除)
lv_obj_t * ui_PanelCard4G;
lv_obj_t * ui_LabelTxt4G;
lv_obj_t * ui_LabelBleStatus;
// WiFi UI (同心弧扇形 + 圆点)
lv_obj_t * ui_WifiArc1;  // 内层弧 (弱信号)
lv_obj_t * ui_WifiArc2;  // 外层弧 (强信号)
lv_obj_t * ui_WifiDot;   // 底部圆点
// Battery UI
lv_obj_t * ui_LabelBattery;
lv_obj_t * ui_BatteryBody;
lv_obj_t * ui_BatteryFill;
lv_obj_t * ui_BatteryCap;
// 底部导航
lv_obj_t * ui_PanelMainShow;
lv_obj_t * ui_ButtonAI;
lv_obj_t * ui_ImageAI;
lv_obj_t * ui_ButtonBluetooth;
lv_obj_t * ui_ImageBluetooth;
lv_obj_t * ui_ButtonScene;
lv_obj_t * ui_ImageScene;
lv_obj_t * ui_ButtonSetting;
lv_obj_t * ui_ImageSetting;
lv_obj_t * ui_BadgeSetting;

// 大时钟
lv_obj_t * ui_ClockLabel;
lv_obj_t * ui_DateLabel;
// 状态药丸
lv_obj_t * ui_PillOnline;
lv_obj_t * ui_PillTemp;
// 内部状态条 (仅本文件可见)
static lv_obj_t * ui_StatusPill;
static lv_obj_t * ui_StatusDivider;
static lv_obj_t * ui_StatusFlipLine;

typedef enum {
    MAIN_NAV_ICON_AI,
    MAIN_NAV_ICON_DEVICE,
    MAIN_NAV_ICON_SCENE,
    MAIN_NAV_ICON_SETTINGS,
} MainNavIconType;

static int nav_normal_width(void)
{
    return 58;
}

static int nav_normal_height(void)
{
    return 58;
}

static int nav_focused_width(void)
{
    return 118;
}

static int nav_focused_height(void)
{
    return 72;
}

static lv_color_t nav_idle_color(void)
{
    return ui_theme_is_light() ? ui_theme_color_text() : lv_color_hex(0xFFFFFF);
}

static void set_icon_part_color(lv_obj_t * obj, lv_color_t color)
{
    lv_obj_set_style_line_color(obj, color, 0);
    lv_obj_set_style_arc_color(obj, color, LV_PART_MAIN);
    lv_obj_set_style_border_color(obj, color, 0);
    lv_obj_set_style_bg_color(obj, color, 0);
    lv_obj_set_style_text_color(obj, color, 0);
}

static void recolor_icon_tree(lv_obj_t * root, lv_color_t color)
{
    if (!root) {
        return;
    }

    uint32_t child_count = lv_obj_get_child_cnt(root);
    for (uint32_t i = 0; i < child_count; ++i) {
        lv_obj_t * child = lv_obj_get_child(root, i);
        set_icon_part_color(child, color);
        recolor_icon_tree(child, color);
    }
}

static lv_obj_t * create_line(lv_obj_t * parent, const lv_point_t * points, uint16_t point_count)
{
    lv_obj_t * line = lv_line_create(parent);
    lv_line_set_points(line, points, point_count);
    lv_obj_set_style_line_width(line, 2, 0);
    lv_obj_set_style_line_rounded(line, true, 0);
    lv_obj_set_style_line_color(line, nav_idle_color(), 0);
    lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE);
    return line;
}

static lv_obj_t * create_outline_rect(lv_obj_t * parent, int w, int h, int x, int y, int radius)
{
    lv_obj_t * obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, w, h);
    lv_obj_align(obj, LV_ALIGN_TOP_LEFT, x, y);
    lv_obj_set_style_bg_opa(obj, 0, 0);
    lv_obj_set_style_border_color(obj, nav_idle_color(), 0);
    lv_obj_set_style_border_width(obj, 2, 0);
    lv_obj_set_style_border_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return obj;
}

static lv_obj_t * create_solid_dot(lv_obj_t * parent, int size, int x, int y)
{
    lv_obj_t * dot = lv_obj_create(parent);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, size, size);
    lv_obj_align(dot, LV_ALIGN_TOP_LEFT, x, y);
    lv_obj_set_style_bg_color(dot, nav_idle_color(), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return dot;
}

static lv_obj_t * create_robot_icon(lv_obj_t * parent)
{
    static const lv_point_t antenna[] = {{16, 3}, {16, 8}};
    static const lv_point_t base[] = {{11, 27}, {21, 27}};

    lv_obj_t * icon = lv_obj_create(parent);
    lv_obj_remove_style_all(icon);
    lv_obj_set_size(icon, 32, 32);
    lv_obj_clear_flag(icon, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    create_line(icon, antenna, 2);
    create_solid_dot(icon, 4, 14, 0);
    create_outline_rect(icon, 22, 16, 5, 8, 7);
    create_solid_dot(icon, 4, 11, 14);
    create_solid_dot(icon, 4, 19, 14);
    create_outline_rect(icon, 16, 5, 8, 25, 2);
    create_line(icon, base, 2);
    return icon;
}

static lv_obj_t * create_phone_bt_icon(lv_obj_t * parent)
{
    lv_obj_t * icon = lv_obj_create(parent);
    lv_obj_remove_style_all(icon);
    lv_obj_set_size(icon, 32, 32);
    lv_obj_clear_flag(icon, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    create_outline_rect(icon, 13, 24, 4, 4, 3);
    create_solid_dot(icon, 2, 9, 24);

    lv_obj_t * bt = lv_label_create(icon);
    lv_label_set_text(bt, LV_SYMBOL_BLUETOOTH);
    lv_obj_set_style_text_font(bt, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(bt, nav_idle_color(), 0);
    lv_obj_set_style_text_opa(bt, LV_OPA_COVER, 0);
    lv_obj_align(bt, LV_ALIGN_RIGHT_MID, -2, 0);
    lv_obj_clear_flag(bt, LV_OBJ_FLAG_CLICKABLE);
    return icon;
}

static lv_obj_t * create_settings_icon(lv_obj_t * parent)
{
    static const lv_point_t top_line[] = {{4, 9}, {28, 9}};
    static const lv_point_t mid_line[] = {{4, 16}, {28, 16}};
    static const lv_point_t bottom_line[] = {{4, 23}, {28, 23}};

    lv_obj_t * icon = lv_obj_create(parent);
    lv_obj_remove_style_all(icon);
    lv_obj_set_size(icon, 32, 32);
    lv_obj_clear_flag(icon, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    create_line(icon, top_line, 2);
    create_line(icon, mid_line, 2);
    create_line(icon, bottom_line, 2);
    create_solid_dot(icon, 5, 10, 7);
    create_solid_dot(icon, 5, 20, 14);
    create_solid_dot(icon, 5, 14, 21);
    return icon;
}

// 场景图标：2x2 圆角小方块网格，象形“快捷面板”。
static lv_obj_t * create_scene_icon(lv_obj_t * parent)
{
    lv_obj_t * icon = lv_obj_create(parent);
    lv_obj_remove_style_all(icon);
    lv_obj_set_size(icon, 32, 32);
    lv_obj_clear_flag(icon, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    create_outline_rect(icon, 12, 12, 4, 4, 3);
    create_outline_rect(icon, 12, 12, 16, 4, 3);
    create_outline_rect(icon, 12, 12, 4, 16, 3);
    create_outline_rect(icon, 12, 12, 16, 16, 3);
    return icon;
}

static lv_obj_t * create_main_nav_icon(lv_obj_t * parent, MainNavIconType type)
{
    switch (type) {
        case MAIN_NAV_ICON_AI:
            return create_robot_icon(parent);
        case MAIN_NAV_ICON_DEVICE:
            return create_phone_bt_icon(parent);
        case MAIN_NAV_ICON_SCENE:
            return create_scene_icon(parent);
        case MAIN_NAV_ICON_SETTINGS:
        default:
            return create_settings_icon(parent);
    }
}

// 单 anim 驱动 width，height 按 normal/focused 端点线性映射，省掉一份 anim 调度。
static void nav_anim_size_cb(void * obj, int32_t value)
{
    lv_obj_t * btn = (lv_obj_t *)obj;
    lv_obj_set_width(btn, value);

    const int wMin = nav_normal_width();
    const int wMax = nav_focused_width();
    if (wMax == wMin) {
        return;
    }
    const int hMin = nav_normal_height();
    const int hMax = nav_focused_height();
    const int32_t h = hMin + (value - wMin) * (hMax - hMin) / (wMax - wMin);
    lv_obj_set_height(btn, h);
}

// 焦点动画结束后恢复 shadow_opa：移除 local override 后，按钮回到 state-based 阴影。
static void nav_anim_restore_shadow_cb(lv_anim_t * a)
{
    lv_obj_t * btn = (lv_obj_t *)a->var;
    if (btn) {
        lv_obj_remove_local_style_prop(btn, LV_STYLE_SHADOW_OPA, LV_PART_MAIN);
    }
}

static void animate_nav_size(lv_obj_t * btn, int target_w, int target_h)
{
    (void)target_h;  // 由 nav_anim_size_cb 内部按 width 线性推算
    lv_anim_del(btn, nav_anim_size_cb);

    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, btn);
    lv_anim_set_time(&anim, 100);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&anim, nav_anim_size_cb);
    lv_anim_set_values(&anim, lv_obj_get_width(btn), target_w);
    lv_anim_set_ready_cb(&anim, nav_anim_restore_shadow_cb);
    lv_anim_start(&anim);
}

static void apply_nav_focus_visual(lv_obj_t * btn, bool focused, bool animate)
{
    if (!btn) {
        return;
    }

    int target_w = focused ? nav_focused_width() : nav_normal_width();
    int target_h = focused ? nav_focused_height() : nav_normal_height();
    if (animate) {
        // 动画期间临时压住 shadow_opa：LVGL draw 路径会在 opa<=MIN 时直接 skip shadow，
        // 即便 trans_nav 还在 tween shadow_width 也不会再触发 box blur 重算。
        // 动画 ready_cb 会移除该 local override，让 state-based shadow_opa（50/55）自然恢复，
        // 此时 trans_nav 的 shadow_width 残留 ~40ms 正好作为 shadow fade-in。
        lv_obj_set_style_shadow_opa(btn, 0, LV_PART_MAIN);
        animate_nav_size(btn, target_w, target_h);
    } else {
        // 静态切换（首次进入 / 主题重建）：清掉可能残留的 local override
        lv_obj_remove_local_style_prop(btn, LV_STYLE_SHADOW_OPA, LV_PART_MAIN);
        lv_obj_set_size(btn, target_w, target_h);
    }

    const lv_color_t fg = focused ? ui_theme_color_focus() : nav_idle_color();
    lv_obj_t * icon = lv_obj_get_child(btn, 0);
    lv_obj_t * label = lv_obj_get_child(btn, 1);
    recolor_icon_tree(icon, fg);
    if (label) {
        lv_obj_set_style_text_color(label, fg, 0);
        lv_obj_set_style_text_opa(label, ui_theme_is_light() ? LV_OPA_COVER : (focused ? LV_OPA_COVER : 180), 0);
    }

    lv_obj_set_style_radius(btn, focused ? 24 : 18, 0);
    if (!focused) {
        lv_obj_set_style_bg_color(btn, ui_theme_is_light() ? ui_theme_color_card() : lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_bg_opa(btn, ui_theme_is_light() ? LV_OPA_COVER : 20, 0);
        lv_obj_set_style_border_color(btn, ui_theme_color_card_border(), 0);
        lv_obj_set_style_border_opa(btn, ui_theme_is_light() ? LV_OPA_COVER : 30, 0);
    }
}

static void nav_button_event_cb(lv_event_t * e)
{
    lv_obj_t * btn = lv_event_get_target(e);
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_FOCUSED) {
        apply_nav_focus_visual(btn, true, true);
    } else if (code == LV_EVENT_DEFOCUSED) {
        apply_nav_focus_visual(btn, false, true);
    }
}

static lv_obj_t * create_nav_button(lv_obj_t * parent, MainNavIconType icon_type, const char * label_text)
{
    lv_obj_t * btn = lv_btn_create(parent);
    lv_obj_set_size(btn, nav_normal_width(), nav_normal_height());
    lv_obj_set_style_pad_all(btn, 0, 0);

    lv_obj_add_style(btn, &style_icon_btn, 0);
    lv_obj_add_style(btn, &style_icon_btn_pressed, LV_STATE_PRESSED);
    lv_obj_add_style(btn, &style_icon_btn_focused, LV_STATE_FOCUSED);
    lv_obj_add_event_cb(btn, nav_button_event_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(btn, nav_button_event_cb, LV_EVENT_DEFOCUSED, NULL);

    lv_obj_set_layout(btn, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(btn, 0, 0);
    lv_obj_set_style_pad_top(btn, 5, 0);
    lv_obj_set_style_pad_bottom(btn, 8, 0);

    lv_obj_t * icon = create_main_nav_icon(btn, icon_type);

    lv_obj_t * lbl = lv_label_create(btn);
    lv_label_set_text(lbl, label_text);
    lv_obj_set_style_text_color(lbl, nav_idle_color(), 0);
    lv_obj_set_style_text_opa(lbl, ui_theme_is_light() ? 230 : 180, 0);
    lv_obj_set_style_text_font(lbl, &my_font_misans_20, 0);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);

    apply_nav_focus_visual(btn, false, false);
    (void)icon;
    return btn;
}

static lv_obj_t * create_status_segment(lv_obj_t * parent, lv_color_t dot_color, const char * text, int label_width)
{
    lv_obj_t * segment = lv_obj_create(parent);
    lv_obj_remove_style_all(segment);
    lv_obj_set_size(segment, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_clear_flag(segment, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_layout(segment, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(segment, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(segment, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(segment, 5, 0);

    lv_obj_t * dot = lv_obj_create(segment);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 7, 7);
    lv_obj_set_style_bg_color(dot, dot_color, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_shadow_color(dot, dot_color, 0);
    lv_obj_set_style_shadow_width(dot, ui_theme_is_light() ? 3 : 4, 0);
    lv_obj_set_style_shadow_opa(dot, ui_theme_is_light() ? 120 : LV_OPA_COVER, 0);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * lbl = lv_label_create(segment);
    lv_label_set_text(lbl, text);
    lv_obj_set_width(lbl, label_width);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(lbl, ui_theme_color_text(), 0);
    lv_obj_set_style_text_opa(lbl, ui_theme_is_light() ? LV_OPA_COVER : 170, 0);
    lv_obj_set_style_text_font(lbl, &my_font_misans_20, 0);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    return segment;
}

static lv_obj_t * create_combined_status_pill(lv_obj_t * parent)
{
    lv_obj_t * pill = lv_obj_create(parent);
    ui_StatusPill = pill;
    lv_obj_remove_style_all(pill);
    lv_obj_set_size(pill, 154, 34);
    lv_obj_add_style(pill, &style_card, 0);
    lv_obj_set_style_radius(pill, 17, 0);
    lv_obj_set_style_pad_hor(pill, 10, 0);
    lv_obj_set_style_pad_ver(pill, 0, 0);
    lv_obj_set_style_shadow_color(pill, ui_theme_color_main_shadow(), 0);
    lv_obj_set_style_shadow_width(pill, ui_theme_is_light() ? 7 : 0, 0);
    lv_obj_set_style_shadow_opa(pill, ui_theme_is_light() ? 35 : 0, 0);
    lv_obj_set_style_shadow_ofs_y(pill, ui_theme_is_light() ? 2 : 0, 0);
    lv_obj_clear_flag(pill, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_set_layout(pill, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(pill, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(pill, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(pill, 8, 0);

    ui_PillOnline = create_status_segment(pill, ui_theme_color_success(), "在线", 42);

    ui_StatusDivider = lv_obj_create(pill);
    lv_obj_remove_style_all(ui_StatusDivider);
    lv_obj_set_size(ui_StatusDivider, 1, 18);
    lv_obj_set_style_bg_color(ui_StatusDivider, ui_theme_color_status_divider(), 0);
    lv_obj_set_style_bg_opa(ui_StatusDivider, ui_theme_is_light() ? LV_OPA_COVER : 45, 0);
    lv_obj_clear_flag(ui_StatusDivider, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    ui_PillTemp = create_status_segment(pill, ui_theme_color_warning(), "--°C", ui_theme_is_light() ? 48 : 48);

    ui_StatusFlipLine = lv_obj_create(pill);
    lv_obj_remove_style_all(ui_StatusFlipLine);
    lv_obj_set_size(ui_StatusFlipLine, 154, 1);
    lv_obj_add_flag(ui_StatusFlipLine, LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_clear_flag(ui_StatusFlipLine, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(ui_StatusFlipLine, ui_theme_color_status_divider(), 0);
    lv_obj_set_style_bg_opa(ui_StatusFlipLine, ui_theme_is_light() ? 150 : 70, 0);
    lv_obj_align(ui_StatusFlipLine, LV_ALIGN_CENTER, 0, 0);
    return pill;
}

void ui_MainScreen_screen_init(void)
{
    ui_MainScreen = lv_obj_create(NULL);
    lv_obj_clear_flag(ui_MainScreen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(ui_MainScreen, 240, 240);
    
    // ===== 主题背景 =====
    lv_obj_set_style_bg_color(ui_MainScreen, ui_theme_color_bg(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui_MainScreen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(ui_MainScreen, ui_theme_color_bg_grad(), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(ui_MainScreen, LV_GRAD_DIR_VER, LV_PART_MAIN);
    lv_obj_set_style_border_width(ui_MainScreen, 0, 0);
    
    // Flex 布局: 垂直堆叠
    lv_obj_set_layout(ui_MainScreen, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(ui_MainScreen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(ui_MainScreen, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(ui_MainScreen, 0, 0);
    lv_obj_set_style_pad_row(ui_MainScreen, 0, 0);

    // ================= 顶部状态栏 (高32px) =================
    ui_PanelTopTitle = lv_obj_create(ui_MainScreen);
    lv_obj_remove_style_all(ui_PanelTopTitle);
    lv_obj_set_width(ui_PanelTopTitle, 240);
    lv_obj_set_height(ui_PanelTopTitle, ui_theme_is_light() ? 32 : 28);
    lv_obj_set_style_bg_opa(ui_PanelTopTitle, 0, 0);
    lv_obj_clear_flag(ui_PanelTopTitle, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_hor(ui_PanelTopTitle, ui_theme_is_light() ? 14 : 12, 0);
    lv_obj_set_style_pad_top(ui_PanelTopTitle, ui_theme_is_light() ? 8 : 6, 0);
    lv_obj_set_style_pad_column(ui_PanelTopTitle, 6, 0);

    // Flex Row 布局, 左对齐 (spacer 把电池推到右侧)
    lv_obj_set_layout(ui_PanelTopTitle, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(ui_PanelTopTitle, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ui_PanelTopTitle, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // --- 左侧组: WiFi + 4G ---
    lv_obj_t * left_status = lv_obj_create(ui_PanelTopTitle);
    lv_obj_remove_style_all(left_status);
    lv_obj_set_size(left_status, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_clear_flag(left_status, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(left_status, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(left_status, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(left_status, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(left_status, ui_theme_is_light() ? 9 : 8, 0);

    // WiFi 信号指示 (小屏强化可读性: 2层弧 + 底部圆点)
    lv_obj_t * wifi_icon = lv_obj_create(left_status);
    lv_obj_remove_style_all(wifi_icon);
    lv_obj_set_size(wifi_icon, ui_theme_is_light() ? 26 : 22, ui_theme_is_light() ? 22 : 20);
    lv_obj_clear_flag(wifi_icon, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_color_t bar_inactive = ui_theme_color_inactive();

    // 外层弧 (强信号)
    ui_WifiArc2 = lv_arc_create(wifi_icon);
    lv_obj_set_size(ui_WifiArc2, ui_theme_is_light() ? 22 : 18, ui_theme_is_light() ? 22 : 18);
    lv_obj_align(ui_WifiArc2, LV_ALIGN_CENTER, 0, ui_theme_is_light() ? 1 : 1);
    lv_arc_set_bg_angles(ui_WifiArc2, 210, 330);
    lv_arc_set_angles(ui_WifiArc2, 0, 0);
    lv_obj_set_style_bg_opa(ui_WifiArc2, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(ui_WifiArc2, 0, LV_PART_MAIN);
    lv_obj_set_style_arc_color(ui_WifiArc2, bar_inactive, LV_PART_MAIN);
    lv_obj_set_style_arc_width(ui_WifiArc2, ui_theme_is_light() ? 4 : 4, LV_PART_MAIN);
    lv_obj_set_style_opa(ui_WifiArc2, 0, LV_PART_INDICATOR);
    lv_obj_set_style_opa(ui_WifiArc2, 0, LV_PART_KNOB);
    lv_obj_clear_flag(ui_WifiArc2, LV_OBJ_FLAG_CLICKABLE);

    // 内层弧 (中等信号)
    ui_WifiArc1 = lv_arc_create(wifi_icon);
    lv_obj_set_size(ui_WifiArc1, ui_theme_is_light() ? 13 : 10, ui_theme_is_light() ? 13 : 10);
    lv_obj_align(ui_WifiArc1, LV_ALIGN_CENTER, 0, ui_theme_is_light() ? 2 : 2);
    lv_arc_set_bg_angles(ui_WifiArc1, 210, 330);
    lv_arc_set_angles(ui_WifiArc1, 0, 0);
    lv_obj_set_style_bg_opa(ui_WifiArc1, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(ui_WifiArc1, 0, LV_PART_MAIN);
    lv_obj_set_style_arc_color(ui_WifiArc1, bar_inactive, LV_PART_MAIN);
    lv_obj_set_style_arc_width(ui_WifiArc1, ui_theme_is_light() ? 4 : 3, LV_PART_MAIN);
    lv_obj_set_style_opa(ui_WifiArc1, 0, LV_PART_INDICATOR);
    lv_obj_set_style_opa(ui_WifiArc1, 0, LV_PART_KNOB);
    lv_obj_clear_flag(ui_WifiArc1, LV_OBJ_FLAG_CLICKABLE);

    // 底部圆点 (弱信号)
    ui_WifiDot = lv_obj_create(wifi_icon);
    lv_obj_remove_style_all(ui_WifiDot);
    lv_obj_set_size(ui_WifiDot, ui_theme_is_light() ? 6 : 5, ui_theme_is_light() ? 6 : 5);
    lv_obj_align(ui_WifiDot, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(ui_WifiDot, bar_inactive, 0);
    lv_obj_set_style_bg_opa(ui_WifiDot, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(ui_WifiDot, LV_RADIUS_CIRCLE, 0);
    lv_obj_clear_flag(ui_WifiDot, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    // 4G 文本容器 (仅承载 "4G" 字样)
    ui_PanelCard4G = lv_obj_create(left_status);
    lv_obj_remove_style_all(ui_PanelCard4G);
    lv_obj_set_size(ui_PanelCard4G, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_clear_flag(ui_PanelCard4G, LV_OBJ_FLAG_SCROLLABLE);

    ui_LabelTxt4G = lv_label_create(ui_PanelCard4G);
    lv_label_set_text(ui_LabelTxt4G, "4G");
    lv_obj_set_style_text_font(ui_LabelTxt4G, ui_theme_is_light() ? &lv_font_montserrat_14 : &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(ui_LabelTxt4G, ui_theme_color_text_muted(), 0);
    lv_obj_set_style_text_opa(ui_LabelTxt4G, ui_theme_is_light() ? 230 : 180, 0);

    ui_LabelBleStatus = lv_label_create(left_status);
    lv_label_set_text(ui_LabelBleStatus, LV_SYMBOL_BLUETOOTH);
    lv_obj_set_style_text_font(ui_LabelBleStatus,
                               ui_theme_is_light() ? &lv_font_montserrat_14 : &lv_font_montserrat_12,
                               0);
    lv_obj_set_style_text_color(ui_LabelBleStatus, bar_inactive, 0);
    lv_obj_set_style_text_opa(ui_LabelBleStatus, ui_theme_is_light() ? 190 : 150, 0);
    lv_obj_clear_flag(ui_LabelBleStatus, LV_OBJ_FLAG_CLICKABLE);

    // --- 弹性占位 (把电池推到右侧) ---
    lv_obj_t * spacer = lv_obj_create(ui_PanelTopTitle);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_flex_grow(spacer, 1);
    lv_obj_set_height(spacer, 1);
    lv_obj_clear_flag(spacer, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    // --- 右侧组: 电池 ---
    lv_obj_t * right_status = lv_obj_create(ui_PanelTopTitle);
    lv_obj_remove_style_all(right_status);
    lv_obj_set_size(right_status, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_clear_flag(right_status, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(right_status, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(right_status, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right_status, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(right_status, ui_theme_is_light() ? 5 : 4, 0);

    // 充电闪电图标 (默认隐藏，充电时显示在电池左侧)
    ui_LabelBattery = lv_label_create(right_status);
    lv_label_set_text(ui_LabelBattery, LV_SYMBOL_CHARGE);
    lv_obj_set_style_text_font(ui_LabelBattery, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(ui_LabelBattery, ui_theme_is_light() ? lv_color_hex(0xF59E0B) : lv_color_hex(0xFACC15), 0);
    lv_obj_set_style_text_opa(ui_LabelBattery, LV_OPA_COVER, 0);
    lv_obj_add_flag(ui_LabelBattery, LV_OBJ_FLAG_HIDDEN);

    // 电池图标容器 (4 格电量显示)
    lv_obj_t * bat_container = lv_obj_create(right_status);
    lv_obj_remove_style_all(bat_container);
    lv_obj_set_size(bat_container, ui_theme_is_light() ? 30 : 28, ui_theme_is_light() ? 13 : 12);
    lv_obj_clear_flag(bat_container, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    // 电池主体 (外框)
    ui_BatteryBody = lv_obj_create(bat_container);
    lv_obj_remove_style_all(ui_BatteryBody);
    lv_obj_set_size(ui_BatteryBody, ui_theme_is_light() ? 26 : 24, ui_theme_is_light() ? 11 : 10);
    lv_obj_align(ui_BatteryBody, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_border_color(ui_BatteryBody, ui_theme_color_text_subtle(), 0);
    lv_obj_set_style_border_opa(ui_BatteryBody, ui_theme_is_light() ? 210 : 150, 0);
    lv_obj_set_style_border_width(ui_BatteryBody, 1, 0);
    lv_obj_set_style_radius(ui_BatteryBody, 2, 0);
    lv_obj_set_style_bg_opa(ui_BatteryBody, 0, 0);
    lv_obj_clear_flag(ui_BatteryBody, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    // 电池内部分段容器
    ui_BatteryFill = lv_obj_create(ui_BatteryBody);
    lv_obj_remove_style_all(ui_BatteryFill);
    lv_obj_set_size(ui_BatteryFill, ui_theme_is_light() ? 20 : 18, 6);
    lv_obj_align(ui_BatteryFill, LV_ALIGN_LEFT_MID, 2, 0);
    lv_obj_clear_flag(ui_BatteryFill, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_layout(ui_BatteryFill, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(ui_BatteryFill, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ui_BatteryFill, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(ui_BatteryFill, 0, 0);
    lv_obj_set_style_pad_column(ui_BatteryFill, 1, 0);

    for (int i = 0; i < 4; ++i) {
        lv_obj_t * segment = lv_obj_create(ui_BatteryFill);
        lv_obj_remove_style_all(segment);
        lv_obj_set_size(segment, ui_theme_is_light() ? 4 : 4, 6);
        lv_obj_set_style_bg_color(segment, lv_color_hex(0x4ADE80), 0);
        lv_obj_set_style_bg_opa(segment, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(segment, 1, 0);
        lv_obj_clear_flag(segment, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    }

    // 电池正极帽
    ui_BatteryCap = lv_obj_create(bat_container);
    lv_obj_remove_style_all(ui_BatteryCap);
    lv_obj_set_size(ui_BatteryCap, 2, ui_theme_is_light() ? 5 : 4);
    lv_obj_align(ui_BatteryCap, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(ui_BatteryCap, ui_theme_color_text_subtle(), 0);
    lv_obj_set_style_bg_opa(ui_BatteryCap, ui_theme_is_light() ? 210 : 150, 0);
    lv_obj_set_style_radius(ui_BatteryCap, 1, 0);
    lv_obj_clear_flag(ui_BatteryCap, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    // ================= 中央大时钟区域 =================
    lv_obj_t * clock_area = lv_obj_create(ui_MainScreen);
    lv_obj_remove_style_all(clock_area);
    lv_obj_set_width(clock_area, 240);
    lv_obj_set_flex_grow(clock_area, 1);
    lv_obj_clear_flag(clock_area, LV_OBJ_FLAG_SCROLLABLE);
    
    lv_obj_set_layout(clock_area, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(clock_area, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(clock_area, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(clock_area, ui_theme_is_light() ? 4 : 2, 0);

    // 大时钟数字 (开机未联网时显示占位; SNTP 同步成功后由 statusTimerCallback 更新)
    ui_ClockLabel = lv_label_create(clock_area);
    lv_label_set_text(ui_ClockLabel, "--:--");
    lv_obj_set_style_text_font(ui_ClockLabel, &lv_font_montserrat_42, 0);
    lv_obj_set_style_text_color(ui_ClockLabel, ui_theme_color_text(), 0);
    lv_obj_set_style_text_opa(ui_ClockLabel, ui_theme_is_light() ? LV_OPA_COVER : 240, 0);
    lv_obj_set_style_text_letter_space(ui_ClockLabel, ui_theme_is_light() ? 1 : 2, 0);

    // 日期副标题 (使用中文字体)
    ui_DateLabel = lv_label_create(clock_area);
    lv_label_set_text(ui_DateLabel, "--/--");
    lv_obj_set_style_text_font(ui_DateLabel, &my_font_misans_20, 0);
    lv_obj_set_style_text_color(ui_DateLabel, ui_theme_color_text_muted(), 0);
    lv_obj_set_style_text_opa(ui_DateLabel, ui_theme_is_light() ? 220 : 130, 0);
    lv_obj_set_style_text_letter_space(ui_DateLabel, 0, 0);

    create_combined_status_pill(clock_area);

    // ================= 底部导航Dock =================
    ui_PanelMainShow = lv_obj_create(ui_MainScreen);
    lv_obj_remove_style_all(ui_PanelMainShow);
    lv_obj_set_width(ui_PanelMainShow, 240);
    lv_obj_set_height(ui_PanelMainShow, 82);
    lv_obj_clear_flag(ui_PanelMainShow, LV_OBJ_FLAG_SCROLLABLE);
    
    lv_obj_set_layout(ui_PanelMainShow, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(ui_PanelMainShow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ui_PanelMainShow, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(ui_PanelMainShow, 12, 0);
    lv_obj_set_style_pad_bottom(ui_PanelMainShow, 8, 0);
    // 滚盘模式: 允许水平滚动 + snap 居中
    lv_obj_add_flag(ui_PanelMainShow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(ui_PanelMainShow, LV_DIR_HOR);
    lv_obj_set_scroll_snap_x(ui_PanelMainShow, LV_SCROLL_SNAP_CENTER);
    lv_obj_set_scrollbar_mode(ui_PanelMainShow, LV_SCROLLBAR_MODE_OFF);

    // 创建导航按钮 (中文标签)
    ui_ButtonAI = create_nav_button(ui_PanelMainShow, MAIN_NAV_ICON_AI, "语音");
    ui_ImageAI = lv_obj_get_child(ui_ButtonAI, 0);

    ui_ButtonBluetooth = create_nav_button(ui_PanelMainShow, MAIN_NAV_ICON_DEVICE, "蓝牙");
    ui_ImageBluetooth = lv_obj_get_child(ui_ButtonBluetooth, 0);

    ui_ButtonScene = create_nav_button(ui_PanelMainShow, MAIN_NAV_ICON_SCENE, "场景");
    ui_ImageScene = lv_obj_get_child(ui_ButtonScene, 0);

    ui_ButtonSetting = create_nav_button(ui_PanelMainShow, MAIN_NAV_ICON_SETTINGS, "设置");
    ui_ImageSetting = lv_obj_get_child(ui_ButtonSetting, 0);
    ui_BadgeSetting = lv_obj_create(ui_ButtonSetting);
    lv_obj_remove_style_all(ui_BadgeSetting);
    lv_obj_set_size(ui_BadgeSetting, 10, 10);
    lv_obj_add_flag(ui_BadgeSetting, LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_clear_flag(ui_BadgeSetting, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(ui_BadgeSetting, lv_color_hex(0xFF4D4F), 0);
    lv_obj_set_style_bg_opa(ui_BadgeSetting, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(ui_BadgeSetting, lv_color_hex(0xFFF5F5), 0);
    lv_obj_set_style_border_width(ui_BadgeSetting, 1, 0);
    lv_obj_set_style_radius(ui_BadgeSetting, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_shadow_color(ui_BadgeSetting, lv_color_hex(0xFF4D4F), 0);
    lv_obj_set_style_shadow_width(ui_BadgeSetting, 8, 0);
    lv_obj_set_style_shadow_opa(ui_BadgeSetting, 160, 0);
    lv_obj_align(ui_BadgeSetting, LV_ALIGN_TOP_RIGHT, -7, 7);
    
    // 初始布局计算
    lv_obj_update_layout(ui_MainScreen);
}

void ui_MainScreen_screen_destroy(void)
{
    if(ui_MainScreen) lv_obj_del(ui_MainScreen);
    ui_MainScreen = NULL;
    ui_PanelTopTitle = NULL;
    ui_PanelCard4G = NULL;
    ui_LabelTxt4G = NULL;
    ui_LabelBleStatus = NULL;
    ui_WifiArc1 = NULL;
    ui_WifiArc2 = NULL;
    ui_WifiDot = NULL;
    ui_LabelBattery = NULL;
    ui_BatteryBody = NULL;
    ui_BatteryFill = NULL;
    ui_BatteryCap = NULL;
    ui_ClockLabel = NULL;
    ui_DateLabel = NULL;
    ui_PillOnline = NULL;
    ui_PillTemp = NULL;
    ui_StatusPill = NULL;
    ui_StatusDivider = NULL;
    ui_StatusFlipLine = NULL;
    ui_PanelMainShow = NULL;
    ui_ButtonAI = NULL;
    ui_ImageAI = NULL;
    ui_ButtonBluetooth = NULL;
    ui_ImageBluetooth = NULL;
    ui_ButtonScene = NULL;
    ui_ImageScene = NULL;
    ui_ButtonSetting = NULL;
    ui_ImageSetting = NULL;
    ui_BadgeSetting = NULL;
}

void ui_MainScreen_apply_focus_instant(int index)
{
    lv_obj_t * btns[4] = { ui_ButtonAI, ui_ButtonBluetooth, ui_ButtonScene, ui_ButtonSetting };
    for (int i = 0; i < 4; ++i) {
        if (btns[i] != NULL) {
            apply_nav_focus_visual(btns[i], i == index, false);
        }
    }
}

void ui_MainScreen_set_pill_portal_mode(bool portalMode)
{
    static const int32_t ONLINE_LABEL_WIDTH_NORMAL = 42;
    static const int32_t ONLINE_LABEL_WIDTH_PORTAL = 120;
    const lv_obj_flag_t collapseFlags = LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_IGNORE_LAYOUT;

    if (ui_StatusDivider != NULL) {
        if (portalMode) {
            lv_obj_add_flag(ui_StatusDivider, collapseFlags);
        } else {
            lv_obj_clear_flag(ui_StatusDivider, collapseFlags);
        }
    }
    if (ui_PillTemp != NULL) {
        if (portalMode) {
            lv_obj_add_flag(ui_PillTemp, collapseFlags);
        } else {
            lv_obj_clear_flag(ui_PillTemp, collapseFlags);
        }
    }
    if (ui_PillOnline != NULL) {
        lv_obj_t * onlineLabel = lv_obj_get_child(ui_PillOnline, 1);
        if (onlineLabel != NULL) {
            lv_obj_set_width(onlineLabel, portalMode ? ONLINE_LABEL_WIDTH_PORTAL : ONLINE_LABEL_WIDTH_NORMAL);
        }
    }
}
