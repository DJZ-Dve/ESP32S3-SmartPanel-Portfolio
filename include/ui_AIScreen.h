#ifndef _UI_AISCREEN_H
#define _UI_AISCREEN_H

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

extern lv_obj_t * ui_AIScreen;
extern lv_obj_t * ui_AIEyeLeft;
extern lv_obj_t * ui_AIEyeRight;
extern lv_obj_t * ui_AIPupilLeft;
extern lv_obj_t * ui_AIPupilRight;

// Eyebrows
extern lv_obj_t * ui_AIBrowLeft;
extern lv_obj_t * ui_AIBrowRight;

// Top status label
extern lv_obj_t * ui_AIStatusLabel;

// Wave bars
#define AI_WAVE_COUNT 5
extern lv_obj_t * ui_AIWaves[AI_WAVE_COUNT];

typedef enum {
    AI_STATE_IDLE,
    AI_STATE_ACK,
    AI_STATE_LISTENING,
    AI_STATE_THINKING,
    AI_STATE_EXECUTING,
    AI_STATE_SPEAKING,
    AI_STATE_SUCCESS,
    AI_STATE_ERROR
} ai_state_t;

void ui_AIScreen_screen_init(void);

// Exported control methods
void ui_AIScreen_set_state(ai_state_t state);
void ui_AIScreen_start_anim(void);
void ui_AIScreen_stop_anim(void);
void ui_AIScreen_update_wave(uint8_t volume); // volume 0-100
void ui_AIScreen_set_status_text(const char* text); // 自定义顶部状态文字

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif
