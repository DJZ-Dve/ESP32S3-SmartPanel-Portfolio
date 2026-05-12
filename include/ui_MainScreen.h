#ifndef UI_MAINSCREEN_H
#define UI_MAINSCREEN_H

#ifdef __cplusplus
extern "C" {
#endif

// SCREEN: ui_MainScreen
extern void ui_MainScreen_screen_init(void);
extern void ui_MainScreen_screen_destroy(void);
extern void ui_MainScreen_apply_focus_instant(int index);
extern lv_obj_t * ui_MainScreen;
extern lv_obj_t * ui_PanelTopTitle;
// 4G UI ("4G" 文本，柱状信号条已移除)
extern lv_obj_t * ui_PanelCard4G;
extern lv_obj_t * ui_LabelTxt4G;
extern lv_obj_t * ui_LabelBleStatus;
// WiFi UI (同心弧扇形 + 圆点)
extern lv_obj_t * ui_WifiArc1;
extern lv_obj_t * ui_WifiArc2;
extern lv_obj_t * ui_WifiDot;
// Battery UI
extern lv_obj_t * ui_LabelBattery;
extern lv_obj_t * ui_BatteryBody;
extern lv_obj_t * ui_BatteryFill;
extern lv_obj_t * ui_BatteryCap;
// 大时钟
extern lv_obj_t * ui_ClockLabel;
extern lv_obj_t * ui_DateLabel;
// 状态药丸
extern lv_obj_t * ui_PillOnline;
extern lv_obj_t * ui_PillTemp;
// AP 配网期间将状态药丸折叠为单段：隐藏分隔条 + 温度段，加宽在线段
extern void ui_MainScreen_set_pill_portal_mode(bool portalMode);
// 底部导航
extern lv_obj_t * ui_PanelMainShow;
extern lv_obj_t * ui_ButtonAI;
extern lv_obj_t * ui_ImageAI;
extern lv_obj_t * ui_ButtonBluetooth;
extern lv_obj_t * ui_ImageBluetooth;
extern lv_obj_t * ui_ButtonScene;
extern lv_obj_t * ui_ImageScene;
extern lv_obj_t * ui_ButtonSetting;
extern lv_obj_t * ui_ImageSetting;
extern lv_obj_t * ui_BadgeSetting;

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif
