/**
 * @file    ui_AIScreen.c
 * @brief   AI 表情屏界面 —— 状态机驱动的拟人眼睛动画
 *
 * 共 7 种状态（ai_state_t），对应不同表情与动画：
 *
 *  AI_STATE_IDLE       待命      随机眨眼 + 瞳孔漫游（自然待机）
 *  AI_STATE_LISTENING  聆听中    瞳孔向下盯声纹 + 左右扫视 + 眉微皱（专注）
 *  AI_STATE_THINKING   思考中    挑眉左右交替 + 瞳孔跟随上移 + 底部波浪跑马灯
 *  AI_STATE_SPEAKING   播报中    圆弧笑眼 ^^ + 眉微扬 + 笑眼随机抖动（活力）
 *  AI_STATE_ACK        已唤醒    瞳孔微上 + 眉微扬（惊喜/注意）
 *  AI_STATE_SUCCESS    已完成    圆弧笑眼 ^^ + 双眉高扬 + 静止开心
 *  AI_STATE_ERROR      出现问题   瞳孔下垂 + 眉内皱（皱眉）+ 偶尔眨眼
 */
 
#include "ui.h"
#include <stdlib.h>

lv_obj_t * ui_AIScreen;

// Eyes
lv_obj_t * ui_AIEyeLeft;
lv_obj_t * ui_AIEyeRight;
lv_obj_t * ui_AIPupilLeft;
lv_obj_t * ui_AIPupilRight;

// Eyebrows
lv_obj_t * ui_AIBrowLeft = NULL;
lv_obj_t * ui_AIBrowRight = NULL;
static lv_point_t brow_left_points[2];
static lv_point_t brow_right_points[2];

// Arc eyes for speaking ^ ^
static lv_obj_t * ui_AIArcLeft = NULL;
static lv_obj_t * ui_AIArcRight = NULL;

// Top status label
lv_obj_t * ui_AIStatusLabel = NULL;

// Waves
lv_obj_t * ui_AIWaves[AI_WAVE_COUNT];

static lv_timer_t * ai_anim_timer = NULL;
static bool ai_is_blinking = false;
static ai_state_t current_state = AI_STATE_IDLE;

// 聆听扫视方向控制
static int listen_scan_dir = 1; // 1=向右, -1=向左
static int listen_scan_x = 0;  // 当前扫视 x 位置

// 思考状态：挑眉相位
static int think_brow_phase = 0; // 0=左高右低, 1=右高左低
static uint32_t think_brow_last_switch = 0;
static int think_phase = 0; // 思考瞳孔相位（移到全局以便重置）

// 用于在 screen_init 后延迟应用状态
static ai_state_t pending_state = AI_STATE_IDLE;

// 随机生成器帮助函数
static int get_rand(int min, int max) {
    return min + rand() % ((max + 1) - min);
}

static lv_color_t ai_screen_bg_color(void) {
    return lv_color_hex(0x050510);
}

static lv_color_t ai_screen_text_color(void) {
    return lv_color_hex(0xFFFFFF);
}

static lv_color_t ai_screen_focus_color(void) {
    return lv_color_hex(0x00D4FF);
}

static lv_color_t ai_screen_pupil_color(void) {
    return lv_color_hex(0x000000);
}

// translate 动画回调包装函数（lv_obj_set_style_translate_x/y 需要3个参数，
// 但 lv_anim_exec_xcb_t 只传2个参数，直接用会导致 selector 是垃圾值）
static void set_translate_x(void * obj, int32_t v) {
    lv_obj_set_style_translate_x((lv_obj_t *)obj, (lv_coord_t)v, 0);
}
static void set_translate_y(void * obj, int32_t v) {
    lv_obj_set_style_translate_y((lv_obj_t *)obj, (lv_coord_t)v, 0);
}

// 眼珠转动动画
static void pupil_move_anim(lv_obj_t * pupil, int target_x, int target_y) {
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, pupil);
    lv_anim_set_values(&a, lv_obj_get_style_translate_x(pupil, 0), target_x);
    lv_anim_set_time(&a, 300);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_exec_cb(&a, set_translate_x);
    lv_anim_start(&a);

    lv_anim_t a_y;
    lv_anim_init(&a_y);
    lv_anim_set_var(&a_y, pupil);
    lv_anim_set_values(&a_y, lv_obj_get_style_translate_y(pupil, 0), target_y);
    lv_anim_set_time(&a_y, 300);
    lv_anim_set_path_cb(&a_y, lv_anim_path_ease_in_out);
    lv_anim_set_exec_cb(&a_y, set_translate_y);
    lv_anim_start(&a_y);
}

// 眉毛位移动画（通过 translate_y 控制高低）
static void brow_move_anim(lv_obj_t * brow, int target_y, int duration) {
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, brow);
    lv_anim_set_values(&a, lv_obj_get_style_translate_y(brow, 0), target_y);
    lv_anim_set_time(&a, duration);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_exec_cb(&a, set_translate_y);
    lv_anim_start(&a);
}

// 设置眉毛角度（通过倾斜两端点的 Y 来模拟旋转）
// tilt: 正值=外高内低（上扬），负值=外低内高（皱眉）
static void brow_set_tilt(lv_obj_t * brow, lv_point_t * points, bool is_left, int tilt) {
    if (!brow) return;
    // 眉毛线段 16px 宽
    points[0].x = 0;
    points[1].x = 16;
    if (is_left) {
        // 左眉：内侧(右端)高，外侧(左端)低 → tilt正值=上扬
        points[0].y = tilt;   // 外侧
        points[1].y = -tilt;  // 内侧
    } else {
        // 右眉：内侧(左端)高，外侧(右端)低
        points[0].y = -tilt;  // 内侧
        points[1].y = tilt;   // 外侧
    }
    lv_line_set_points(brow, points, 2);
}

static void blink_finish_reset_cb(lv_anim_t *a) {
    ai_is_blinking = false;
}

// 眨眼动画结束回调（睁开）
static void blink_ready_cb(lv_anim_t * a) {
    lv_obj_t * obj = (lv_obj_t *)a->var;
    
    lv_anim_t a_h;
    lv_anim_init(&a_h);
    lv_anim_set_var(&a_h, obj);
    lv_anim_set_values(&a_h, lv_obj_get_height(obj), 40); // 恢复原高
    lv_anim_set_time(&a_h, 150);
    lv_anim_set_path_cb(&a_h, lv_anim_path_overshoot); // 弹跳睁眼
    lv_anim_set_exec_cb(&a_h, (lv_anim_exec_xcb_t)lv_obj_set_height);
    
    // 只有左眼触发重置标志位，避免重复执行
    if (obj == ui_AIEyeLeft) {
        lv_anim_set_ready_cb(&a_h, blink_finish_reset_cb);
    }
    
    lv_anim_start(&a_h);
}

// 通用眨眼触发
static void do_blink(void) {
    ai_is_blinking = true;
    lv_anim_t aL;
    lv_anim_init(&aL);
    lv_anim_set_var(&aL, ui_AIEyeLeft);
    lv_anim_set_values(&aL, lv_obj_get_height(ui_AIEyeLeft), 4);
    lv_anim_set_time(&aL, 100);
    lv_anim_set_exec_cb(&aL, (lv_anim_exec_xcb_t)lv_obj_set_height);
    lv_anim_set_ready_cb(&aL, blink_ready_cb);
    lv_anim_start(&aL);

    lv_anim_t aR;
    lv_anim_init(&aR);
    lv_anim_set_var(&aR, ui_AIEyeRight);
    lv_anim_set_values(&aR, lv_obj_get_height(ui_AIEyeRight), 4);
    lv_anim_set_time(&aR, 100);
    lv_anim_set_exec_cb(&aR, (lv_anim_exec_xcb_t)lv_obj_set_height);
    lv_anim_set_ready_cb(&aR, blink_ready_cb);
    lv_anim_start(&aR);
}

// ========== 各状态的动画逻辑（直接在 set_state 中配置） ==========

// 眉毛重置到自然状态
static void brow_reset(void) {
    if (ui_AIBrowLeft) {
        lv_anim_del(ui_AIBrowLeft, NULL);
        lv_obj_set_style_translate_y(ui_AIBrowLeft, 0, 0);
        brow_set_tilt(ui_AIBrowLeft, brow_left_points, true, 0);
        lv_obj_clear_flag(ui_AIBrowLeft, LV_OBJ_FLAG_HIDDEN);
    }
    if (ui_AIBrowRight) {
        lv_anim_del(ui_AIBrowRight, NULL);
        lv_obj_set_style_translate_y(ui_AIBrowRight, 0, 0);
        brow_set_tilt(ui_AIBrowRight, brow_right_points, false, 0);
        lv_obj_clear_flag(ui_AIBrowRight, LV_OBJ_FLAG_HIDDEN);
    }
}

// 聆听动画：眼珠看向下方声纹区域，眉毛自然微倾
static void apply_listening_anim(void) {
    // 眼珠深度向下看，朝声纹区域
    pupil_move_anim(ui_AIPupilLeft, 0, 15);
    pupil_move_anim(ui_AIPupilRight, 0, 15);
    // 重置扫视状态
    listen_scan_dir = 1;
    listen_scan_x = 0;
    // 眉毛：微微外低内高（专注/乖巧）
    brow_set_tilt(ui_AIBrowLeft, brow_left_points, true, -1);
    brow_set_tilt(ui_AIBrowRight, brow_right_points, false, -1);
}

// 思考过渡动画：眼珠先平滑回到中心，眉毛开始挑眉
static void apply_thinking_transition(void) {
    // 从聆听位置（下方）平滑动画回到中心 (0,0)
    pupil_move_anim(ui_AIPupilLeft, 0, 0);
    pupil_move_anim(ui_AIPupilRight, 0, 0);
    // 重置思考相位
    think_phase = 0;
    think_brow_phase = 0;
    think_brow_last_switch = lv_tick_get();
    // 初始挑眉：左高右低
    brow_set_tilt(ui_AIBrowLeft, brow_left_points, true, 2);
    brow_set_tilt(ui_AIBrowRight, brow_right_points, false, -2);
    brow_move_anim(ui_AIBrowLeft, -3, 400);  // 左眉上移
    brow_move_anim(ui_AIBrowRight, 2, 400);  // 右眉下移
}

// 完成/开心动画
static void apply_ack_anim(void) {
    pupil_move_anim(ui_AIPupilLeft, 0, -2);
    pupil_move_anim(ui_AIPupilRight, 0, -2);
    brow_set_tilt(ui_AIBrowLeft, brow_left_points, true, 1);
    brow_set_tilt(ui_AIBrowRight, brow_right_points, false, 1);
    brow_move_anim(ui_AIBrowLeft, -2, 250);
    brow_move_anim(ui_AIBrowRight, -2, 250);
}

static void apply_success_anim(void) {
    // 隐藏普通眼睛，显示 ^ ^ 弧线（复用说话的笑眼）
    lv_obj_add_flag(ui_AIEyeLeft, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_AIEyeRight, LV_OBJ_FLAG_HIDDEN);
    
    if (ui_AIArcLeft) {
        lv_obj_clear_flag(ui_AIArcLeft, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_translate_y(ui_AIArcLeft, 0, 0);
    }
    if (ui_AIArcRight) {
        lv_obj_clear_flag(ui_AIArcRight, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_translate_y(ui_AIArcRight, 0, 0);
    }
    // 眉毛：双眉上扬（开心）
    brow_set_tilt(ui_AIBrowLeft, brow_left_points, true, 2);
    brow_set_tilt(ui_AIBrowRight, brow_right_points, false, 2);
    brow_move_anim(ui_AIBrowLeft, -4, 300);
    brow_move_anim(ui_AIBrowRight, -4, 300);
}

static void apply_error_anim(void) {
    pupil_move_anim(ui_AIPupilLeft, 0, 2);
    pupil_move_anim(ui_AIPupilRight, 0, 2);
    brow_set_tilt(ui_AIBrowLeft, brow_left_points, true, -2);
    brow_set_tilt(ui_AIBrowRight, brow_right_points, false, -2);
    brow_move_anim(ui_AIBrowLeft, 1, 250);
    brow_move_anim(ui_AIBrowRight, 1, 250);
}

// 执行动画：设备正在派发动作（BLE 发射等），瞳孔同步左右乒乓 + 波浪齐步呼吸
static void apply_executing_anim(void) {
    // 瞳孔：定位到略上方，启动无限循环左右乒乓
    lv_obj_set_style_translate_y(ui_AIPupilLeft, -2, 0);
    lv_obj_set_style_translate_y(ui_AIPupilRight, -2, 0);
    lv_obj_set_style_translate_x(ui_AIPupilLeft, 0, 0);
    lv_obj_set_style_translate_x(ui_AIPupilRight, 0, 0);

    lv_anim_t aL;
    lv_anim_init(&aL);
    lv_anim_set_var(&aL, ui_AIPupilLeft);
    lv_anim_set_values(&aL, -3, 3);
    lv_anim_set_time(&aL, 1200);
    lv_anim_set_playback_time(&aL, 1200);
    lv_anim_set_repeat_count(&aL, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&aL, lv_anim_path_ease_in_out);
    lv_anim_set_exec_cb(&aL, set_translate_x);
    lv_anim_start(&aL);

    lv_anim_t aR;
    lv_anim_init(&aR);
    lv_anim_set_var(&aR, ui_AIPupilRight);
    lv_anim_set_values(&aR, -3, 3);
    lv_anim_set_time(&aR, 1200);
    lv_anim_set_playback_time(&aR, 1200);
    lv_anim_set_repeat_count(&aR, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&aR, lv_anim_path_ease_in_out);
    lv_anim_set_exec_cb(&aR, set_translate_x);
    lv_anim_start(&aR);

    // 眉毛：平直略上提（专注 / 准备好）
    brow_set_tilt(ui_AIBrowLeft, brow_left_points, true, 0);
    brow_set_tilt(ui_AIBrowRight, brow_right_points, false, 0);
    brow_move_anim(ui_AIBrowLeft, -1, 250);
    brow_move_anim(ui_AIBrowRight, -1, 250);
}

// 说话动画：把普通眼睛隐藏，显示 ^ ^ 弧线
static void apply_speaking_anim(void) {
    // 隐藏普通眼睛，显示 ^ ^ 弧线
    lv_obj_add_flag(ui_AIEyeLeft, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_AIEyeRight, LV_OBJ_FLAG_HIDDEN);
    
    if (ui_AIArcLeft) {
        lv_obj_clear_flag(ui_AIArcLeft, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_translate_y(ui_AIArcLeft, 0, 0); // 复位
    }
    if (ui_AIArcRight) {
        lv_obj_clear_flag(ui_AIArcRight, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_translate_y(ui_AIArcRight, 0, 0); // 复位
    }
    // 眉毛：微微上扬（愉悦）
    brow_set_tilt(ui_AIBrowLeft, brow_left_points, true, 1);
    brow_set_tilt(ui_AIBrowRight, brow_right_points, false, 1);
    brow_move_anim(ui_AIBrowLeft, -2, 300);
    brow_move_anim(ui_AIBrowRight, -2, 300);
}

// 定时器回调：根据状态执行不同的动画
static void ai_anim_timer_cb(lv_timer_t * timer) {
    if (!ui_AIScreen || lv_scr_act() != ui_AIScreen) return;

    if (current_state == AI_STATE_IDLE) {
        int action = get_rand(0, 10);
        if (action < 3 && !ai_is_blinking) {
            do_blink();
        } else {
            int target_x = get_rand(-6, 6);
            int target_y = get_rand(-6, 6);
            pupil_move_anim(ui_AIPupilLeft, target_x, target_y);
            pupil_move_anim(ui_AIPupilRight, target_x, target_y);
        }
        lv_timer_set_period(ai_anim_timer, get_rand(800, 3000));
        
    } else if (current_state == AI_STATE_LISTENING) {
        // 聆听时：眼珠深度向下看声纹，左右来回扫视（仿佛在阅读声纹）
        if (get_rand(0, 15) < 2 && !ai_is_blinking) {
            do_blink();
        } else {
            // 来回扫视：到达边界时反转方向
            listen_scan_x += listen_scan_dir * get_rand(2, 4);
            if (listen_scan_x > 6) { listen_scan_x = 6; listen_scan_dir = -1; }
            if (listen_scan_x < -6) { listen_scan_x = -6; listen_scan_dir = 1; }
            
            // 始终保持向下看 y=15
            pupil_move_anim(ui_AIPupilLeft, listen_scan_x, 15);
            pupil_move_anim(ui_AIPupilRight, listen_scan_x, 15);
        }
        lv_timer_set_period(ai_anim_timer, get_rand(400, 800));
        
    } else if (current_state == AI_STATE_THINKING) {
        // 思考时：挑眉+瞳孔看向挑眉方向 + 底部波浪跑马灯
        
        // 每2秒切换挑眉方向
        uint32_t now = lv_tick_get();
        if (now - think_brow_last_switch > 2000) {
            think_brow_phase = 1 - think_brow_phase;
            think_brow_last_switch = now;
            
            if (think_brow_phase == 0) {
                // 左高右低
                brow_set_tilt(ui_AIBrowLeft, brow_left_points, true, 2);
                brow_set_tilt(ui_AIBrowRight, brow_right_points, false, -2);
                brow_move_anim(ui_AIBrowLeft, -3, 500);
                brow_move_anim(ui_AIBrowRight, 2, 500);
            } else {
                // 右高左低
                brow_set_tilt(ui_AIBrowLeft, brow_left_points, true, -2);
                brow_set_tilt(ui_AIBrowRight, brow_right_points, false, 2);
                brow_move_anim(ui_AIBrowLeft, 2, 500);
                brow_move_anim(ui_AIBrowRight, -3, 500);
            }
        }
        
        // 瞳孔看向挑眉方向（2个相位交替：左上 ↔ 右上）
        if (think_brow_phase == 0) {
            // 左眉高 → 看左上
            pupil_move_anim(ui_AIPupilLeft, -6, -4);
            pupil_move_anim(ui_AIPupilRight, -6, -4);
        } else {
            // 右眉高 → 看右上
            pupil_move_anim(ui_AIPupilLeft, 6, -4);
            pupil_move_anim(ui_AIPupilRight, 6, -4);
        }
        think_phase++;
        
        // 波浪跑马灯效果
        static int wave_step = 0;
        for (int i = 0; i < AI_WAVE_COUNT; i++) {
            float brightness = 0.15f;
            if (i == wave_step) brightness = 1.0f;
            else if (i == (wave_step + 1) % AI_WAVE_COUNT) brightness = 0.55f;
            else if (i == (wave_step + AI_WAVE_COUNT - 1) % AI_WAVE_COUNT) brightness = 0.55f;
            
            int h = 4 + (int)(26.0f * brightness);
            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, ui_AIWaves[i]);
            lv_anim_set_values(&a, lv_obj_get_height(ui_AIWaves[i]), h);
            lv_anim_set_time(&a, 250);
            lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
            lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_height);
            lv_anim_start(&a);
        }
        wave_step = (wave_step + 1) % AI_WAVE_COUNT;

        lv_timer_set_period(ai_anim_timer, 500);

    } else if (current_state == AI_STATE_EXECUTING) {
        // 执行中：5 根波浪齐步呼吸（与 THINKING 的相位错开跑马灯区分）
        // 瞳孔乒乓由 apply_executing_anim 的无限循环动画驱动，timer 不再管
        static int exec_phase = 0;
        int h_target = (exec_phase & 1) ? 6 : 26;
        for (int i = 0; i < AI_WAVE_COUNT; i++) {
            if (!ui_AIWaves[i]) continue;
            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, ui_AIWaves[i]);
            lv_anim_set_values(&a, lv_obj_get_height(ui_AIWaves[i]), h_target);
            lv_anim_set_time(&a, 320);
            lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
            lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_height);
            lv_anim_start(&a);
        }
        exec_phase++;
        lv_timer_set_period(ai_anim_timer, 350);

    } else if (current_state == AI_STATE_SPEAKING) {
        // 说话时：笑眼微抖，随机上下抖动 ^ ^ 增加活力
        int dy = get_rand(-2, 2);
        
        if (ui_AIArcLeft && ui_AIArcRight) {
            lv_anim_t aL;
            lv_anim_init(&aL);
            lv_anim_set_var(&aL, ui_AIArcLeft);
            lv_anim_set_values(&aL, lv_obj_get_style_translate_y(ui_AIArcLeft, 0), dy);
            lv_anim_set_time(&aL, 150);
            lv_anim_set_exec_cb(&aL, set_translate_y);
            lv_anim_start(&aL);
            
            lv_anim_t aR;
            lv_anim_init(&aR);
            lv_anim_set_var(&aR, ui_AIArcRight);
            lv_anim_set_values(&aR, lv_obj_get_style_translate_y(ui_AIArcRight, 0), dy);
            lv_anim_set_time(&aR, 150);
            lv_anim_set_exec_cb(&aR, set_translate_y);
            lv_anim_start(&aR);
        }
        
        lv_timer_set_period(ai_anim_timer, 250);
    } else if (current_state == AI_STATE_SUCCESS) {
        // 完成状态：笑眼不抖动，保持静止的开心表情
        // 不做周期性动画，只需定时器保持活跃即可
        lv_timer_set_period(ai_anim_timer, 2000);
    } else if (current_state == AI_STATE_ERROR) {
        if (get_rand(0, 12) < 2 && !ai_is_blinking) {
            do_blink();
        }
        lv_timer_set_period(ai_anim_timer, 900);
    }
}

void ui_AIScreen_set_state(ai_state_t state) {
    // 保存目标状态（供 screen_init 后使用）
    pending_state = state;
    
    if (current_state == state) return;
    current_state = state;
    
    // 安全检查：如果控件还没初始化就先不操作
    if (!ui_AIEyeLeft || !ui_AIEyeRight || !ui_AIPupilLeft || !ui_AIPupilRight) return;
    
    // ===== 更新顶部状态文字 =====
    if (ui_AIStatusLabel) {
        switch (state) {
            case AI_STATE_IDLE:      lv_label_set_text(ui_AIStatusLabel, "待命"); break;
            case AI_STATE_LISTENING: lv_label_set_text(ui_AIStatusLabel, "聆听中..."); break;
            case AI_STATE_THINKING:  lv_label_set_text(ui_AIStatusLabel, "思考中..."); break;
            case AI_STATE_EXECUTING: lv_label_set_text(ui_AIStatusLabel, "执行中..."); break;
            case AI_STATE_SPEAKING:  lv_label_set_text(ui_AIStatusLabel, "播报中..."); break;
            case AI_STATE_ACK:       lv_label_set_text(ui_AIStatusLabel, "已唤醒"); break;
            case AI_STATE_SUCCESS:   lv_label_set_text(ui_AIStatusLabel, "已完成"); break;
            case AI_STATE_ERROR:     lv_label_set_text(ui_AIStatusLabel, "出现问题"); break;
        }
    }
    
    // 停止所有正在运行的动画
    lv_anim_del(ui_AIEyeLeft, NULL);
    lv_anim_del(ui_AIEyeRight, NULL);
    lv_anim_del(ui_AIPupilLeft, NULL);
    lv_anim_del(ui_AIPupilRight, NULL);
    for (int i = 0; i < AI_WAVE_COUNT; i++) {
        if (ui_AIWaves[i]) lv_anim_del(ui_AIWaves[i], NULL);
    }
    
    // 恢复基准状态
    lv_obj_clear_flag(ui_AIEyeLeft, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(ui_AIEyeRight, LV_OBJ_FLAG_HIDDEN);
    if (ui_AIArcLeft) lv_obj_add_flag(ui_AIArcLeft, LV_OBJ_FLAG_HIDDEN);
    if (ui_AIArcRight) lv_obj_add_flag(ui_AIArcRight, LV_OBJ_FLAG_HIDDEN);

    lv_obj_set_height(ui_AIEyeLeft, 46);
    lv_obj_set_height(ui_AIEyeRight, 46);
    lv_obj_set_style_translate_y(ui_AIPupilLeft, 0, 0);
    lv_obj_set_style_translate_y(ui_AIPupilRight, 0, 0);
    lv_obj_set_style_translate_x(ui_AIPupilLeft, 0, 0);
    lv_obj_set_style_translate_x(ui_AIPupilRight, 0, 0);
    lv_obj_clear_flag(ui_AIPupilLeft, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(ui_AIPupilRight, LV_OBJ_FLAG_HIDDEN);
    
    // 重置眉毛
    brow_reset();
    
    // 波浪恢复到初始高度
    for (int i = 0; i < AI_WAVE_COUNT; i++) {
        if (ui_AIWaves[i]) {
            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, ui_AIWaves[i]);
            lv_anim_set_values(&a, lv_obj_get_height(ui_AIWaves[i]), 4);
            lv_anim_set_time(&a, 300);
            lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
            lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_height);
            lv_anim_start(&a);
        }
    }
    ai_is_blinking = false;

    // 根据新状态立即应用初始动画
    switch (state) {
        case AI_STATE_ACK:
            apply_ack_anim();
            break;
        case AI_STATE_LISTENING:
            apply_listening_anim();
            break;
        case AI_STATE_THINKING:
            apply_thinking_transition(); // 先平滑回中心
            break;
        case AI_STATE_EXECUTING:
            apply_executing_anim();
            break;
        case AI_STATE_SPEAKING:
            apply_speaking_anim();
            break;
        case AI_STATE_SUCCESS:
            apply_success_anim();
            break;
        case AI_STATE_ERROR:
            apply_error_anim();
            break;
        default:
            break;
    }

    // 如果定时器已存在，设置适当延迟后触发
    if (ai_anim_timer) {
        // 思考状态需要等瞳孔回中心动画(300ms)完成后再开始
        int delay = (state == AI_STATE_THINKING) ? 400 : 50;
        lv_timer_set_period(ai_anim_timer, delay);
    }
}

// 供外部调用：更新波纹
void ui_AIScreen_update_wave(uint8_t volume) {
    if (!ui_AIScreen || lv_scr_act() != ui_AIScreen) return;
    if (current_state == AI_STATE_THINKING) return; // 思考状态自己控制波浪

    // Volume: 0-100
    // 波浪映射：中间高，两边低，微调系数使整体更饱满
    static float base_heights[AI_WAVE_COUNT] = { 0.4f, 0.7f, 1.0f, 0.7f, 0.4f };
    
    for (int i = 0; i < AI_WAVE_COUNT; i++) {
        // 最小高度 6px (低音量可见基准)
        // 最大动态高度 42px -> 总高度最大 48px (容器高度 50px，预留 2px buffer)
        // 变化幅度扩大：原 26px -> 现 42px (约 1.6 倍)
        int h = 6 + (int)((float)volume / 100.0f * 42.0f * base_heights[i]);
        
        // 边界限制
        if (h > 48) h = 48;
        if (h < 6) h = 6;
        
        // 恢复动画以实现更自然的弹性伸缩效果
        // 时间设为 50ms (略大于 30fps 的 33ms 帧间隔)，配合 ease_out 实现灵动感
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, ui_AIWaves[i]);
        lv_anim_set_values(&a, lv_obj_get_height(ui_AIWaves[i]), h);
        lv_anim_set_time(&a, 50); 
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out); // 缓动函数
        lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_height);
        lv_anim_start(&a);
    }
}

// 开始动画
void ui_AIScreen_start_anim(void) {
    if(ai_anim_timer == NULL) {
        ai_anim_timer = lv_timer_create(ai_anim_timer_cb, 100, NULL); // 首次100ms快速生效
    }
    ai_is_blinking = false;
    
    // 如果有 pending state，在 timer 创建后重新应用
    if (pending_state != AI_STATE_IDLE) {
        current_state = AI_STATE_IDLE; // 先重置，让 set_state 能生效
        ui_AIScreen_set_state(pending_state);
    }
}

// 停止动画
void ui_AIScreen_stop_anim(void) {
    if(ai_anim_timer != NULL) {
        lv_timer_del(ai_anim_timer);
        ai_anim_timer = NULL;
    }
    current_state = AI_STATE_IDLE;
    pending_state = AI_STATE_IDLE;
}

// 自定义顶部状态文字（由业务层调用，覆盖 set_state 的默认文字）
void ui_AIScreen_set_status_text(const char* text) {
    if (!ui_AIStatusLabel || !text) return;
    lv_label_set_text(ui_AIStatusLabel, text);
}

void ui_AIScreen_screen_init(void) {
    ui_AIScreen = lv_obj_create(NULL);
    lv_obj_clear_flag(ui_AIScreen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(ui_AIScreen, 240, 240);

    // AI 表情屏在亮色/暗色主题下统一使用暗色视觉，避免眼睛语义随主题反转。
    lv_obj_set_style_bg_color(ui_AIScreen, ai_screen_bg_color(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui_AIScreen, 255, LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(ui_AIScreen, ai_screen_bg_color(), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(ui_AIScreen, LV_GRAD_DIR_VER, LV_PART_MAIN);

    // ========== 顶部状态文字 ==========
    ui_AIStatusLabel = lv_label_create(ui_AIScreen);
    lv_label_set_text(ui_AIStatusLabel, "待命");
    lv_obj_set_style_text_font(ui_AIStatusLabel, &my_font_misans_20, 0);
    lv_obj_set_style_text_color(ui_AIStatusLabel, ai_screen_text_color(), 0);
    lv_obj_set_style_text_opa(ui_AIStatusLabel, 150, 0);
    lv_obj_align(ui_AIStatusLabel, LV_ALIGN_TOP_MID, 0, 18);

    // ========== 眼睛部分 ==========
    // 240x240 屏幕: 眼距 50px，左眼 centerX = 120-25 = 95, 右眼 centerX = 120+25 = 145
    // 眼睛大小 28x46
    
    // -- 左眼 --
    ui_AIEyeLeft = lv_obj_create(ui_AIScreen);
    lv_obj_set_size(ui_AIEyeLeft, 28, 46);
    lv_obj_align(ui_AIEyeLeft, LV_ALIGN_CENTER, -25, -20);
    lv_obj_clear_flag(ui_AIEyeLeft, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(ui_AIEyeLeft, ai_screen_text_color(), LV_PART_MAIN);
    lv_obj_set_style_radius(ui_AIEyeLeft, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_border_width(ui_AIEyeLeft, 0, LV_PART_MAIN);
    // 用阴影做发光效果
    lv_obj_set_style_shadow_color(ui_AIEyeLeft, ai_screen_focus_color(), LV_PART_MAIN);
    lv_obj_set_style_shadow_width(ui_AIEyeLeft, 12, LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(ui_AIEyeLeft, 150, LV_PART_MAIN);

    ui_AIPupilLeft = lv_obj_create(ui_AIEyeLeft);
    lv_obj_set_size(ui_AIPupilLeft, 12, 12);
    lv_obj_align(ui_AIPupilLeft, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(ui_AIPupilLeft, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(ui_AIPupilLeft, ai_screen_pupil_color(), LV_PART_MAIN);
    lv_obj_set_style_radius(ui_AIPupilLeft, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_border_width(ui_AIPupilLeft, 0, LV_PART_MAIN);

    // -- 右眼 --
    ui_AIEyeRight = lv_obj_create(ui_AIScreen);
    lv_obj_set_size(ui_AIEyeRight, 28, 46);
    lv_obj_align(ui_AIEyeRight, LV_ALIGN_CENTER, 25, -20);
    lv_obj_clear_flag(ui_AIEyeRight, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(ui_AIEyeRight, ai_screen_text_color(), LV_PART_MAIN);
    lv_obj_set_style_radius(ui_AIEyeRight, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_border_width(ui_AIEyeRight, 0, LV_PART_MAIN);
    // 发光效果
    lv_obj_set_style_shadow_color(ui_AIEyeRight, ai_screen_focus_color(), LV_PART_MAIN);
    lv_obj_set_style_shadow_width(ui_AIEyeRight, 12, LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(ui_AIEyeRight, 150, LV_PART_MAIN);

    ui_AIPupilRight = lv_obj_create(ui_AIEyeRight);
    lv_obj_set_size(ui_AIPupilRight, 12, 12);
    lv_obj_align(ui_AIPupilRight, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(ui_AIPupilRight, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(ui_AIPupilRight, ai_screen_pupil_color(), LV_PART_MAIN);
    lv_obj_set_style_radius(ui_AIPupilRight, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_border_width(ui_AIPupilRight, 0, LV_PART_MAIN);

    // ========== 眉毛部分 ==========
    // 眉毛位于眼睛上方
    
    // -- 左眉 --
    brow_left_points[0].x = 0;
    brow_left_points[0].y = 0;
    brow_left_points[1].x = 20;
    brow_left_points[1].y = 0;
    
    ui_AIBrowLeft = lv_line_create(ui_AIScreen);
    lv_line_set_points(ui_AIBrowLeft, brow_left_points, 2);
    lv_obj_align(ui_AIBrowLeft, LV_ALIGN_CENTER, -25, -50);
    lv_obj_set_style_line_color(ui_AIBrowLeft, ai_screen_text_color(), LV_PART_MAIN);
    lv_obj_set_style_line_width(ui_AIBrowLeft, 3, LV_PART_MAIN);
    lv_obj_set_style_line_rounded(ui_AIBrowLeft, true, LV_PART_MAIN);
    
    // -- 右眉 --
    brow_right_points[0].x = 0;
    brow_right_points[0].y = 0;
    brow_right_points[1].x = 20;
    brow_right_points[1].y = 0;
    
    ui_AIBrowRight = lv_line_create(ui_AIScreen);
    lv_line_set_points(ui_AIBrowRight, brow_right_points, 2);
    lv_obj_align(ui_AIBrowRight, LV_ALIGN_CENTER, 25, -50);
    lv_obj_set_style_line_color(ui_AIBrowRight, ai_screen_text_color(), LV_PART_MAIN);
    lv_obj_set_style_line_width(ui_AIBrowRight, 3, LV_PART_MAIN);
    lv_obj_set_style_line_rounded(ui_AIBrowRight, true, LV_PART_MAIN);

    // -- 说话时的 ^ ^ 左眼 --
    ui_AIArcLeft = lv_arc_create(ui_AIScreen);
    lv_obj_set_size(ui_AIArcLeft, 28, 28);
    lv_obj_align(ui_AIArcLeft, LV_ALIGN_CENTER, -25, -16);
    // 先移除默认的 indicator 和 knob 样式
    lv_obj_remove_style(ui_AIArcLeft, NULL, LV_PART_INDICATOR);
    lv_obj_remove_style(ui_AIArcLeft, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(ui_AIArcLeft, LV_OBJ_FLAG_CLICKABLE);
    // 设置背景弧线角度和样式（用 PART_MAIN 画半圆弧）
    lv_arc_set_bg_angles(ui_AIArcLeft, 180, 360);
    lv_obj_set_style_arc_color(ui_AIArcLeft, ai_screen_text_color(), LV_PART_MAIN);
    lv_obj_set_style_arc_width(ui_AIArcLeft, 4, LV_PART_MAIN);
    // 去除矩形背景、边框、阴影
    lv_obj_set_style_bg_opa(ui_AIArcLeft, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(ui_AIArcLeft, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(ui_AIArcLeft, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(ui_AIArcLeft, 0, LV_PART_MAIN);
    lv_obj_add_flag(ui_AIArcLeft, LV_OBJ_FLAG_HIDDEN); // 默认隐藏
    
    // -- 说话时的 ^ ^ 右眼 --
    ui_AIArcRight = lv_arc_create(ui_AIScreen);
    lv_obj_set_size(ui_AIArcRight, 28, 28);
    lv_obj_align(ui_AIArcRight, LV_ALIGN_CENTER, 25, -16);
    // 先移除默认的 indicator 和 knob 样式
    lv_obj_remove_style(ui_AIArcRight, NULL, LV_PART_INDICATOR);
    lv_obj_remove_style(ui_AIArcRight, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(ui_AIArcRight, LV_OBJ_FLAG_CLICKABLE);
    // 设置背景弧线角度和样式
    lv_arc_set_bg_angles(ui_AIArcRight, 180, 360);
    lv_obj_set_style_arc_color(ui_AIArcRight, ai_screen_text_color(), LV_PART_MAIN);
    lv_obj_set_style_arc_width(ui_AIArcRight, 4, LV_PART_MAIN);
    // 去除矩形背景、边框、阴影
    lv_obj_set_style_bg_opa(ui_AIArcRight, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(ui_AIArcRight, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(ui_AIArcRight, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(ui_AIArcRight, 0, LV_PART_MAIN);
    lv_obj_add_flag(ui_AIArcRight, LV_OBJ_FLAG_HIDDEN); // 默认隐藏

    // ========== 底部声波波纹 ==========
    // 放置在一个容器中，方便整体对齐
    lv_obj_t * wave_container = lv_obj_create(ui_AIScreen);
    lv_obj_set_size(wave_container, 200, 60);
    lv_obj_align(wave_container, LV_ALIGN_BOTTOM_MID, 0, -15);
    lv_obj_set_style_bg_opa(wave_container, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(wave_container, 0, LV_PART_MAIN);
    lv_obj_clear_flag(wave_container, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_set_layout(wave_container, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(wave_container, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(wave_container, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    for (int i = 0; i < AI_WAVE_COUNT; i++) {
        ui_AIWaves[i] = lv_obj_create(wave_container);
        lv_obj_set_size(ui_AIWaves[i], 10, 6);
        lv_obj_clear_flag(ui_AIWaves[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_color(ui_AIWaves[i], ai_screen_focus_color(), LV_PART_MAIN);
        lv_obj_set_style_radius(ui_AIWaves[i], LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_border_width(ui_AIWaves[i], 0, LV_PART_MAIN);
        lv_obj_set_style_shadow_color(ui_AIWaves[i], ai_screen_focus_color(), LV_PART_MAIN);
        lv_obj_set_style_shadow_width(ui_AIWaves[i], 10, LV_PART_MAIN);
    }
}
