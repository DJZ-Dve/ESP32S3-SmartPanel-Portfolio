#include "App_IdfUi.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "App_FlashGuard.h"
#include "App_IdfAppMode.h"
#include "App_IdfAudio.h"
#include "App_IdfBleAircon.h"
#include "App_IdfCellular.h"
#include "App_IdfDisplay.h"
#include "App_IdfLearnFlow.h"
#include "App_IdfLvgl.h"
#include "App_IdfNetwork.h"
#include "App_IdfOta.h"
#include "App_IdfRecorder.h"
#include "App_IdfScene.h"
#include "App_IdfSensors.h"
#include "App_IdfTime.h"
#include "App_IdfTransport.h"
#include "App_Log.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include "lvgl.h"
#include "ui.h"
#include "ui_AIScreen.h"
#include "ui_MainScreen.h"
#include "ui_theme.h"

namespace AppIdfUi {
namespace {

constexpr const char* TAG_UI_IDF = "IDF_UI";
constexpr const char* kUiPrefsNamespace = "ui";
constexpr const char* kUiPrefsThemeKey = "theme";
constexpr int kSettingsItemCount = 6;
constexpr int kSettingsPopupRowCount = 3;
constexpr int kBlePairRowCount = 6;
constexpr int kMaxSceneRows = 20;

enum class SettingsItem : int {
    Volume = 0,
    Network = 1,
    WifiReset = 2,
    Theme = 3,
    Mode = 4,
    Firmware = 5,
};

enum class SettingsPopupMode : int {
    None = 0,
    Volume,
    Network,
    WifiReset,
    Theme,
    Mode,
    ModeConfirm,
};

lv_group_t* g_mainGroup = nullptr;
lv_timer_t* g_statusTimer = nullptr;
lv_timer_t* g_aiWaveTimer = nullptr;
lv_obj_t* g_statusVolumeLabel = nullptr;
lv_obj_t* g_settingsScreen = nullptr;
lv_obj_t* g_settingsItems[kSettingsItemCount] = {};
lv_obj_t* g_settingsValueLabels[kSettingsItemCount] = {};
lv_obj_t* g_settingsPopupMask = nullptr;
lv_obj_t* g_settingsPopupCard = nullptr;
lv_obj_t* g_settingsPopupTitle = nullptr;
lv_obj_t* g_settingsPopupList = nullptr;
lv_obj_t* g_settingsPopupRows[kSettingsPopupRowCount] = {};
lv_obj_t* g_settingsPopupRowLabels[kSettingsPopupRowCount] = {};
lv_obj_t* g_blePairScreen = nullptr;
lv_obj_t* g_blePairRows[kBlePairRowCount] = {};
lv_obj_t* g_blePairNameLabels[kBlePairRowCount] = {};
lv_obj_t* g_blePairStatusLabel = nullptr;
lv_obj_t* g_blePairPopupMask = nullptr;
lv_obj_t* g_blePairPopupTitle = nullptr;
lv_obj_t* g_blePairPopupDetail = nullptr;
lv_obj_t* g_sceneScreen = nullptr;
lv_obj_t* g_sceneTitle = nullptr;
lv_obj_t* g_sceneRows[kMaxSceneRows] = {};
lv_obj_t* g_sceneRowLabels[kMaxSceneRows] = {};
lv_obj_t* g_sceneEmptyLabel = nullptr;
int g_sceneFocusIndex = 0;
size_t g_sceneVisibleCount = 0;
// 当前可见场景条目缓存（按当前模式过滤的快照）。放 PSRAM：每条 ~104B × 20 = ~2KB，
// 留 internal SRAM 大块给 NimBLE 配对。在 createSceneScreenLocked 时按需分配一次。
AppIdfScene::SceneItem* g_sceneVisibleItems = nullptr;

// 学习屏：跟随 AppIdfLearnFlow 状态切换文案。
lv_obj_t* g_learnScreen = nullptr;
lv_obj_t* g_learnTitle = nullptr;
lv_obj_t* g_learnStepLabel = nullptr;
lv_obj_t* g_learnProgressBar = nullptr;
lv_obj_t* g_learnMessage = nullptr;
lv_obj_t* g_learnArcLabel = nullptr;
lv_obj_t* g_learnHint = nullptr;
lv_timer_t* g_learnTimer = nullptr;
AppIdfLearnFlow::State g_learnLastSeenState = AppIdfLearnFlow::State::Idle;
bool g_learnLastFirstStage = false;
bool g_learnLastMismatch = false;

// 学习屏文案打字机：60ms 推进一个 UTF-8 字符；queued 用于 Capturing→AwaitingLabel 时
// 先播 "信号录制成功" 过渡再切到主文案。
char g_learnMsgTarget[160] = "";
size_t g_learnMsgCursorBytes = 0;
char g_learnMsgQueued[160] = "";
bool g_learnMsgHasQueued = false;
uint32_t g_learnMsgIntroDoneTick = 0;  // 0 = 当前 target 尚未播完
uint32_t g_learnMsgQueuedHoldMs = 0;
lv_timer_t* g_learnTypeTimer = nullptr;
lv_obj_t* g_otaScreen = nullptr;
lv_obj_t* g_otaTitleLabel = nullptr;
lv_obj_t* g_otaStatusLabel = nullptr;
lv_obj_t* g_otaBar = nullptr;
lv_obj_t* g_otaPercentLabel = nullptr;
lv_obj_t* g_otaDetailLabel = nullptr;
bool g_mainGroupReady = false;
bool g_started = false;
int g_mainFocusIndex = 0;
int g_aiStateIndex = 0;
int g_settingsFocusIndex = 0;
int g_settingsPopupSelectedIndex = 0;
int g_blePairSelectedIndex = 0;
int g_blePairWindowStart = 0;
int g_lastBlePairState = -1;
UiThemeMode g_currentTheme = UI_THEME_DARK;
SettingsPopupMode g_settingsPopupMode = SettingsPopupMode::None;
// ModeConfirm 翻页用：从 Mode→ModeConfirm 时记录用户选定的目标，从 ModeConfirm 取消返回时回填焦点。
AppIdfAppMode::Mode g_settingsPopupPendingMode = AppIdfAppMode::Mode::BLE;
int g_settingsPopupModeReturnIndex = 0;
uint32_t g_handledKeyCount = 0;
uint32_t g_blePairPopupUntilMs = 0;
uint32_t g_blePairAutoExitMs = 0;
uint32_t g_aiAutoExitMs = 0;
uint32_t g_otaFinishedUntilMs = 0;
bool g_aiRecordingActive = false;
bool g_aiUploadPending = false;
// EXECUTING 期间置位，让 refreshAiRecorderStateLocked 跳过 uploading→THINKING 的覆盖
bool g_aiExecutingActive = false;
bool g_otaWasActive = false;
bool g_settingsPopupVisible = false;
const char* g_activeScreenName = "unknown";

void showMainLocked(bool animated = true);
void showAiLocked(ai_state_t state);
void showSceneScreenLocked();
void refreshSceneScreenLocked();
void focusSceneIndexLocked(int index);
void showLearnScreenLocked();
void refreshLearnScreenLocked();
void hideLearnScreenLocked();
esp_err_t applyThemePreferenceLocked(UiThemeMode mode, bool persist, bool stayOnSettings);

constexpr int kMainItemCount = 4;

lv_obj_t* mainItemAt(int index) {
    switch (index) {
        case 0:
            return ui_ButtonAI;
        case 1:
            return ui_ButtonBluetooth;
        case 2:
            return ui_ButtonScene;
        case 3:
            return ui_ButtonSetting;
        default:
            return nullptr;
    }
}

bool isMainItemVisible(int index) {
    lv_obj_t* item = mainItemAt(index);
    return item != nullptr && !lv_obj_has_flag(item, LV_OBJ_FLAG_HIDDEN);
}

void applyMainModeVisibilityLocked() {
    const bool showBle = AppIdfAppMode::isBle();
    if (ui_ButtonBluetooth != nullptr) {
        if (showBle) {
            lv_obj_clear_flag(ui_ButtonBluetooth, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(ui_ButtonBluetooth, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (ui_LabelBleStatus != nullptr) {
        if (showBle) {
            lv_obj_clear_flag(ui_LabelBleStatus, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(ui_LabelBleStatus, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

ai_state_t aiStateAt(int index) {
    static constexpr ai_state_t kStates[] = {
        AI_STATE_ACK,
        AI_STATE_LISTENING,
        AI_STATE_THINKING,
        AI_STATE_EXECUTING,
        AI_STATE_SPEAKING,
        AI_STATE_SUCCESS,
        AI_STATE_ERROR,
    };
    constexpr int kStateCount = sizeof(kStates) / sizeof(kStates[0]);
    return kStates[index % kStateCount];
}

ai_state_t mapAiStatus(AiStatus status) {
    switch (status) {
        case AiStatus::Ack:
            return AI_STATE_ACK;
        case AiStatus::Listening:
            return AI_STATE_LISTENING;
        case AiStatus::Thinking:
            return AI_STATE_THINKING;
        case AiStatus::Executing:
            return AI_STATE_EXECUTING;
        case AiStatus::Speaking:
            return AI_STATE_SPEAKING;
        case AiStatus::Success:
            return AI_STATE_SUCCESS;
        case AiStatus::Error:
        default:
            return AI_STATE_ERROR;
    }
}

const char* aiStateLabel(ai_state_t state) {
    switch (state) {
        case AI_STATE_ACK:
            return "已唤醒";
        case AI_STATE_LISTENING:
            return "聆听中...";
        case AI_STATE_THINKING:
            return "思考中...";
        case AI_STATE_EXECUTING:
            return "执行中...";
        case AI_STATE_SPEAKING:
            return "播报中...";
        case AI_STATE_SUCCESS:
            return "已完成";
        case AI_STATE_ERROR:
            return "出现问题";
        case AI_STATE_IDLE:
        default:
            return "待命";
    }
}

const char* themeNameCn(UiThemeMode mode) {
    return mode == UI_THEME_LIGHT ? "亮色" : "暗色";
}

const char* themeNameAscii(UiThemeMode mode) {
    return mode == UI_THEME_LIGHT ? "light" : "dark";
}

const char* networkModeNameCn(AppIdfTransport::NetMode mode) {
    switch (mode) {
        case AppIdfTransport::NetMode::WIFI_ONLY:
            return "WiFi";
        case AppIdfTransport::NetMode::CELLULAR_ONLY:
            return "4G";
        case AppIdfTransport::NetMode::AUTO:
        default:
            return "自动";
    }
}

void loadThemePreference() {
    uint8_t storedTheme = UI_THEME_DARK;
    nvs_handle_t handle = 0;
    const esp_err_t err = nvs_open(kUiPrefsNamespace, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        const esp_err_t getErr = nvs_get_u8(handle, kUiPrefsThemeKey, &storedTheme);
        if (getErr != ESP_OK && getErr != ESP_ERR_NVS_NOT_FOUND) {
            LOG_W(TAG_UI_IDF, "Failed to read UI theme preference: %s", esp_err_to_name(getErr));
        }
        nvs_close(handle);
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        LOG_W(TAG_UI_IDF, "Failed to open UI preferences: %s", esp_err_to_name(err));
    }

    g_currentTheme = storedTheme == UI_THEME_LIGHT ? UI_THEME_LIGHT : UI_THEME_DARK;
}

esp_err_t saveThemePreference(UiThemeMode mode) {
    ScopedFlashGuard flashGuard("IDF UI theme save", 5000);
    if (!flashGuard.ok()) {
        return ESP_ERR_TIMEOUT;
    }

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(kUiPrefsNamespace, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        LOG_W(TAG_UI_IDF, "Failed to open UI preferences for writing: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_u8(handle, kUiPrefsThemeKey, mode == UI_THEME_LIGHT ? UI_THEME_LIGHT : UI_THEME_DARK);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err != ESP_OK) {
        LOG_W(TAG_UI_IDF, "Failed to save UI theme preference: %s", esp_err_to_name(err));
    }
    return err;
}

void ensureMainGroup() {
    if (g_mainGroup == nullptr) {
        g_mainGroup = lv_group_create();
    }
    if (g_mainGroupReady) {
        return;
    }

    for (int i = 0; i < kMainItemCount; ++i) {
        lv_obj_t* item = mainItemAt(i);
        if (item != nullptr) {
            lv_group_add_obj(g_mainGroup, item);
        }
    }
    g_mainGroupReady = true;
}

void resetMainGroup() {
    if (g_mainGroup != nullptr) {
        lv_group_del(g_mainGroup);
        g_mainGroup = nullptr;
    }
    g_mainGroupReady = false;
}

void focusMainIndex(int index) {
    if (index < 0) {
        index = 0;
    }
    index = index % kMainItemCount;
    for (int tries = 0; tries < kMainItemCount; ++tries) {
        if (isMainItemVisible(index)) {
            break;
        }
        index = (index + 1) % kMainItemCount;
    }
    g_mainFocusIndex = index;

    ensureMainGroup();
    for (int i = 0; i < kMainItemCount; ++i) {
        lv_obj_t* item = mainItemAt(i);
        if (item != nullptr) {
            lv_obj_clear_state(item, LV_STATE_FOCUSED);
        }
    }

    lv_obj_t* focused = mainItemAt(g_mainFocusIndex);
    if (focused != nullptr) {
        lv_group_focus_obj(focused);
        lv_obj_add_state(focused, LV_STATE_FOCUSED);
        lv_obj_scroll_to_view(focused, LV_ANIM_ON);
    }
}

lv_obj_t* settingsItemAt(int index) {
    if (index < 0 || index >= kSettingsItemCount) {
        return nullptr;
    }
    return g_settingsItems[index];
}

void focusSettingsIndexLocked(int index) {
    if (index < 0) {
        index = 0;
    }
    g_settingsFocusIndex = index % kSettingsItemCount;

    for (int i = 0; i < kSettingsItemCount; ++i) {
        lv_obj_t* item = settingsItemAt(i);
        if (item != nullptr) {
            lv_obj_clear_state(item, LV_STATE_FOCUSED);
        }
    }

    lv_obj_t* focused = settingsItemAt(g_settingsFocusIndex);
    if (focused != nullptr) {
        lv_obj_add_state(focused, LV_STATE_FOCUSED);
        lv_obj_scroll_to_view(focused, LV_ANIM_ON);
    }
}

int batteryPercentToSegments(int batteryPercent, int segmentCount) {
    if (batteryPercent <= 0 || segmentCount <= 0) {
        return 0;
    }

    int litSegments = (batteryPercent + 24) / 25;
    if (litSegments > segmentCount) {
        litSegments = segmentCount;
    }
    return litSegments;
}

lv_color_t batteryColorForPercent(int batteryPercent) {
    if (batteryPercent <= 25) {
        return ui_theme_color_error();
    }
    if (batteryPercent <= 50) {
        return ui_theme_is_light() ? lv_color_hex(0xF59E0B) : lv_color_hex(0xFBBF24);
    }
    return ui_theme_color_success();
}

void updateSettingsSummaryLocked() {
    if (g_settingsValueLabels[static_cast<int>(SettingsItem::Volume)] != nullptr) {
        char volumeBuf[16];
        snprintf(volumeBuf, sizeof(volumeBuf), "%u%%", static_cast<unsigned>(AppIdfAudio::getVolume()));
        lv_label_set_text(g_settingsValueLabels[static_cast<int>(SettingsItem::Volume)], volumeBuf);
    }

    if (g_settingsValueLabels[static_cast<int>(SettingsItem::Network)] != nullptr) {
        const AppIdfTransport::Snapshot transport = AppIdfTransport::snapshot();
        lv_label_set_text(g_settingsValueLabels[static_cast<int>(SettingsItem::Network)],
                          networkModeNameCn(transport.mode));
    }

    if (g_settingsValueLabels[static_cast<int>(SettingsItem::WifiReset)] != nullptr) {
        // 仅显示凭据状态;配网热点 SSID 已经由主屏药丸展示,这里再展示一遍既会与"重置WiFi"
        // 文字重叠,STA 连上后也无法及时切回"已保存"。
        const char* text = AppIdfNetwork::hasStoredCredentials() ? "已保存" : "未配置";
        lv_label_set_text(g_settingsValueLabels[static_cast<int>(SettingsItem::WifiReset)], text);
    }

    if (g_settingsValueLabels[static_cast<int>(SettingsItem::Theme)] != nullptr) {
        lv_label_set_text(g_settingsValueLabels[static_cast<int>(SettingsItem::Theme)], themeNameCn(g_currentTheme));
    }

    if (g_settingsValueLabels[static_cast<int>(SettingsItem::Mode)] != nullptr) {
        lv_label_set_text(g_settingsValueLabels[static_cast<int>(SettingsItem::Mode)],
                          AppIdfAppMode::nameCn(AppIdfAppMode::current()));
    }

    if (g_settingsValueLabels[static_cast<int>(SettingsItem::Firmware)] != nullptr) {
        const esp_app_desc_t* desc = esp_app_get_description();
        const char* version = (desc != nullptr && desc->version[0] != '\0') ? desc->version : "IDF";
        lv_label_set_text(g_settingsValueLabels[static_cast<int>(SettingsItem::Firmware)], version);
    }
}

lv_obj_t* createSettingItemLocked(lv_obj_t* parent,
                                  const void* icon,
                                  const char* text,
                                  const char* value,
                                  lv_obj_t** valueLabelOut) {
    lv_obj_t* item = lv_obj_create(parent);
    lv_obj_remove_style_all(item);
    lv_obj_set_size(item, 200, 36);
    lv_obj_add_style(item, &style_glass_list, 0);
    lv_obj_add_style(item, &style_glass_list_focused, LV_STATE_FOCUSED);
    lv_obj_clear_flag(item, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* left = lv_obj_create(item);
    lv_obj_remove_style_all(left);
    lv_obj_set_size(left, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(left, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_layout(left, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(left, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(left, 8, 0);
    lv_obj_clear_flag(left, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    if (icon != nullptr) {
        lv_obj_t* img = lv_img_create(left);
        lv_img_set_src(img, icon);
        // 统一缩放为 20px 视觉尺寸，避免不同源图标(如 40x40 的 bind)把行内 label 挤偏。
        // SIZE_MODE_REAL 让 self_size 跟随 zoom 后的真实尺寸，否则 flex 仍按原图占位。
        const lv_img_dsc_t* iconDsc = static_cast<const lv_img_dsc_t*>(icon);
        const uint16_t srcMax = iconDsc->header.w > iconDsc->header.h ? iconDsc->header.w : iconDsc->header.h;
        if (srcMax > 20) {
            lv_img_set_size_mode(img, LV_IMG_SIZE_MODE_REAL);
            lv_img_set_zoom(img, static_cast<uint16_t>((20 * 256) / srcMax));
        }
        lv_obj_set_style_img_recolor(img, ui_theme_is_light() ? ui_theme_color_text_muted() : lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_img_recolor_opa(img, ui_theme_is_light() ? LV_OPA_COVER : 0, 0);
        lv_obj_set_style_img_recolor(img, ui_theme_color_focus(), LV_STATE_FOCUSED);
        lv_obj_set_style_img_recolor_opa(img, ui_theme_is_light() ? LV_OPA_COVER : 80, LV_STATE_FOCUSED);
        lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
    }

    lv_obj_t* label = lv_label_create(left);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, ui_theme_color_text(), 0);
    lv_obj_set_style_text_opa(label, ui_theme_is_light() ? LV_OPA_COVER : 216, 0);
    lv_obj_set_style_text_font(label, &my_font_misans_20, 0);

    lv_obj_t* valueLabel = lv_label_create(item);
    lv_label_set_text(valueLabel, value != nullptr ? value : "");
    lv_obj_set_style_text_color(valueLabel, ui_theme_color_text_muted(), 0);
    lv_obj_set_style_text_opa(valueLabel, ui_theme_is_light() ? 220 : 120, 0);
    lv_obj_set_style_text_font(valueLabel, &my_font_misans_20, 0);
    lv_obj_align(valueLabel, LV_ALIGN_RIGHT_MID, -4, 0);
    if (valueLabelOut != nullptr) {
        *valueLabelOut = valueLabel;
    }

    return item;
}

void createSceneScreenLocked() {
    if (g_sceneScreen != nullptr) {
        return;
    }

    if (g_sceneVisibleItems == nullptr) {
        g_sceneVisibleItems = static_cast<AppIdfScene::SceneItem*>(
            heap_caps_calloc(kMaxSceneRows, sizeof(AppIdfScene::SceneItem),
                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (g_sceneVisibleItems == nullptr) {
            g_sceneVisibleItems = static_cast<AppIdfScene::SceneItem*>(
                heap_caps_calloc(kMaxSceneRows, sizeof(AppIdfScene::SceneItem),
                                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        }
    }

    g_sceneScreen = lv_obj_create(nullptr);
    lv_obj_clear_flag(g_sceneScreen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(g_sceneScreen, ui_theme_is_light() ? ui_theme_color_bg() : lv_color_hex(0x0D1117),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_sceneScreen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(g_sceneScreen,
                                   ui_theme_is_light() ? ui_theme_color_bg_grad() : lv_color_hex(0x161B22),
                                   LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(g_sceneScreen, LV_GRAD_DIR_VER, LV_PART_MAIN);

    g_sceneTitle = lv_label_create(g_sceneScreen);
    lv_label_set_text(g_sceneTitle, "场景");
    lv_obj_set_style_text_color(g_sceneTitle, ui_theme_color_text(), 0);
    lv_obj_set_style_text_opa(g_sceneTitle, ui_theme_is_light() ? LV_OPA_COVER : 230, 0);
    lv_obj_set_style_text_font(g_sceneTitle, &my_font_misans_20, 0);
    lv_obj_align(g_sceneTitle, LV_ALIGN_TOP_MID, 0, 22);

    lv_obj_t* list = lv_obj_create(g_sceneScreen);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, 220, 158);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 50);
    lv_obj_set_layout(list, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(list, 4, 0);
    lv_obj_clear_flag(list, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);

    g_sceneEmptyLabel = lv_label_create(list);
    lv_label_set_text(g_sceneEmptyLabel, "暂无场景");
    lv_obj_set_style_text_align(g_sceneEmptyLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(g_sceneEmptyLabel, ui_theme_color_text_muted(), 0);
    lv_obj_set_style_text_opa(g_sceneEmptyLabel, ui_theme_is_light() ? 200 : 140, 0);
    lv_obj_set_style_text_font(g_sceneEmptyLabel, &my_font_misans_20, 0);

    for (int i = 0; i < kMaxSceneRows; ++i) {
        g_sceneRows[i] = lv_obj_create(list);
        lv_obj_remove_style_all(g_sceneRows[i]);
        lv_obj_set_size(g_sceneRows[i], 210, 26);
        lv_obj_set_style_bg_color(g_sceneRows[i],
                                  ui_theme_is_light() ? ui_theme_color_card() : lv_color_hex(0x1F2937), 0);
        lv_obj_set_style_bg_opa(g_sceneRows[i], ui_theme_is_light() ? LV_OPA_COVER : 175, 0);
        lv_obj_set_style_bg_color(g_sceneRows[i], ui_theme_color_focus_bg(), LV_STATE_FOCUSED);
        lv_obj_set_style_bg_opa(g_sceneRows[i], ui_theme_is_light() ? LV_OPA_COVER : 220, LV_STATE_FOCUSED);
        lv_obj_set_style_radius(g_sceneRows[i], 8, 0);
        lv_obj_clear_flag(g_sceneRows[i], LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(g_sceneRows[i], LV_OBJ_FLAG_HIDDEN);

        g_sceneRowLabels[i] = lv_label_create(g_sceneRows[i]);
        lv_obj_set_width(g_sceneRowLabels[i], 198);
        lv_obj_set_style_text_color(g_sceneRowLabels[i], ui_theme_color_text(), 0);
        lv_obj_set_style_text_opa(g_sceneRowLabels[i], ui_theme_is_light() ? LV_OPA_COVER : 210, 0);
        lv_obj_set_style_text_font(g_sceneRowLabels[i], &my_font_misans_20, 0);
        lv_label_set_long_mode(g_sceneRowLabels[i], LV_LABEL_LONG_DOT);
        lv_obj_align(g_sceneRowLabels[i], LV_ALIGN_LEFT_MID, 6, 0);
    }
}

void focusSceneIndexLocked(int index) {
    if (g_sceneVisibleCount == 0) {
        g_sceneFocusIndex = 0;
        return;
    }
    if (index < 0) index = 0;
    g_sceneFocusIndex = index % static_cast<int>(g_sceneVisibleCount);
    for (int i = 0; i < kMaxSceneRows; ++i) {
        if (g_sceneRows[i] == nullptr) continue;
        if (i == g_sceneFocusIndex) {
            lv_obj_add_state(g_sceneRows[i], LV_STATE_FOCUSED);
            lv_obj_scroll_to_view(g_sceneRows[i], LV_ANIM_ON);
        } else {
            lv_obj_clear_state(g_sceneRows[i], LV_STATE_FOCUSED);
        }
    }
}

void refreshSceneScreenLocked() {
    if (g_sceneScreen == nullptr) {
        createSceneScreenLocked();
    }

    // 标题：场景 · <当前模式中文名>
    if (g_sceneTitle != nullptr) {
        char titleBuf[40];
        snprintf(titleBuf, sizeof(titleBuf), "场景 · %s",
                 AppIdfAppMode::nameCn(AppIdfAppMode::current()));
        lv_label_set_text(g_sceneTitle, titleBuf);
    }

    // 数据源：按当前模式过滤的快照（BLE = ROM 预制，IR/RF433 = 同 type 持久条目）。
    const size_t total = AppIdfScene::listForCurrentMode(g_sceneVisibleItems, kMaxSceneRows);
    g_sceneVisibleCount = total;

    if (total == 0) {
        if (g_sceneEmptyLabel != nullptr) {
            lv_obj_clear_flag(g_sceneEmptyLabel, LV_OBJ_FLAG_HIDDEN);
        }
        for (int i = 0; i < kMaxSceneRows; ++i) {
            if (g_sceneRows[i] != nullptr) {
                lv_obj_add_flag(g_sceneRows[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
        g_sceneFocusIndex = 0;
        return;
    }

    if (g_sceneEmptyLabel != nullptr) {
        lv_obj_add_flag(g_sceneEmptyLabel, LV_OBJ_FLAG_HIDDEN);
    }
    for (size_t i = 0; i < total; ++i) {
        const AppIdfScene::SceneItem& item = g_sceneVisibleItems[i];
        char buf[80];
        snprintf(buf, sizeof(buf), "%s", item.label);
        if (g_sceneRowLabels[i] != nullptr) {
            lv_label_set_text(g_sceneRowLabels[i], buf);
        }
        if (g_sceneRows[i] != nullptr) {
            lv_obj_clear_flag(g_sceneRows[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    for (size_t i = total; i < kMaxSceneRows; ++i) {
        if (g_sceneRows[i] != nullptr) {
            lv_obj_add_flag(g_sceneRows[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (g_sceneFocusIndex < 0 || g_sceneFocusIndex >= static_cast<int>(total)) {
        g_sceneFocusIndex = 0;
    }
    focusSceneIndexLocked(g_sceneFocusIndex);
}

void showSceneScreenLocked() {
    if (g_sceneScreen == nullptr) {
        createSceneScreenLocked();
    }
    refreshSceneScreenLocked();
    lv_scr_load_anim(g_sceneScreen, LV_SCR_LOAD_ANIM_FADE_IN, 200, 0, false);
    g_activeScreenName = "scene";
}

const char* learnTitleFor(AppIdfLearnFlow::State s) {
    switch (s) {
        case AppIdfLearnFlow::State::Capturing:      return "场景学习";
        case AppIdfLearnFlow::State::AwaitingLabel:  return "命名信号";
        case AppIdfLearnFlow::State::UploadingLabel: return "保存中";
        default:                                      return "场景学习";
    }
}

const char* learnStepLabelFor(AppIdfLearnFlow::State s, AppIdfLearnFlow::CaptureKind kind,
                              bool firstStageCaptured) {
    // RF433 只需按一次：3 个里程碑等距推进；IR 需两次相同按键，第一次 1/3、第二次 2/3 同标签。
    if (kind == AppIdfLearnFlow::CaptureKind::RF433) {
        switch (s) {
            case AppIdfLearnFlow::State::Capturing:      return "1 / 3";
            case AppIdfLearnFlow::State::AwaitingLabel:  return "2 / 3";
            case AppIdfLearnFlow::State::UploadingLabel: return "3 / 3";
            default:                                      return "";
        }
    }
    switch (s) {
        case AppIdfLearnFlow::State::Capturing:
            return firstStageCaptured ? "2 / 3" : "1 / 3";
        case AppIdfLearnFlow::State::AwaitingLabel:
            return "2 / 3";
        case AppIdfLearnFlow::State::UploadingLabel:
            return "3 / 3";
        default:
            return "";
    }
}

int learnProgressFor(AppIdfLearnFlow::State s, AppIdfLearnFlow::CaptureKind kind,
                     bool firstStageCaptured) {
    if (kind == AppIdfLearnFlow::CaptureKind::RF433) {
        switch (s) {
            case AppIdfLearnFlow::State::Capturing:      return 33;
            case AppIdfLearnFlow::State::AwaitingLabel:  return 66;
            case AppIdfLearnFlow::State::UploadingLabel: return 100;
            default:                                      return 0;
        }
    }
    switch (s) {
        case AppIdfLearnFlow::State::Capturing:      return firstStageCaptured ? 50 : 25;
        case AppIdfLearnFlow::State::AwaitingLabel:  return 75;
        case AppIdfLearnFlow::State::UploadingLabel: return 100;
        default:                                      return 0;
    }
}

const char* learnMessageFor(AppIdfLearnFlow::State s, bool firstStageCaptured, bool mismatch) {
    switch (s) {
        case AppIdfLearnFlow::State::Capturing:
            if (firstStageCaptured) return "请再按一次\n相同的按键";
            // mismatch 与 firstStageCaptured 互斥：mismatch 时底层已回退到等第一次。
            if (mismatch)           return "信号不一致\n请输入相同的信号";
            return "请按一下\n要学习的遥控器按键";
        case AppIdfLearnFlow::State::AwaitingLabel:
            return "长按 KEY2 说出功能\n说完后松开";
        case AppIdfLearnFlow::State::UploadingLabel:
            return "正在保存...";
        default:
            return "";
    }
}

const char* learnHintFor(AppIdfLearnFlow::State s) {
    switch (s) {
        case AppIdfLearnFlow::State::UploadingLabel:
            return "请稍候...";
        default:
            return "";
    }
}

size_t utf8CharLen(uint8_t b) {
    if ((b & 0x80) == 0x00) return 1;
    if ((b & 0xE0) == 0xC0) return 2;
    if ((b & 0xF0) == 0xE0) return 3;
    if ((b & 0xF8) == 0xF0) return 4;
    return 1;
}

void learnTypewriterTick() {
    if (g_learnMessage == nullptr) return;
    const size_t total = strlen(g_learnMsgTarget);
    if (g_learnMsgCursorBytes < total) {
        const uint8_t b = static_cast<uint8_t>(g_learnMsgTarget[g_learnMsgCursorBytes]);
        size_t step = utf8CharLen(b);
        if (g_learnMsgCursorBytes + step > total) step = total - g_learnMsgCursorBytes;
        g_learnMsgCursorBytes += step;
        char buf[sizeof(g_learnMsgTarget)];
        memcpy(buf, g_learnMsgTarget, g_learnMsgCursorBytes);
        buf[g_learnMsgCursorBytes] = '\0';
        lv_label_set_text(g_learnMessage, buf);
        if (g_learnMsgCursorBytes >= total) {
            g_learnMsgIntroDoneTick = lv_tick_get();
        }
    } else if (g_learnMsgHasQueued) {
        if (g_learnMsgIntroDoneTick == 0) g_learnMsgIntroDoneTick = lv_tick_get();
        if (lv_tick_elaps(g_learnMsgIntroDoneTick) >= g_learnMsgQueuedHoldMs) {
            strncpy(g_learnMsgTarget, g_learnMsgQueued, sizeof(g_learnMsgTarget) - 1);
            g_learnMsgTarget[sizeof(g_learnMsgTarget) - 1] = '\0';
            g_learnMsgQueued[0] = '\0';
            g_learnMsgHasQueued = false;
            g_learnMsgCursorBytes = 0;
            g_learnMsgIntroDoneTick = 0;
            g_learnMsgQueuedHoldMs = 0;
            lv_label_set_text(g_learnMessage, "");
        }
    }
}

void learnTypeTimerCb(lv_timer_t*) {
    learnTypewriterTick();
}

void setLearnMessageImmediate(const char* text) {
    if (text == nullptr) text = "";
    if (!g_learnMsgHasQueued && strcmp(g_learnMsgTarget, text) == 0 &&
        g_learnMsgCursorBytes >= strlen(g_learnMsgTarget)) {
        return;
    }
    strncpy(g_learnMsgTarget, text, sizeof(g_learnMsgTarget) - 1);
    g_learnMsgTarget[sizeof(g_learnMsgTarget) - 1] = '\0';
    g_learnMsgQueued[0] = '\0';
    g_learnMsgHasQueued = false;
    g_learnMsgCursorBytes = 0;
    g_learnMsgIntroDoneTick = 0;
    g_learnMsgQueuedHoldMs = 0;
    if (g_learnMessage != nullptr) lv_label_set_text(g_learnMessage, "");
}

void setLearnMessageWithIntro(const char* intro, const char* mainText, uint32_t holdMs) {
    if (intro == nullptr) intro = "";
    if (mainText == nullptr) mainText = "";
    strncpy(g_learnMsgTarget, intro, sizeof(g_learnMsgTarget) - 1);
    g_learnMsgTarget[sizeof(g_learnMsgTarget) - 1] = '\0';
    strncpy(g_learnMsgQueued, mainText, sizeof(g_learnMsgQueued) - 1);
    g_learnMsgQueued[sizeof(g_learnMsgQueued) - 1] = '\0';
    g_learnMsgHasQueued = true;
    g_learnMsgCursorBytes = 0;
    g_learnMsgIntroDoneTick = 0;
    g_learnMsgQueuedHoldMs = holdMs;
    if (g_learnMessage != nullptr) lv_label_set_text(g_learnMessage, "");
}

void learnTimerCb(lv_timer_t*) {
    refreshLearnScreenLocked();
}

void createLearnScreenLocked() {
    if (g_learnScreen != nullptr) return;

    g_learnScreen = lv_obj_create(nullptr);
    lv_obj_clear_flag(g_learnScreen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(g_learnScreen, ui_theme_is_light() ? ui_theme_color_bg() : lv_color_hex(0x0D1117),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_learnScreen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(g_learnScreen,
                                   ui_theme_is_light() ? ui_theme_color_bg_grad() : lv_color_hex(0x161B22),
                                   LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(g_learnScreen, LV_GRAD_DIR_VER, LV_PART_MAIN);

    g_learnTitle = lv_label_create(g_learnScreen);
    lv_label_set_text(g_learnTitle, "场景学习");
    lv_obj_set_style_text_color(g_learnTitle, ui_theme_color_text(), 0);
    lv_obj_set_style_text_opa(g_learnTitle, ui_theme_is_light() ? LV_OPA_COVER : 230, 0);
    lv_obj_set_style_text_font(g_learnTitle, &my_font_misans_20, 0);
    lv_obj_align(g_learnTitle, LV_ALIGN_TOP_MID, 0, 22);

    g_learnStepLabel = lv_label_create(g_learnScreen);
    lv_label_set_text(g_learnStepLabel, "");
    lv_obj_set_style_text_color(g_learnStepLabel, ui_theme_color_focus(), 0);
    lv_obj_set_style_text_opa(g_learnStepLabel, ui_theme_is_light() ? LV_OPA_COVER : 220, 0);
    lv_obj_set_style_text_font(g_learnStepLabel, &lv_font_montserrat_14, 0);
    lv_obj_align(g_learnStepLabel, LV_ALIGN_TOP_MID, 0, 50);

    g_learnProgressBar = lv_bar_create(g_learnScreen);
    lv_obj_set_size(g_learnProgressBar, 160, 6);
    lv_obj_align(g_learnProgressBar, LV_ALIGN_TOP_MID, 0, 72);
    lv_bar_set_range(g_learnProgressBar, 0, 100);
    lv_bar_set_value(g_learnProgressBar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(g_learnProgressBar,
                              ui_theme_is_light() ? lv_color_hex(0xD0D7DE) : lv_color_hex(0x21262D),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_learnProgressBar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(g_learnProgressBar, 3, LV_PART_MAIN);
    lv_obj_set_style_bg_color(g_learnProgressBar, ui_theme_color_focus(), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(g_learnProgressBar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(g_learnProgressBar, 3, LV_PART_INDICATOR);

    g_learnMessage = lv_label_create(g_learnScreen);
    lv_label_set_text(g_learnMessage, "");
    lv_label_set_long_mode(g_learnMessage, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(g_learnMessage, 220);
    lv_obj_set_style_text_align(g_learnMessage, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(g_learnMessage, ui_theme_color_text(), 0);
    lv_obj_set_style_text_opa(g_learnMessage, ui_theme_is_light() ? LV_OPA_COVER : 220, 0);
    lv_obj_set_style_text_font(g_learnMessage, &my_font_misans_20, 0);
    lv_obj_align(g_learnMessage, LV_ALIGN_CENTER, 0, -10);

    g_learnArcLabel = lv_label_create(g_learnScreen);
    lv_label_set_text(g_learnArcLabel, "");
    lv_obj_set_style_text_color(g_learnArcLabel, ui_theme_color_text_muted(), 0);
    lv_obj_set_style_text_opa(g_learnArcLabel, ui_theme_is_light() ? 200 : 150, 0);
    lv_obj_set_style_text_font(g_learnArcLabel, &my_font_misans_20, 0);
    lv_obj_align(g_learnArcLabel, LV_ALIGN_CENTER, 0, 50);

    g_learnHint = lv_label_create(g_learnScreen);
    lv_label_set_text(g_learnHint, "");
    lv_obj_set_style_text_color(g_learnHint, ui_theme_color_text_muted(), 0);
    lv_obj_set_style_text_opa(g_learnHint, ui_theme_is_light() ? 200 : 130, 0);
    lv_obj_set_style_text_font(g_learnHint, &my_font_misans_20, 0);
    lv_obj_align(g_learnHint, LV_ALIGN_BOTTOM_MID, 0, -10);
}

void refreshLearnScreenLocked() {
    if (g_learnScreen == nullptr) return;

    const auto state = AppIdfLearnFlow::currentState();
    const auto kind = AppIdfLearnFlow::captureKind();
    const bool firstStage = AppIdfLearnFlow::irFirstStageCaptured();
    const bool mismatch = AppIdfLearnFlow::irMismatchPending();
    if (state != g_learnLastSeenState || firstStage != g_learnLastFirstStage ||
        mismatch != g_learnLastMismatch) {
        const auto prevState = g_learnLastSeenState;
        g_learnLastSeenState = state;
        g_learnLastFirstStage = firstStage;
        g_learnLastMismatch = mismatch;
        if (g_learnTitle != nullptr)     lv_label_set_text(g_learnTitle, learnTitleFor(state));
        if (g_learnStepLabel != nullptr) lv_label_set_text(g_learnStepLabel, learnStepLabelFor(state, kind, firstStage));
        if (g_learnMessage != nullptr) {
            const bool capturedToAwaiting =
                (prevState == AppIdfLearnFlow::State::Capturing) &&
                (state == AppIdfLearnFlow::State::AwaitingLabel);
            if (capturedToAwaiting) {
                setLearnMessageWithIntro("信号录制成功",
                                         learnMessageFor(state, firstStage, mismatch), 1000);
            } else {
                setLearnMessageImmediate(learnMessageFor(state, firstStage, mismatch));
            }
        }
        if (g_learnHint != nullptr)      lv_label_set_text(g_learnHint, learnHintFor(state));
        if (g_learnProgressBar != nullptr) {
            lv_bar_set_value(g_learnProgressBar, learnProgressFor(state, kind, firstStage), LV_ANIM_ON);
        }
    }

    // LearnFlow 自己回到 Idle（成功/失败/超时）→ 退回场景屏。
    if (state == AppIdfLearnFlow::State::Idle && lv_scr_act() == g_learnScreen) {
        showSceneScreenLocked();
        return;
    }

    if (g_learnArcLabel != nullptr) {
        const uint32_t remaining = AppIdfLearnFlow::remainingMs();
        if (remaining > 0) {
            char buf[32];
            snprintf(buf, sizeof(buf), "还剩 %us", static_cast<unsigned>((remaining + 999) / 1000));
            lv_label_set_text(g_learnArcLabel, buf);
        } else {
            lv_label_set_text(g_learnArcLabel, "");
        }
    }
}

void showLearnScreenLocked() {
    if (g_learnScreen == nullptr) {
        createLearnScreenLocked();
    }
    g_learnLastSeenState = static_cast<AppIdfLearnFlow::State>(0xFF);  // 强制下一次 refresh 全量刷新
    g_learnLastFirstStage = !AppIdfLearnFlow::irFirstStageCaptured();   // 强制刷新进度/文案
    g_learnLastMismatch = !AppIdfLearnFlow::irMismatchPending();
    refreshLearnScreenLocked();
    lv_scr_load_anim(g_learnScreen, LV_SCR_LOAD_ANIM_FADE_IN, 200, 0, false);
    g_activeScreenName = "learn";
    if (g_learnTimer == nullptr) {
        g_learnTimer = lv_timer_create(learnTimerCb, 200, nullptr);
    }
    if (g_learnTypeTimer == nullptr) {
        g_learnTypeTimer = lv_timer_create(learnTypeTimerCb, 60, nullptr);
    }
}

void hideLearnScreenLocked() {
    if (g_learnTimer != nullptr) {
        lv_timer_del(g_learnTimer);
        g_learnTimer = nullptr;
    }
    if (g_learnTypeTimer != nullptr) {
        lv_timer_del(g_learnTypeTimer);
        g_learnTypeTimer = nullptr;
    }
    g_learnMsgTarget[0] = '\0';
    g_learnMsgQueued[0] = '\0';
    g_learnMsgHasQueued = false;
    g_learnMsgCursorBytes = 0;
    g_learnMsgIntroDoneTick = 0;
    g_learnMsgQueuedHoldMs = 0;
}

bool executeFocusedSceneLocked() {
    if (g_sceneVisibleCount == 0) return false;
    if (g_sceneFocusIndex < 0 || g_sceneFocusIndex >= static_cast<int>(g_sceneVisibleCount)) {
        return false;
    }
    const AppIdfScene::SceneItem& item = g_sceneVisibleItems[g_sceneFocusIndex];
    const esp_err_t err = AppIdfScene::executeById(item.id);
    LOG_I(TAG_UI_IDF, "scene[%u] execute: %s", item.id, err == ESP_OK ? "ok" : esp_err_to_name(err));
    return err == ESP_OK;
}

bool removeFocusedSceneLocked() {
    if (g_sceneVisibleCount == 0) return false;
    if (g_sceneFocusIndex < 0 || g_sceneFocusIndex >= static_cast<int>(g_sceneVisibleCount)) {
        return false;
    }
    const uint8_t id = g_sceneVisibleItems[g_sceneFocusIndex].id;
    const esp_err_t err = AppIdfScene::removeById(id);
    LOG_I(TAG_UI_IDF, "scene[%u] delete: %s", id, err == ESP_OK ? "ok" : esp_err_to_name(err));
    if (err != ESP_OK) {
        AppIdfAudio::playLocalCue("op_failed", 4000);
        return false;
    }
    AppIdfAudio::playLocalCue("done", 4000);
    refreshSceneScreenLocked();
    return true;
}

void createSettingsScreenLocked() {
    if (g_settingsScreen != nullptr) {
        updateSettingsSummaryLocked();
        return;
    }

    g_settingsScreen = lv_obj_create(nullptr);
    lv_obj_clear_flag(g_settingsScreen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(g_settingsScreen, ui_theme_is_light() ? ui_theme_color_bg() : lv_color_hex(0x0D1117),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_settingsScreen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(g_settingsScreen,
                                   ui_theme_is_light() ? ui_theme_color_bg_grad() : lv_color_hex(0x161B22),
                                   LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(g_settingsScreen, LV_GRAD_DIR_VER, LV_PART_MAIN);

    lv_obj_t* title = lv_label_create(g_settingsScreen);
    lv_label_set_text(title, "设置");
    lv_obj_set_style_text_color(title, ui_theme_color_text(), 0);
    lv_obj_set_style_text_opa(title, ui_theme_is_light() ? LV_OPA_COVER : 230, 0);
    lv_obj_set_style_text_font(title, &my_font_misans_20, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 22);

    lv_obj_t* list = lv_obj_create(g_settingsScreen);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, 204, 166);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 58);
    lv_obj_set_layout(list, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(list, 6, 0);
    // 5 项放不下 166px,允许垂直滚动;聚焦切换时由 lv_obj_scroll_to_view 自动滚动
    lv_obj_clear_flag(list, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);

    g_settingsItems[static_cast<int>(SettingsItem::Volume)] =
        createSettingItemLocked(list, &ui_img_icon_volume, "音量", "--",
                                &g_settingsValueLabels[static_cast<int>(SettingsItem::Volume)]);
    g_settingsItems[static_cast<int>(SettingsItem::Network)] =
        createSettingItemLocked(list, &ui_img_icon_network, "网络", "自动",
                                &g_settingsValueLabels[static_cast<int>(SettingsItem::Network)]);
    g_settingsItems[static_cast<int>(SettingsItem::WifiReset)] =
        createSettingItemLocked(list, &ui_img_icon_network, "重置WiFi", "—",
                                &g_settingsValueLabels[static_cast<int>(SettingsItem::WifiReset)]);
    g_settingsItems[static_cast<int>(SettingsItem::Theme)] =
        createSettingItemLocked(list, &ui_img_icon_lang, "主题", themeNameCn(g_currentTheme),
                                &g_settingsValueLabels[static_cast<int>(SettingsItem::Theme)]);
    g_settingsItems[static_cast<int>(SettingsItem::Mode)] =
        createSettingItemLocked(list, &ui_img_icon_bind, "模式", AppIdfAppMode::nameCn(AppIdfAppMode::current()),
                                &g_settingsValueLabels[static_cast<int>(SettingsItem::Mode)]);
    g_settingsItems[static_cast<int>(SettingsItem::Firmware)] =
        createSettingItemLocked(list, &ui_img_icon_info, "固件", "IDF",
                                &g_settingsValueLabels[static_cast<int>(SettingsItem::Firmware)]);

    updateSettingsSummaryLocked();
    focusSettingsIndexLocked(g_settingsFocusIndex);
}

void createSettingsPopupLocked() {
    if (g_settingsScreen == nullptr || g_settingsPopupMask != nullptr) {
        return;
    }

    g_settingsPopupMask = lv_obj_create(g_settingsScreen);
    lv_obj_remove_style_all(g_settingsPopupMask);
    lv_obj_set_size(g_settingsPopupMask, 240, 240);
    lv_obj_align(g_settingsPopupMask, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(g_settingsPopupMask, ui_theme_color_overlay(), 0);
    lv_obj_set_style_bg_opa(g_settingsPopupMask, ui_theme_is_light() ? 150 : 130, 0);
    lv_obj_add_flag(g_settingsPopupMask, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(g_settingsPopupMask, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    g_settingsPopupCard = lv_obj_create(g_settingsPopupMask);
    lv_obj_set_size(g_settingsPopupCard, 172, 134);
    lv_obj_center(g_settingsPopupCard);
    lv_obj_add_style(g_settingsPopupCard, &style_card, 0);
    lv_obj_set_style_bg_color(g_settingsPopupCard, ui_theme_is_light() ? ui_theme_color_card() : lv_color_hex(0x111827),
                              0);
    lv_obj_set_style_bg_opa(g_settingsPopupCard, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(g_settingsPopupCard,
                                  ui_theme_is_light() ? ui_theme_color_card_border() : lv_color_hex(0x38BDF8),
                                  0);
    lv_obj_set_style_border_opa(g_settingsPopupCard, ui_theme_is_light() ? LV_OPA_COVER : 90, 0);
    lv_obj_set_style_border_width(g_settingsPopupCard, 1, 0);
    lv_obj_set_style_radius(g_settingsPopupCard, 14, 0);
    lv_obj_clear_flag(g_settingsPopupCard, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    g_settingsPopupTitle = lv_label_create(g_settingsPopupCard);
    lv_obj_set_width(g_settingsPopupTitle, 146);
    lv_obj_set_style_text_align(g_settingsPopupTitle, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(g_settingsPopupTitle, &my_font_misans_20, 0);
    lv_obj_set_style_text_color(g_settingsPopupTitle, ui_theme_color_text(), 0);
    lv_obj_set_style_text_opa(g_settingsPopupTitle, ui_theme_is_light() ? LV_OPA_COVER : 235, 0);
    lv_label_set_long_mode(g_settingsPopupTitle, LV_LABEL_LONG_DOT);
    lv_obj_align(g_settingsPopupTitle, LV_ALIGN_TOP_MID, 0, 8);

    g_settingsPopupList = lv_obj_create(g_settingsPopupCard);
    lv_obj_remove_style_all(g_settingsPopupList);
    lv_obj_set_size(g_settingsPopupList, 146, 76);
    lv_obj_align(g_settingsPopupList, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_layout(g_settingsPopupList, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(g_settingsPopupList, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g_settingsPopupList, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(g_settingsPopupList, 5, 0);
    lv_obj_clear_flag(g_settingsPopupList, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    for (int i = 0; i < kSettingsPopupRowCount; ++i) {
        g_settingsPopupRows[i] = lv_obj_create(g_settingsPopupList);
        lv_obj_remove_style_all(g_settingsPopupRows[i]);
        lv_obj_set_size(g_settingsPopupRows[i], 138, 22);
        lv_obj_set_style_bg_color(g_settingsPopupRows[i],
                                  ui_theme_is_light() ? ui_theme_color_card() : lv_color_hex(0x1F2937),
                                  0);
        lv_obj_set_style_bg_opa(g_settingsPopupRows[i], ui_theme_is_light() ? LV_OPA_COVER : 175, 0);
        lv_obj_set_style_bg_color(g_settingsPopupRows[i], ui_theme_color_focus_bg(), LV_STATE_FOCUSED);
        lv_obj_set_style_bg_opa(g_settingsPopupRows[i], ui_theme_is_light() ? LV_OPA_COVER : 220,
                                LV_STATE_FOCUSED);
        lv_obj_set_style_radius(g_settingsPopupRows[i], 10, 0);
        lv_obj_clear_flag(g_settingsPopupRows[i], LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

        g_settingsPopupRowLabels[i] = lv_label_create(g_settingsPopupRows[i]);
        lv_obj_set_width(g_settingsPopupRowLabels[i], 126);
        lv_obj_set_style_text_align(g_settingsPopupRowLabels[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(g_settingsPopupRowLabels[i], &my_font_misans_20, 0);
        lv_obj_set_style_text_color(g_settingsPopupRowLabels[i], ui_theme_color_text(), 0);
        lv_obj_set_style_text_opa(g_settingsPopupRowLabels[i], ui_theme_is_light() ? LV_OPA_COVER : 210, 0);
        lv_label_set_long_mode(g_settingsPopupRowLabels[i], LV_LABEL_LONG_DOT);
        lv_obj_center(g_settingsPopupRowLabels[i]);
    }
}

void refreshSettingsPopupLocked() {
    if (!g_settingsPopupVisible || g_settingsPopupMask == nullptr || g_settingsPopupTitle == nullptr) {
        return;
    }

    // ModeConfirm 状态下隐藏第三行;其它状态恢复显示。每次刷新都重置,避免上一态残留。
    if (g_settingsPopupRows[2] != nullptr) {
        if (g_settingsPopupMode == SettingsPopupMode::ModeConfirm) {
            lv_obj_add_flag(g_settingsPopupRows[2], LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_IGNORE_LAYOUT);
        } else {
            lv_obj_clear_flag(g_settingsPopupRows[2], LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_IGNORE_LAYOUT);
        }
    }

    // ModeConfirm 标题需要换行成两行(主句 + 提示),其它状态保持单行截断。
    // 同时把按钮列表高度收成 50px(BOTTOM_MID 锚点会自动把 list top 下推到 y=76),
    // 给两行 20pt 字 ~50px+ 实际高度让出足够空间,避免压在"取消"按钮上。
    if (g_settingsPopupMode == SettingsPopupMode::ModeConfirm) {
        lv_label_set_long_mode(g_settingsPopupTitle, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_line_space(g_settingsPopupTitle, 2, 0);
        lv_obj_align(g_settingsPopupTitle, LV_ALIGN_TOP_MID, 0, 4);
        if (g_settingsPopupList != nullptr) {
            lv_obj_set_height(g_settingsPopupList, 50);
        }
    } else {
        lv_label_set_long_mode(g_settingsPopupTitle, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_line_space(g_settingsPopupTitle, 0, 0);
        lv_obj_align(g_settingsPopupTitle, LV_ALIGN_TOP_MID, 0, 8);
        if (g_settingsPopupList != nullptr) {
            lv_obj_set_height(g_settingsPopupList, 76);
        }
    }

    const char* rowTexts[kSettingsPopupRowCount] = {"", "", ""};
    char titleBuf[48] = {};
    if (g_settingsPopupMode == SettingsPopupMode::Volume) {
        snprintf(titleBuf, sizeof(titleBuf), "音量 %u%%", static_cast<unsigned>(AppIdfAudio::getVolume()));
        rowTexts[0] = "减小 10%";
        rowTexts[1] = "完成";
        rowTexts[2] = "增大 10%";
    } else if (g_settingsPopupMode == SettingsPopupMode::Network) {
        snprintf(titleBuf, sizeof(titleBuf), "网络模式");
        rowTexts[0] = "自动";
        rowTexts[1] = "WiFi";
        rowTexts[2] = "4G";
    } else if (g_settingsPopupMode == SettingsPopupMode::WifiReset) {
        snprintf(titleBuf, sizeof(titleBuf), "重置WiFi?");
        rowTexts[0] = "取消";
        rowTexts[1] = "确认重置";
        rowTexts[2] = "取消";
    } else if (g_settingsPopupMode == SettingsPopupMode::Theme) {
        snprintf(titleBuf, sizeof(titleBuf), "主题");
        rowTexts[0] = "亮色";
        rowTexts[1] = "暗色";
        rowTexts[2] = "完成";
    } else if (g_settingsPopupMode == SettingsPopupMode::Mode) {
        snprintf(titleBuf, sizeof(titleBuf), "切换模式 (重启)");
        rowTexts[0] = "蓝牙空调";
        rowTexts[1] = "红外";
        rowTexts[2] = "射频433";
    } else if (g_settingsPopupMode == SettingsPopupMode::ModeConfirm) {
        // 标题宽 146px,用"切到"前缀让 BLE/IR/RF433 三种中文名都能稳定放在第一行内,
        // 避免"切换到 蓝牙空调"超宽自动折行变成 3 行进而压住按钮区。
        snprintf(titleBuf, sizeof(titleBuf), "切到 %s\n(切换会重启)", AppIdfAppMode::nameCn(g_settingsPopupPendingMode));
        rowTexts[0] = "取消";
        rowTexts[1] = "确认切换";
        rowTexts[2] = "";
    } else {
        snprintf(titleBuf, sizeof(titleBuf), "设置");
    }

    lv_label_set_text(g_settingsPopupTitle, titleBuf);
    for (int i = 0; i < kSettingsPopupRowCount; ++i) {
        if (g_settingsPopupRowLabels[i] != nullptr) {
            lv_label_set_text(g_settingsPopupRowLabels[i], rowTexts[i]);
        }
        if (g_settingsPopupRows[i] != nullptr) {
            if (i == g_settingsPopupSelectedIndex) {
                lv_obj_add_state(g_settingsPopupRows[i], LV_STATE_FOCUSED);
            } else {
                lv_obj_clear_state(g_settingsPopupRows[i], LV_STATE_FOCUSED);
            }
        }
    }

    lv_obj_clear_flag(g_settingsPopupMask, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(g_settingsPopupMask);
}

void openSettingsPopupLocked(SettingsPopupMode mode) {
    createSettingsPopupLocked();
    if (g_settingsPopupMask == nullptr) {
        return;
    }

    g_settingsPopupMode = mode;
    g_settingsPopupVisible = true;
    if (mode == SettingsPopupMode::Network) {
        g_settingsPopupSelectedIndex = static_cast<int>(AppIdfTransport::snapshot().mode);
    } else if (mode == SettingsPopupMode::Theme) {
        g_settingsPopupSelectedIndex = g_currentTheme == UI_THEME_LIGHT ? 0 : 1;
    } else if (mode == SettingsPopupMode::Mode) {
        g_settingsPopupSelectedIndex = static_cast<int>(AppIdfAppMode::current());
    } else if (mode == SettingsPopupMode::WifiReset) {
        // 危险操作,默认聚焦"取消"(row 0),用户须主动按到"确认重置"
        g_settingsPopupSelectedIndex = 0;
    } else {
        g_settingsPopupSelectedIndex = 1;
    }
    refreshSettingsPopupLocked();
}

void closeSettingsPopupLocked() {
    if (g_settingsPopupMask != nullptr) {
        lv_obj_add_flag(g_settingsPopupMask, LV_OBJ_FLAG_HIDDEN);
    }
    g_settingsPopupVisible = false;
    g_settingsPopupMode = SettingsPopupMode::None;
}

void focusNextSettingsPopupItemLocked() {
    if (!g_settingsPopupVisible) {
        return;
    }
    // ModeConfirm 只有 2 行可循环(取消/确认),其它状态仍走 3 行。
    const int rowCount = (g_settingsPopupMode == SettingsPopupMode::ModeConfirm) ? 2 : kSettingsPopupRowCount;
    g_settingsPopupSelectedIndex = (g_settingsPopupSelectedIndex + 1) % rowCount;
    refreshSettingsPopupLocked();
}

void confirmSettingsPopupSelectionLocked() {
    if (!g_settingsPopupVisible) {
        return;
    }

    if (g_settingsPopupMode == SettingsPopupMode::Volume) {
        int target = static_cast<int>(AppIdfAudio::getVolume());
        if (g_settingsPopupSelectedIndex == 0) {
            target -= 10;
        } else if (g_settingsPopupSelectedIndex == 2) {
            target += 10;
        } else {
            closeSettingsPopupLocked();
            return;
        }

        if (target < 0) {
            target = 0;
        } else if (target > 100) {
            target = 100;
        }
        const esp_err_t err = AppIdfAudio::setVolume(static_cast<uint8_t>(target));
        if (err != ESP_OK) {
            LOG_W(TAG_UI_IDF, "Failed to set volume from settings: %s", esp_err_to_name(err));
        }
        updateSettingsSummaryLocked();
        refreshSettingsPopupLocked();
        return;
    }

    if (g_settingsPopupMode == SettingsPopupMode::Network) {
        AppIdfTransport::NetMode mode = AppIdfTransport::NetMode::AUTO;
        if (g_settingsPopupSelectedIndex == 1) {
            mode = AppIdfTransport::NetMode::WIFI_ONLY;
        } else if (g_settingsPopupSelectedIndex == 2) {
            mode = AppIdfTransport::NetMode::CELLULAR_ONLY;
        }
        const esp_err_t err = AppIdfTransport::requestMode(mode);
        if (err != ESP_OK) {
            LOG_W(TAG_UI_IDF, "Failed to request network mode from settings: %s", esp_err_to_name(err));
        }
        updateSettingsSummaryLocked();
        closeSettingsPopupLocked();
        return;
    }

    if (g_settingsPopupMode == SettingsPopupMode::Theme) {
        if (g_settingsPopupSelectedIndex == 0) {
            applyThemePreferenceLocked(UI_THEME_LIGHT, true, true);
        } else if (g_settingsPopupSelectedIndex == 1) {
            applyThemePreferenceLocked(UI_THEME_DARK, true, true);
        } else {
            closeSettingsPopupLocked();
        }
        updateSettingsSummaryLocked();
        return;
    }

    if (g_settingsPopupMode == SettingsPopupMode::Mode) {
        AppIdfAppMode::Mode target = static_cast<AppIdfAppMode::Mode>(g_settingsPopupSelectedIndex);
        if (target == AppIdfAppMode::current()) {
            // 选中的就是当前模式，无需切换
            closeSettingsPopupLocked();
            return;
        }
        // 翻页到二次确认态：保留 pendingMode 与返回索引，默认聚焦"取消"行。
        g_settingsPopupPendingMode = target;
        g_settingsPopupModeReturnIndex = g_settingsPopupSelectedIndex;
        g_settingsPopupMode = SettingsPopupMode::ModeConfirm;
        g_settingsPopupSelectedIndex = 0;
        refreshSettingsPopupLocked();
        return;
    }

    if (g_settingsPopupMode == SettingsPopupMode::ModeConfirm) {
        if (g_settingsPopupSelectedIndex == 0) {
            // 取消：翻回 Mode 选择态，焦点回到用户刚选过的目标行。
            g_settingsPopupMode = SettingsPopupMode::Mode;
            g_settingsPopupSelectedIndex = g_settingsPopupModeReturnIndex;
            refreshSettingsPopupLocked();
            return;
        }
        // 确认：写 NVS + 软重启。switchAndRestart 内部会延迟后调用 esp_restart 不返回。
        LOG_I(TAG_UI_IDF, "UI confirmed app mode switch to %s",
              AppIdfAppMode::nameAscii(g_settingsPopupPendingMode));
        const esp_err_t err = AppIdfAppMode::switchAndRestart(g_settingsPopupPendingMode, 800);
        if (err != ESP_OK) {
            LOG_W(TAG_UI_IDF, "App mode switch failed: %s", esp_err_to_name(err));
        }
        closeSettingsPopupLocked();
        return;
    }

    if (g_settingsPopupMode == SettingsPopupMode::WifiReset) {
        if (g_settingsPopupSelectedIndex == 1) {
            // 与串口 WIFICLEAR 一致:清 NVS 凭据后立即开启配网热点
            const esp_err_t clearErr = AppIdfNetwork::clearCredentials();
            if (clearErr != ESP_OK) {
                LOG_W(TAG_UI_IDF, "WiFi reset: clearCredentials failed: %s", esp_err_to_name(clearErr));
            } else {
                const esp_err_t portalErr = AppIdfNetwork::startPortal();
                if (portalErr != ESP_OK) {
                    LOG_W(TAG_UI_IDF, "WiFi reset: startPortal failed: %s", esp_err_to_name(portalErr));
                }
            }
        }
        closeSettingsPopupLocked();
        updateSettingsSummaryLocked();
        return;
    }
}

void setBlePairPopupLocked(const char* title, const char* detail, bool visible) {
    if (g_blePairPopupMask == nullptr) {
        return;
    }

    if (visible) {
        if (g_blePairPopupTitle != nullptr) {
            lv_label_set_text(g_blePairPopupTitle, title != nullptr ? title : "");
        }
        if (g_blePairPopupDetail != nullptr) {
            lv_label_set_text(g_blePairPopupDetail, detail != nullptr ? detail : "");
        }
        lv_obj_clear_flag(g_blePairPopupMask, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(g_blePairPopupMask);
    } else {
        lv_obj_add_flag(g_blePairPopupMask, LV_OBJ_FLAG_HIDDEN);
    }
}

const char* otaStatusText(const AppIdfOta::Snapshot& ota) {
    if (ota.rollbackPending) {
        return "等待验证";
    }
    if (ota.pending) {
        return "准备更新";
    }
    if (ota.busy) {
        return ota.progress >= 100 ? "准备重启" : "正在更新";
    }
    if (ota.progress >= 100) {
        return "更新完成";
    }
    if (ota.lastError != ESP_OK || ota.lastReason[0] != '\0') {
        return "更新失败";
    }
    return "更新状态";
}

bool otaSnapshotActive(const AppIdfOta::Snapshot& ota) {
    return ota.busy || ota.pending || ota.rollbackPending;
}

int otaProgressValue(const AppIdfOta::Snapshot& ota) {
    if (ota.rollbackPending) {
        return 100;
    }
    if (ota.progress < 0) {
        return 0;
    }
    if (ota.progress > 100) {
        return 100;
    }
    return ota.progress;
}

void createOtaScreenLocked() {
    if (g_otaScreen != nullptr) {
        return;
    }

    g_otaScreen = lv_obj_create(nullptr);
    lv_obj_clear_flag(g_otaScreen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(g_otaScreen, ui_theme_is_light() ? ui_theme_color_bg() : lv_color_hex(0x0D1117),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_otaScreen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(g_otaScreen,
                                   ui_theme_is_light() ? ui_theme_color_bg_grad() : lv_color_hex(0x102033),
                                   LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(g_otaScreen, LV_GRAD_DIR_VER, LV_PART_MAIN);

    g_otaTitleLabel = lv_label_create(g_otaScreen);
    lv_label_set_text(g_otaTitleLabel, "固件更新");
    lv_obj_set_style_text_color(g_otaTitleLabel, ui_theme_color_text(), 0);
    lv_obj_set_style_text_opa(g_otaTitleLabel, ui_theme_is_light() ? LV_OPA_COVER : 235, 0);
    lv_obj_set_style_text_font(g_otaTitleLabel, &my_font_misans_20, 0);
    lv_obj_align(g_otaTitleLabel, LV_ALIGN_TOP_MID, 0, 26);

    g_otaStatusLabel = lv_label_create(g_otaScreen);
    lv_obj_set_width(g_otaStatusLabel, 190);
    lv_obj_set_style_text_align(g_otaStatusLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(g_otaStatusLabel, ui_theme_color_focus(), 0);
    lv_obj_set_style_text_font(g_otaStatusLabel, &my_font_misans_20, 0);
    lv_label_set_long_mode(g_otaStatusLabel, LV_LABEL_LONG_DOT);
    lv_obj_align(g_otaStatusLabel, LV_ALIGN_TOP_MID, 0, 64);

    g_otaBar = lv_bar_create(g_otaScreen);
    lv_obj_set_size(g_otaBar, 172, 12);
    lv_bar_set_range(g_otaBar, 0, 100);
    lv_bar_set_value(g_otaBar, 0, LV_ANIM_OFF);
    lv_obj_set_style_radius(g_otaBar, 6, LV_PART_MAIN);
    lv_obj_set_style_radius(g_otaBar, 6, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(g_otaBar, ui_theme_is_light() ? lv_color_hex(0xD8E4F0) : lv_color_hex(0x263445),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_otaBar, ui_theme_is_light() ? LV_OPA_COVER : 150, LV_PART_MAIN);
    lv_obj_set_style_bg_color(g_otaBar, ui_theme_color_focus(), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(g_otaBar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_align(g_otaBar, LV_ALIGN_CENTER, 0, -8);

    g_otaPercentLabel = lv_label_create(g_otaScreen);
    lv_obj_set_style_text_color(g_otaPercentLabel, ui_theme_color_text(), 0);
    lv_obj_set_style_text_opa(g_otaPercentLabel, ui_theme_is_light() ? LV_OPA_COVER : 210, 0);
    lv_obj_set_style_text_font(g_otaPercentLabel, &lv_font_montserrat_14, 0);
    lv_obj_align(g_otaPercentLabel, LV_ALIGN_CENTER, 0, 22);

    g_otaDetailLabel = lv_label_create(g_otaScreen);
    lv_obj_set_width(g_otaDetailLabel, 198);
    lv_obj_set_style_text_align(g_otaDetailLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(g_otaDetailLabel, ui_theme_color_text_muted(), 0);
    lv_obj_set_style_text_opa(g_otaDetailLabel, ui_theme_is_light() ? 220 : 170, 0);
    lv_obj_set_style_text_font(g_otaDetailLabel, &lv_font_montserrat_10, 0);
    lv_label_set_long_mode(g_otaDetailLabel, LV_LABEL_LONG_DOT);
    lv_obj_align(g_otaDetailLabel, LV_ALIGN_BOTTOM_MID, 0, -28);
}

void refreshOtaUiLocked() {
    const AppIdfOta::Snapshot ota = AppIdfOta::snapshot();
    const bool active = otaSnapshotActive(ota);
    const uint32_t now = lv_tick_get();

    if (active) {
        createOtaScreenLocked();
        g_otaWasActive = true;
        g_otaFinishedUntilMs = 0;
        if (lv_scr_act() != g_otaScreen) {
            if (lv_scr_act() == ui_AIScreen) {
                ui_AIScreen_stop_anim();
            }
            lv_scr_load_anim(g_otaScreen, LV_SCR_LOAD_ANIM_FADE_ON, 180, 0, false);
        }
        g_activeScreenName = "ota";
    } else if (g_otaWasActive) {
        createOtaScreenLocked();
        if (g_otaFinishedUntilMs == 0) {
            g_otaFinishedUntilMs = now + 5000;
        }
        if (lv_scr_act() != g_otaScreen) {
            lv_scr_load_anim(g_otaScreen, LV_SCR_LOAD_ANIM_FADE_ON, 120, 0, false);
        }
        g_activeScreenName = "ota";
    } else {
        return;
    }

    const int progress = otaProgressValue(ota);
    if (g_otaStatusLabel != nullptr) {
        lv_label_set_text(g_otaStatusLabel, otaStatusText(ota));
        const bool failed = !active && (ota.lastError != ESP_OK || ota.progress < 100);
        lv_obj_set_style_text_color(g_otaStatusLabel,
                                    failed ? ui_theme_color_error()
                                           : (ota.rollbackPending ? ui_theme_color_warning() : ui_theme_color_focus()),
                                    0);
    }
    if (g_otaBar != nullptr) {
        lv_bar_set_value(g_otaBar, progress, LV_ANIM_ON);
        const bool failed = !active && (ota.lastError != ESP_OK || ota.progress < 100);
        lv_obj_set_style_bg_color(g_otaBar,
                                  failed ? ui_theme_color_error()
                                         : (ota.rollbackPending ? ui_theme_color_warning() : ui_theme_color_focus()),
                                  LV_PART_INDICATOR);
    }
    if (g_otaPercentLabel != nullptr) {
        char percentText[16];
        snprintf(percentText, sizeof(percentText), "%d%%", progress);
        lv_label_set_text(g_otaPercentLabel, percentText);
    }
    if (g_otaDetailLabel != nullptr) {
        char detail[160];
        const char* version = ota.version[0] ? ota.version : "-";
        const char* reason = ota.lastReason[0] ? ota.lastReason : "-";
        if (ota.expectedSize > 0) {
            snprintf(detail,
                     sizeof(detail),
                     "%s  %u/%uKB  %s",
                     version,
                     static_cast<unsigned>(ota.writtenSize / 1024),
                     static_cast<unsigned>(ota.expectedSize / 1024),
                     reason);
        } else {
            snprintf(detail, sizeof(detail), "%s  %s", version, reason);
        }
        lv_label_set_text(g_otaDetailLabel, detail);
    }

    if (!active && g_otaFinishedUntilMs != 0 && now > g_otaFinishedUntilMs) {
        g_otaWasActive = false;
        g_otaFinishedUntilMs = 0;
        if (lv_scr_act() == g_otaScreen) {
            showMainLocked();
        }
    }
}

void createBlePairScreenLocked() {
    if (g_blePairScreen != nullptr) {
        return;
    }

    g_blePairScreen = lv_obj_create(nullptr);
    lv_obj_clear_flag(g_blePairScreen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(g_blePairScreen, ui_theme_is_light() ? ui_theme_color_bg() : lv_color_hex(0x0D1117),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_blePairScreen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(g_blePairScreen,
                                   ui_theme_is_light() ? ui_theme_color_bg_grad() : lv_color_hex(0x102033),
                                   LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(g_blePairScreen, LV_GRAD_DIR_VER, LV_PART_MAIN);

    lv_obj_t* title = lv_label_create(g_blePairScreen);
    lv_label_set_text(title, "蓝牙配对");
    lv_obj_set_style_text_color(title, ui_theme_color_text(), 0);
    lv_obj_set_style_text_opa(title, ui_theme_is_light() ? LV_OPA_COVER : 230, 0);
    lv_obj_set_style_text_font(title, &my_font_misans_20, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    g_blePairStatusLabel = lv_label_create(g_blePairScreen);
    lv_obj_set_width(g_blePairStatusLabel, 210);
    lv_obj_set_style_text_align(g_blePairStatusLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(g_blePairStatusLabel,
                                ui_theme_is_light() ? ui_theme_color_text_muted() : lv_color_hex(0xBFEFFF),
                                0);
    lv_obj_set_style_text_opa(g_blePairStatusLabel, ui_theme_is_light() ? 230 : 190, 0);
    lv_obj_set_style_text_font(g_blePairStatusLabel, &my_font_misans_20, 0);
    lv_label_set_long_mode(g_blePairStatusLabel, LV_LABEL_LONG_DOT);
    lv_obj_align(g_blePairStatusLabel, LV_ALIGN_TOP_MID, 0, 34);

    lv_obj_t* list = lv_obj_create(g_blePairScreen);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, 212, 154);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 62);
    lv_obj_set_layout(list, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(list, 4, 0);
    lv_obj_clear_flag(list, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    for (int i = 0; i < kBlePairRowCount; ++i) {
        g_blePairRows[i] = lv_obj_create(list);
        lv_obj_remove_style_all(g_blePairRows[i]);
        lv_obj_set_size(g_blePairRows[i], 208, 22);
        lv_obj_add_style(g_blePairRows[i], &style_glass_list, 0);
        lv_obj_add_style(g_blePairRows[i], &style_glass_list_focused, LV_STATE_FOCUSED);
        lv_obj_set_style_radius(g_blePairRows[i], 10, 0);
        lv_obj_clear_flag(g_blePairRows[i], LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

        g_blePairNameLabels[i] = lv_label_create(g_blePairRows[i]);
        lv_obj_set_width(g_blePairNameLabels[i], 192);
        lv_label_set_long_mode(g_blePairNameLabels[i], LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_font(g_blePairNameLabels[i], &my_font_misans_20, 0);
        lv_obj_set_style_text_color(g_blePairNameLabels[i], ui_theme_color_text(), 0);
        lv_obj_set_style_text_opa(g_blePairNameLabels[i], ui_theme_is_light() ? LV_OPA_COVER : 220, 0);
        lv_obj_align(g_blePairNameLabels[i], LV_ALIGN_LEFT_MID, 8, 0);
    }

    g_blePairPopupMask = lv_obj_create(g_blePairScreen);
    lv_obj_remove_style_all(g_blePairPopupMask);
    lv_obj_set_size(g_blePairPopupMask, 240, 240);
    lv_obj_align(g_blePairPopupMask, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(g_blePairPopupMask, ui_theme_color_overlay(), 0);
    lv_obj_set_style_bg_opa(g_blePairPopupMask, ui_theme_is_light() ? 150 : 120, 0);
    lv_obj_add_flag(g_blePairPopupMask, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(g_blePairPopupMask, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* card = lv_obj_create(g_blePairPopupMask);
    lv_obj_set_size(card, 166, 104);
    lv_obj_center(card);
    lv_obj_add_style(card, &style_card, 0);
    lv_obj_set_style_bg_color(card, ui_theme_is_light() ? ui_theme_color_card() : lv_color_hex(0x111827), 0);
    lv_obj_set_style_bg_opa(card, 240, 0);
    lv_obj_set_style_border_color(card, ui_theme_is_light() ? ui_theme_color_card_border() : lv_color_hex(0x38BDF8), 0);
    lv_obj_set_style_border_opa(card, ui_theme_is_light() ? LV_OPA_COVER : 90, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    g_blePairPopupTitle = lv_label_create(card);
    lv_obj_set_style_text_font(g_blePairPopupTitle, &my_font_misans_20, 0);
    lv_obj_set_style_text_color(g_blePairPopupTitle, ui_theme_color_text(), 0);
    lv_obj_set_style_text_opa(g_blePairPopupTitle, ui_theme_is_light() ? LV_OPA_COVER : 235, 0);
    lv_obj_align(g_blePairPopupTitle, LV_ALIGN_CENTER, 0, -14);

    g_blePairPopupDetail = lv_label_create(card);
    lv_obj_set_width(g_blePairPopupDetail, 138);
    lv_obj_set_style_text_align(g_blePairPopupDetail, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(g_blePairPopupDetail, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(g_blePairPopupDetail, ui_theme_color_text_muted(), 0);
    lv_obj_set_style_text_opa(g_blePairPopupDetail, ui_theme_is_light() ? 220 : 170, 0);
    lv_label_set_long_mode(g_blePairPopupDetail, LV_LABEL_LONG_DOT);
    lv_obj_align(g_blePairPopupDetail, LV_ALIGN_CENTER, 0, 16);
}

void refreshBlePairUiLocked() {
    if (g_blePairScreen == nullptr) {
        return;
    }

    const AppIdfBleAircon::PairingState state = AppIdfBleAircon::getPairingState();
    size_t count = AppIdfBleAircon::getPairingResultCount();
    if (count > AppIdfBleAircon::kMaxPairScanResults) {
        count = AppIdfBleAircon::kMaxPairScanResults;
    }

    if (count == 0) {
        g_blePairSelectedIndex = 0;
        g_blePairWindowStart = 0;
    } else if (g_blePairSelectedIndex >= static_cast<int>(count)) {
        g_blePairSelectedIndex = 0;
        g_blePairWindowStart = 0;
    } else if (g_blePairSelectedIndex < g_blePairWindowStart) {
        g_blePairWindowStart = g_blePairSelectedIndex;
    } else if (g_blePairSelectedIndex >= g_blePairWindowStart + kBlePairRowCount) {
        g_blePairWindowStart = g_blePairSelectedIndex - kBlePairRowCount + 1;
    }

    if (count <= kBlePairRowCount) {
        g_blePairWindowStart = 0;
    } else {
        const int maxWindowStart = static_cast<int>(count) - kBlePairRowCount;
        if (g_blePairWindowStart > maxWindowStart) {
            g_blePairWindowStart = maxWindowStart;
        }
    }

    const uint32_t now = lv_tick_get();
    if (g_lastBlePairState != static_cast<int>(state)) {
        g_lastBlePairState = static_cast<int>(state);
        if (state == AppIdfBleAircon::PairingState::Success) {
            g_blePairPopupUntilMs = now + 900;
            g_blePairAutoExitMs = now + 1000;
        } else if (state == AppIdfBleAircon::PairingState::Error) {
            g_blePairPopupUntilMs = now + 1200;
            g_blePairAutoExitMs = 0;
        } else {
            g_blePairPopupUntilMs = 0;
            g_blePairAutoExitMs = 0;
        }
    }

    if (g_blePairAutoExitMs != 0 && now > g_blePairAutoExitMs) {
        g_blePairAutoExitMs = 0;
        showMainLocked();
        return;
    }

    const char* status = "选择蓝牙设备";
    if (state == AppIdfBleAircon::PairingState::Scanning) {
        status = "正在扫描...";
    } else if (state == AppIdfBleAircon::PairingState::Pairing) {
        status = "正在配对...";
    } else if (state == AppIdfBleAircon::PairingState::Success) {
        status = "配对成功";
    } else if (state == AppIdfBleAircon::PairingState::Error) {
        status = count > 0 ? "配对失败" : "未发现设备";
    } else if (count == 0) {
        status = "未发现设备";
    }

    if (g_blePairStatusLabel != nullptr) {
        char statusText[40];
        if (count > kBlePairRowCount && state != AppIdfBleAircon::PairingState::Scanning &&
            state != AppIdfBleAircon::PairingState::Pairing &&
            state != AppIdfBleAircon::PairingState::Success) {
            snprintf(statusText, sizeof(statusText), "%s %d/%u", status, g_blePairSelectedIndex + 1,
                     static_cast<unsigned>(count));
            lv_label_set_text(g_blePairStatusLabel, statusText);
        } else {
            lv_label_set_text(g_blePairStatusLabel, status);
        }
    }

    for (int i = 0; i < kBlePairRowCount; ++i) {
        if (g_blePairRows[i] == nullptr) {
            continue;
        }

        const int resultIndex = g_blePairWindowStart + i;
        if (resultIndex >= static_cast<int>(count) || state == AppIdfBleAircon::PairingState::Scanning) {
            lv_obj_add_flag(g_blePairRows[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }

        lv_obj_clear_flag(g_blePairRows[i], LV_OBJ_FLAG_HIDDEN);
        const bool focused = (resultIndex == g_blePairSelectedIndex &&
                              state != AppIdfBleAircon::PairingState::Pairing &&
                              state != AppIdfBleAircon::PairingState::Success);
        if (focused) {
            lv_obj_add_state(g_blePairRows[i], LV_STATE_FOCUSED);
        } else {
            lv_obj_clear_state(g_blePairRows[i], LV_STATE_FOCUSED);
        }

        AppIdfBleAircon::PairScanEntry entry;
        if (!AppIdfBleAircon::getPairingResult(static_cast<size_t>(resultIndex), &entry)) {
            lv_obj_add_flag(g_blePairRows[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }

        if (g_blePairNameLabels[i] != nullptr) {
            // 选中行用 SCROLL_CIRCULAR 跑马灯露出长名字，未选中保留省略号；
            // long_mode 切换会 del anim + 复位 offset，所以必须只在变化时调用，
            // 否则周期 refresh 会持续重置滚动位置。set_text 在 CIRCULAR 下会保留 act_time。
            const lv_label_long_mode_t desiredMode = focused ? LV_LABEL_LONG_SCROLL_CIRCULAR : LV_LABEL_LONG_DOT;
            if (lv_label_get_long_mode(g_blePairNameLabels[i]) != desiredMode) {
                lv_label_set_long_mode(g_blePairNameLabels[i], desiredMode);
            }
            lv_label_set_text(g_blePairNameLabels[i], entry.name[0] ? entry.name : "BLE设备");
            lv_obj_set_style_text_color(g_blePairNameLabels[i],
                                        entry.hasService ? ui_theme_color_focus() : ui_theme_color_text(),
                                        0);
        }
    }

    const bool showPairingPopup = state == AppIdfBleAircon::PairingState::Pairing;
    const bool showTimedPopup = g_blePairPopupUntilMs != 0 && now < g_blePairPopupUntilMs;
    if (showPairingPopup || showTimedPopup) {
        const char* popupTitle = "配对中";
        if (state == AppIdfBleAircon::PairingState::Success) {
            popupTitle = "配对成功";
        } else if (state == AppIdfBleAircon::PairingState::Error) {
            popupTitle = "配对失败";
        }
        setBlePairPopupLocked(popupTitle, "", true);
    } else {
        setBlePairPopupLocked("", "", false);
    }
}

void focusNextBlePairItemLocked() {
    const AppIdfBleAircon::PairingState state = AppIdfBleAircon::getPairingState();
    if (state == AppIdfBleAircon::PairingState::Scanning || state == AppIdfBleAircon::PairingState::Pairing ||
        state == AppIdfBleAircon::PairingState::Success) {
        return;
    }

    const size_t count = AppIdfBleAircon::getPairingResultCount();
    if (count == 0) {
        return;
    }

    const size_t cappedCount = count > AppIdfBleAircon::kMaxPairScanResults ? AppIdfBleAircon::kMaxPairScanResults : count;
    g_blePairSelectedIndex = (g_blePairSelectedIndex + 1) % static_cast<int>(cappedCount);
    if (g_blePairSelectedIndex == 0) {
        g_blePairWindowStart = 0;
    }
    refreshBlePairUiLocked();
}

void confirmBlePairSelectionLocked() {
    const AppIdfBleAircon::PairingState state = AppIdfBleAircon::getPairingState();
    if (state == AppIdfBleAircon::PairingState::Scanning || state == AppIdfBleAircon::PairingState::Pairing ||
        state == AppIdfBleAircon::PairingState::Success) {
        return;
    }

    const size_t count = AppIdfBleAircon::getPairingResultCount();
    if (count == 0) {
        AppIdfBleAircon::startPairingScan();
        g_blePairWindowStart = 0;
        g_lastBlePairState = -1;
        refreshBlePairUiLocked();
        return;
    }

    const esp_err_t err = AppIdfBleAircon::startPairing(static_cast<size_t>(g_blePairSelectedIndex));
    if (err == ESP_OK) {
        setBlePairPopupLocked("配对中", "", true);
    } else {
        setBlePairPopupLocked("配对失败", "", true);
        g_blePairPopupUntilMs = lv_tick_get() + 1200;
    }
    g_lastBlePairState = -1;
    refreshBlePairUiLocked();
}

void setStatusArcColor(lv_obj_t* arc, lv_color_t color, lv_opa_t opa) {
    if (arc == nullptr) {
        return;
    }
    lv_obj_set_style_arc_color(arc, color, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(arc, opa, LV_PART_MAIN);
}

void setStatusLabelColor(lv_obj_t* label, lv_color_t color, lv_opa_t opa) {
    if (label == nullptr) {
        return;
    }
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_style_text_opa(label, opa, 0);
}

void setStatusPill(lv_obj_t* pill, lv_color_t dotColor, const char* text) {
    if (pill == nullptr) {
        return;
    }

    lv_obj_t* dot = lv_obj_get_child(pill, 0);
    lv_obj_t* label = lv_obj_get_child(pill, 1);
    if (dot != nullptr) {
        lv_obj_set_style_bg_color(dot, dotColor, 0);
        lv_obj_set_style_shadow_color(dot, dotColor, 0);
    }
    if (label != nullptr && text != nullptr && strcmp(lv_label_get_text(label), text) != 0) {
        lv_label_set_text(label, text);
    }
}

void updateBatteryWidgets(const AppIdfSensors::BatterySnapshot& battery) {
    if (ui_BatteryFill == nullptr || ui_BatteryBody == nullptr || ui_BatteryCap == nullptr ||
        ui_LabelBattery == nullptr) {
        return;
    }

    const lv_color_t inactiveSegmentColor = ui_theme_is_light() ? lv_color_hex(0xD8E4F0) : lv_color_hex(0x334155);
    const lv_color_t chargeIconColor = ui_theme_is_light() ? lv_color_hex(0xF59E0B) : lv_color_hex(0xFACC15);
    const uint32_t childCount = lv_obj_get_child_cnt(ui_BatteryFill);
    if (childCount == 0) {
        return;
    }

    if (!battery.valid || battery.percent < 0) {
        lv_obj_add_flag(ui_LabelBattery, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_border_color(ui_BatteryBody, ui_theme_color_text_subtle(), 0);
        lv_obj_set_style_border_opa(ui_BatteryBody, 120, 0);
        lv_obj_set_style_bg_color(ui_BatteryCap, ui_theme_color_text_subtle(), 0);
        lv_obj_set_style_bg_opa(ui_BatteryCap, 120, 0);
        for (uint32_t i = 0; i < childCount; ++i) {
            lv_obj_t* segment = lv_obj_get_child(ui_BatteryFill, i);
            lv_obj_set_style_bg_color(segment, inactiveSegmentColor, 0);
            lv_obj_set_style_bg_opa(segment, 90, 0);
        }
        return;
    }

    const lv_color_t batteryColor = batteryColorForPercent(battery.percent);
    const int litSegments = batteryPercentToSegments(battery.percent, static_cast<int>(childCount));

    lv_label_set_text(ui_LabelBattery, LV_SYMBOL_CHARGE);
    lv_obj_set_style_text_color(ui_LabelBattery, chargeIconColor, 0);
    lv_obj_set_style_text_opa(ui_LabelBattery, battery.charging ? LV_OPA_COVER : 0, 0);
    if (battery.charging) {
        lv_obj_clear_flag(ui_LabelBattery, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(ui_LabelBattery, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_set_style_border_color(ui_BatteryBody, batteryColor, 0);
    lv_obj_set_style_border_opa(ui_BatteryBody, 180, 0);
    lv_obj_set_style_bg_color(ui_BatteryCap, batteryColor, 0);
    lv_obj_set_style_bg_opa(ui_BatteryCap, 180, 0);

    for (uint32_t i = 0; i < childCount; ++i) {
        lv_obj_t* segment = lv_obj_get_child(ui_BatteryFill, i);
        if (static_cast<int>(i) < litSegments) {
            lv_obj_set_style_bg_color(segment, batteryColor, 0);
            lv_obj_set_style_bg_opa(segment, LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_bg_color(segment, inactiveSegmentColor, 0);
            lv_obj_set_style_bg_opa(segment, 120, 0);
        }
    }
}

void ensureStatusWidgetsLocked() {
    if (ui_PanelTopTitle == nullptr) {
        return;
    }

    if (g_statusVolumeLabel != nullptr && !lv_obj_is_valid(g_statusVolumeLabel)) {
        g_statusVolumeLabel = nullptr;
    }

    lv_obj_set_style_pad_hor(ui_PanelTopTitle, ui_theme_is_light() ? 14 : 10, 0);
    lv_obj_set_style_pad_top(ui_PanelTopTitle, 8, 0);
    lv_obj_set_style_pad_column(ui_PanelTopTitle, ui_theme_is_light() ? 6 : 8, 0);
    lv_obj_set_flex_align(ui_PanelTopTitle, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    if (ui_LabelBleStatus != nullptr) {
        lv_obj_set_style_text_font(ui_LabelBleStatus,
                                   ui_theme_is_light() ? &lv_font_montserrat_14 : &lv_font_montserrat_12,
                                   0);
    }

    if (g_statusVolumeLabel == nullptr) {
        g_statusVolumeLabel = lv_label_create(ui_PanelTopTitle);
        lv_obj_set_style_text_font(g_statusVolumeLabel, &lv_font_montserrat_12, 0);
    }
    lv_label_set_text(g_statusVolumeLabel, LV_SYMBOL_VOLUME_MAX " --");
    lv_obj_set_style_text_font(g_statusVolumeLabel, ui_theme_is_light() ? &lv_font_montserrat_12 : &lv_font_montserrat_14,
                               0);
    lv_obj_set_width(g_statusVolumeLabel, ui_theme_is_light() ? 54 : LV_SIZE_CONTENT);
    lv_label_set_long_mode(g_statusVolumeLabel, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(g_statusVolumeLabel, LV_TEXT_ALIGN_RIGHT, 0);

    if (ui_PanelCard4G != nullptr) {
        lv_obj_move_to_index(ui_PanelCard4G, 1);
    }
    if (ui_LabelBleStatus != nullptr) {
        lv_obj_move_to_index(ui_LabelBleStatus, 2);
    }
    lv_obj_move_to_index(g_statusVolumeLabel, 2);
}

void updateStatusLocked() {
    if (lv_scr_act() != ui_MainScreen || ui_MainScreen == nullptr) {
        return;
    }

    ensureStatusWidgetsLocked();
    const AppIdfSensors::Snapshot sensors = AppIdfSensors::latest();
    const AppIdfNetwork::Snapshot network = AppIdfNetwork::snapshot();
    const AppIdfCellular::Snapshot cellular = AppIdfCellular::snapshot();
    const AppIdfTransport::Snapshot transport = AppIdfTransport::snapshot();
    const AppIdfAudio::Snapshot audio = AppIdfAudio::snapshot();

    const lv_color_t inactiveColor = ui_theme_is_light() ? lv_color_hex(0x6B7785) : ui_theme_color_text_subtle();
    const lv_opa_t inactiveOpa = ui_theme_is_light() ? 190 : 150;
    const lv_color_t wifiColor = network.connected ? ui_theme_color_focus()
                                                    : (network.connecting ? ui_theme_color_warning() : inactiveColor);
    const lv_opa_t wifiOpa = network.connected ? LV_OPA_COVER : (network.connecting ? 220 : inactiveOpa);
    const bool cellularActive = transport.active == AppIdfTransport::ActiveTransport::PPP_4G;
    const bool cellularConnecting = cellular.dialing || transport.cellularStage != AppIdfTransport::CellularStage::IDLE;
    const lv_color_t cellularColor = cellularActive ? ui_theme_color_focus()
                                                     : (cellularConnecting ? ui_theme_color_warning() : inactiveColor);
    const lv_opa_t cellularOpa = cellularActive ? LV_OPA_COVER : (cellularConnecting ? 220 : inactiveOpa);

    setStatusArcColor(ui_WifiArc1, wifiColor, wifiOpa);
    setStatusArcColor(ui_WifiArc2, wifiColor, wifiOpa);
    if (ui_WifiDot != nullptr) {
        lv_obj_set_style_bg_color(ui_WifiDot, wifiColor, 0);
        lv_obj_set_style_bg_opa(ui_WifiDot, wifiOpa, 0);
    }
    setStatusLabelColor(ui_LabelTxt4G, cellularColor, cellularOpa);
    setStatusLabelColor(ui_LabelBleStatus,
                        AppIdfBleAircon::hasActiveConnection() ? ui_theme_color_focus() : inactiveColor,
                        AppIdfBleAircon::hasActiveConnection() ? LV_OPA_COVER : inactiveOpa);

    if (g_statusVolumeLabel != nullptr) {
        char volumeText[14];
        if (audio.started && audio.codecFound) {
            snprintf(volumeText, sizeof(volumeText), LV_SYMBOL_VOLUME_MAX " %u", static_cast<unsigned>(audio.volume));
        } else if (audio.started) {
            snprintf(volumeText, sizeof(volumeText), LV_SYMBOL_VOLUME_MAX " !");
        } else {
            snprintf(volumeText, sizeof(volumeText), LV_SYMBOL_VOLUME_MAX " --");
        }
        lv_label_set_text(g_statusVolumeLabel, volumeText);
    }
    setStatusLabelColor(g_statusVolumeLabel, ui_theme_is_light() ? lv_color_hex(0x465668) : lv_color_hex(0xE2E8F0),
                        LV_OPA_COVER);

    updateBatteryWidgets(sensors.battery);

    if (ui_PillTemp != nullptr) {
        char tempBuf[16];
        if (sensors.temperature.valid) {
            snprintf(tempBuf, sizeof(tempBuf), "%.0f\xc2\xb0""C", sensors.temperature.celsius);
        } else {
            snprintf(tempBuf, sizeof(tempBuf), "--\xc2\xb0""C");
        }
        setStatusPill(ui_PillTemp, ui_theme_color_warning(), tempBuf);
    }

    if (ui_PillOnline != nullptr) {
        // 直接以 network.* 判定:STA 拿到 IP / 断连都会立刻反映到 network.connected,
        // 而 transport.active 要等 1s 周期评估,挂上它会让药丸在重置后卡 3~5 秒才换成 AP 名字
        // (标题栏 WiFi 图标已先变灰,二者错位很明显)。
        const bool portalActive = network.portalActive && !network.connected;
        ui_MainScreen_set_pill_portal_mode(portalActive);

        if (portalActive) {
            const char* portalText = network.portalSsid;
            lv_color_t portalColor = ui_theme_color_warning();
            if (strcmp(network.portalHint, "portal_starting") == 0) {
                portalText = "AP 启动中";
            } else if (strcmp(network.portalHint, "client_connected") == 0) {
                portalText = "已连接,请配网";
            } else if (strcmp(network.portalHint, "connecting") == 0) {
                portalText = "WiFi 连接中";
            } else if (strcmp(network.portalHint, "low_memory") == 0 ||
                       strcmp(network.portalHint, "portal_http_error") == 0) {
                portalText = "配网失败";
                portalColor = ui_theme_color_error();
            }
            setStatusPill(ui_PillOnline, portalColor, portalText);
        } else {
            const char* onlineText = "离线";
            lv_color_t onlineColor = ui_theme_color_warning();
            if (transport.active == AppIdfTransport::ActiveTransport::WIFI || network.connected) {
                onlineText = "WiFi";
                onlineColor = ui_theme_color_success();
            } else if (transport.active == AppIdfTransport::ActiveTransport::PPP_4G) {
                onlineText = "4G";
                onlineColor = ui_theme_color_success();
            } else if (network.connecting || cellularConnecting) {
                onlineText = "连接中";
            }
            setStatusPill(ui_PillOnline, onlineColor, onlineText);
        }
    }
}

void refreshAiRecorderStateLocked() {
    if (lv_scr_act() != ui_AIScreen) {
        return;
    }

    const AppIdfRecorder::Snapshot recorder = AppIdfRecorder::snapshot();
    const bool recorderBusy = recorder.startPending || recorder.recording || recorder.uploading;
    if (!recorderBusy && !g_aiRecordingActive && !g_aiUploadPending) {
        return;
    }

    if (recorder.startPending || recorder.recording) {
        g_aiRecordingActive = true;
        g_aiUploadPending = true;
        showAiLocked(AI_STATE_LISTENING);
        return;
    }
    if (recorder.uploading) {
        g_aiRecordingActive = false;
        g_aiUploadPending = true;
        // EXECUTING 期间不能被 THINKING 覆盖：协议派发整段 uploading 仍为 true,
        // 但 UI 应保持执行中画面直到 executor 返回。
        if (!g_aiExecutingActive) {
            showAiLocked(AI_STATE_THINKING);
        }
        return;
    }

    g_aiRecordingActive = false;
    g_aiUploadPending = false;
    g_aiExecutingActive = false;
    g_aiAutoExitMs = lv_tick_get() + 1500;
    showAiLocked(recorder.lastUploadOk ? AI_STATE_SUCCESS : AI_STATE_ERROR);
}

void aiWaveTimerCallback(lv_timer_t*) {
    if (lv_scr_act() != ui_AIScreen) {
        return;
    }
    const AppIdfRecorder::Snapshot rec = AppIdfRecorder::snapshot();
    if (!rec.recording) {
        ui_AIScreen_update_wave(0);
        return;
    }
    // RMS → 0..100 映射：起步阈值 100（本底噪声），满量程 4100（说话峰值）
    const uint32_t rms = rec.lastRms;
    int32_t level;
    if (rms <= 100U) {
        level = 0;
    } else {
        level = static_cast<int32_t>((rms - 100U) * 100U / 4000U);
        if (level > 100) level = 100;
    }
    ui_AIScreen_update_wave(static_cast<uint8_t>(level));
}

void updateMainClockLocked() {
    if (ui_ClockLabel == nullptr || ui_DateLabel == nullptr) {
        return;
    }
    if (lv_scr_act() != ui_MainScreen) {
        return;
    }

    static const char* kWeekday[7] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};

    if (!AppIdfTime::isSynced()) {
        lv_label_set_text(ui_ClockLabel, "--:--");
        lv_label_set_text(ui_DateLabel, "--/--");
        return;
    }

    time_t now = 0;
    time(&now);
    struct tm tm = {};
    localtime_r(&now, &tm);

    char clockBuf[8];
    char dateBuf[24];
    strftime(clockBuf, sizeof(clockBuf), "%H:%M", &tm);
    snprintf(dateBuf, sizeof(dateBuf), "%02d/%02d %s",
             tm.tm_mon + 1, tm.tm_mday, kWeekday[tm.tm_wday]);

    lv_label_set_text(ui_ClockLabel, clockBuf);
    lv_label_set_text(ui_DateLabel, dateBuf);
}

void statusTimerCallback(lv_timer_t*) {
    refreshOtaUiLocked();
    if (lv_scr_act() == g_otaScreen && g_otaWasActive) {
        return;
    }
    updateStatusLocked();
    updateMainClockLocked();
    refreshAiRecorderStateLocked();
    if (g_aiAutoExitMs != 0 && lv_tick_get() > g_aiAutoExitMs && lv_scr_act() == ui_AIScreen && !g_aiUploadPending) {
        g_aiAutoExitMs = 0;
        showMainLocked();
    }
    if (lv_scr_act() == g_settingsScreen) {
        updateSettingsSummaryLocked();
        if (g_settingsPopupVisible) {
            refreshSettingsPopupLocked();
        }
    }
    if (lv_scr_act() == g_blePairScreen) {
        refreshBlePairUiLocked();
    }
}

void loadRebuiltScreenLocked(lv_obj_t* screen) {
    if (screen == nullptr) {
        return;
    }
    lv_scr_load(screen);
}

void rebuildThemeScreensLocked(bool stayOnSettings) {
    lv_obj_t* oldMain = ui_MainScreen;
    lv_obj_t* oldSettings = g_settingsScreen;
    lv_obj_t* oldSettingsPopup = g_settingsPopupMask;
    lv_obj_t* oldBlePair = g_blePairScreen;
    lv_obj_t* oldOta = g_otaScreen;

    g_statusVolumeLabel = nullptr;
    g_settingsScreen = nullptr;
    g_settingsPopupMask = nullptr;
    g_settingsPopupCard = nullptr;
    g_settingsPopupTitle = nullptr;
    g_settingsPopupList = nullptr;
    g_settingsPopupVisible = false;
    g_settingsPopupMode = SettingsPopupMode::None;
    g_blePairScreen = nullptr;
    g_blePairStatusLabel = nullptr;
    g_blePairPopupMask = nullptr;
    g_blePairPopupTitle = nullptr;
    g_blePairPopupDetail = nullptr;
    g_otaScreen = nullptr;
    g_otaTitleLabel = nullptr;
    g_otaStatusLabel = nullptr;
    g_otaBar = nullptr;
    g_otaPercentLabel = nullptr;
    g_otaDetailLabel = nullptr;
    for (int i = 0; i < kSettingsItemCount; ++i) {
        g_settingsItems[i] = nullptr;
        g_settingsValueLabels[i] = nullptr;
    }
    for (int i = 0; i < kSettingsPopupRowCount; ++i) {
        g_settingsPopupRows[i] = nullptr;
        g_settingsPopupRowLabels[i] = nullptr;
    }
    for (int i = 0; i < kBlePairRowCount; ++i) {
        g_blePairRows[i] = nullptr;
        g_blePairNameLabels[i] = nullptr;
    }

    resetMainGroup();
    ui_theme_init_global_styles();
    ui_theme_apply_display_theme();

    ui_MainScreen_screen_init();
    createSettingsScreenLocked();
    createBlePairScreenLocked();
    createOtaScreenLocked();

    if (g_otaWasActive && g_otaScreen != nullptr) {
        loadRebuiltScreenLocked(g_otaScreen);
        g_activeScreenName = "ota";
        refreshOtaUiLocked();
    } else if (stayOnSettings && g_settingsScreen != nullptr) {
        loadRebuiltScreenLocked(g_settingsScreen);
        g_activeScreenName = "settings";
        focusSettingsIndexLocked(g_settingsFocusIndex);
    } else {
        loadRebuiltScreenLocked(ui_MainScreen);
        g_activeScreenName = "main";
        focusMainIndex(g_mainFocusIndex);
        updateStatusLocked();
    }

    if (oldMain != nullptr && oldMain != ui_MainScreen) {
        lv_obj_del(oldMain);
    }
    if (oldSettings != nullptr && oldSettings != g_settingsScreen) {
        lv_obj_del(oldSettings);
    } else if (oldSettingsPopup != nullptr && oldSettingsPopup != g_settingsPopupMask) {
        lv_obj_del(oldSettingsPopup);
    }
    if (oldBlePair != nullptr && oldBlePair != g_blePairScreen) {
        lv_obj_del(oldBlePair);
    }
    if (oldOta != nullptr && oldOta != g_otaScreen) {
        lv_obj_del(oldOta);
    }
}

esp_err_t applyThemePreferenceLocked(UiThemeMode mode, bool persist, bool stayOnSettings) {
    g_currentTheme = mode == UI_THEME_LIGHT ? UI_THEME_LIGHT : UI_THEME_DARK;
    ui_theme_set(g_currentTheme);
    rebuildThemeScreensLocked(stayOnSettings);

    if (persist) {
        const esp_err_t err = saveThemePreference(g_currentTheme);
        if (err != ESP_OK) {
            return err;
        }
    }

    LOG_I(TAG_UI_IDF, "UI theme set to %s", themeNameAscii(g_currentTheme));
    return ESP_OK;
}

void showMainLocked(bool animated) {
    if (g_settingsPopupVisible) {
        closeSettingsPopupLocked();
    }

    if (lv_scr_act() == g_blePairScreen) {
        const AppIdfBleAircon::PairingState state = AppIdfBleAircon::getPairingState();
        if (state == AppIdfBleAircon::PairingState::Scanning || state == AppIdfBleAircon::PairingState::Pairing) {
            AppIdfBleAircon::cancelPairing();
        }
    }

    if (lv_scr_act() == g_otaScreen && g_otaWasActive) {
        return;
    }

    if (ui_MainScreen == nullptr) {
        ui_MainScreen_screen_init();
        g_mainGroupReady = false;
    }
    applyMainModeVisibilityLocked();

    if (lv_scr_act() == ui_AIScreen) {
        ui_AIScreen_stop_anim();
        if (g_aiRecordingActive) {
            AppIdfRecorder::cancelRecording();
            g_aiRecordingActive = false;
        }
        g_aiUploadPending = false;
        g_aiAutoExitMs = 0;
    }

    if (animated) {
        lv_scr_load_anim(ui_MainScreen, LV_SCR_LOAD_ANIM_FADE_ON, 180, 0, false);
    } else {
        lv_scr_load(ui_MainScreen);
    }
    g_activeScreenName = "main";
    focusMainIndex(g_mainFocusIndex);
    updateStatusLocked();
}

void showSettingsLocked() {
    createSettingsScreenLocked();
    lv_scr_load_anim(g_settingsScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 180, 0, false);
    g_activeScreenName = "settings";
    focusSettingsIndexLocked(g_settingsFocusIndex);
    updateSettingsSummaryLocked();
}

void showBlePairLocked() {
    createBlePairScreenLocked();
    g_blePairSelectedIndex = 0;
    g_blePairWindowStart = 0;
    g_lastBlePairState = -1;
    g_blePairPopupUntilMs = 0;
    g_blePairAutoExitMs = 0;
    AppIdfBleAircon::startPairingScan();

    if (lv_scr_act() == ui_AIScreen) {
        ui_AIScreen_stop_anim();
    }
    lv_scr_load_anim(g_blePairScreen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 180, 0, false);
    g_activeScreenName = "ble_pair";
    refreshBlePairUiLocked();
}

void showAiLocked(ai_state_t state) {
    if (ui_AIScreen == nullptr) {
        ui_AIScreen_screen_init();
    }

    if (lv_scr_act() != ui_AIScreen) {
        lv_scr_load_anim(ui_AIScreen, LV_SCR_LOAD_ANIM_FADE_ON, 180, 0, false);
        ui_AIScreen_start_anim();
    }

    ui_AIScreen_set_state(state);
    ui_AIScreen_set_status_text(aiStateLabel(state));
    g_activeScreenName = "ai";
}

void executeMainSelectionLocked() {
    if (!isMainItemVisible(g_mainFocusIndex)) {
        return;
    }
    // 语音项 (index 0) 短按不响应：必须长按 KEY2 才进入录音流程，松开上传。
    if (g_mainFocusIndex == 1) {
        showBlePairLocked();
    } else if (g_mainFocusIndex == 2) {
        showSceneScreenLocked();
    } else if (g_mainFocusIndex == 3) {
        showSettingsLocked();
    }
}

void confirmSettingsSelectionLocked() {
    const SettingsItem item = static_cast<SettingsItem>(g_settingsFocusIndex);
    if (item == SettingsItem::Volume) {
        openSettingsPopupLocked(SettingsPopupMode::Volume);
        return;
    }
    if (item == SettingsItem::Network) {
        openSettingsPopupLocked(SettingsPopupMode::Network);
        return;
    }
    if (item == SettingsItem::WifiReset) {
        openSettingsPopupLocked(SettingsPopupMode::WifiReset);
        return;
    }
    if (item == SettingsItem::Theme) {
        openSettingsPopupLocked(SettingsPopupMode::Theme);
        return;
    }
    if (item == SettingsItem::Mode) {
        openSettingsPopupLocked(SettingsPopupMode::Mode);
        return;
    }

    updateSettingsSummaryLocked();
}

void startLocked(void*) {
    loadThemePreference();
    if (ui_theme_get() != g_currentTheme) {
        ui_theme_set(g_currentTheme);
        rebuildThemeScreensLocked(false);
    }

    ensureMainGroup();
    createSettingsScreenLocked();
    showMainLocked(false);
    ui_MainScreen_apply_focus_instant(g_mainFocusIndex);
    if (g_statusTimer == nullptr) {
        g_statusTimer = lv_timer_create(statusTimerCallback, 1000, nullptr);
    }
    if (g_aiWaveTimer == nullptr) {
        g_aiWaveTimer = lv_timer_create(aiWaveTimerCallback, 50, nullptr);
    }
    updateStatusLocked();
}

struct PowerSaveTimerOp {
    bool slow;
};

void applyPowerSaveTimersLocked(void* userData) {
    if (userData == nullptr) {
        return;
    }
    const PowerSaveTimerOp* op = static_cast<const PowerSaveTimerOp*>(userData);
    if (op->slow) {
        if (g_statusTimer != nullptr) {
            lv_timer_set_period(g_statusTimer, 5000);
        }
        if (g_aiWaveTimer != nullptr) {
            lv_timer_pause(g_aiWaveTimer);
        }
        AppIdfDisplay::setBacklight(false);
    } else {
        if (g_statusTimer != nullptr) {
            lv_timer_set_period(g_statusTimer, 1000);
        }
        if (g_aiWaveTimer != nullptr) {
            lv_timer_resume(g_aiWaveTimer);
        }
        lv_obj_t* active = lv_scr_act();
        if (active != nullptr) {
            lv_obj_invalidate(active);
        }
    }
}

void handleKeyLocked(void* userData) {
    if (userData == nullptr) {
        return;
    }

    const AppIdfInput::KeyEvent& event = *static_cast<const AppIdfInput::KeyEvent*>(userData);
    ++g_handledKeyCount;

    const bool onMain = lv_scr_act() == ui_MainScreen;
    const bool onAi = lv_scr_act() == ui_AIScreen;
    const bool onSettings = lv_scr_act() == g_settingsScreen;
    const bool onBlePair = lv_scr_act() == g_blePairScreen;
    const bool onScene = (g_sceneScreen != nullptr) && (lv_scr_act() == g_sceneScreen);
    const bool onLearn = (g_learnScreen != nullptr) && (lv_scr_act() == g_learnScreen);
    const bool onOta = lv_scr_act() == g_otaScreen;

    if (onOta && g_otaWasActive) {
        refreshOtaUiLocked();
        return;
    }

    if (event.action == AppIdfInput::KeyAction::LongPressStart && event.keyId == AppIdfInput::KeyId::Key1) {
        // 学习中长按 KEY1 取消并退回场景屏；其它情况照旧回主屏。
        if (AppIdfLearnFlow::isActive()) {
            AppIdfLearnFlow::cancel();
            hideLearnScreenLocked();
            showSceneScreenLocked();
            return;
        }
        showMainLocked();
        return;
    }

    // 场景屏长按 KEY2：
    //   - IR / RF433 模式：删除当前焦点条目（不再进学习；学习走语音入口）
    //   - BLE 模式：ROM 预制不可删，播 op_failed 提示
    if (onScene && event.action == AppIdfInput::KeyAction::LongPressStart &&
        event.keyId == AppIdfInput::KeyId::Key2) {
        if (AppIdfAppMode::isBle()) {
            AppIdfAudio::playLocalCue("op_failed", 4000);
        } else {
            removeFocusedSceneLocked();
        }
        return;
    }

    if (event.action == AppIdfInput::KeyAction::LongPressStart && event.keyId == AppIdfInput::KeyId::Key2) {
        const esp_err_t err = AppIdfRecorder::startRecording();
        if (err == ESP_OK) {
            g_aiStateIndex = 1;
            g_aiRecordingActive = true;
            g_aiUploadPending = false;
            g_aiAutoExitMs = 0;
            showAiLocked(AI_STATE_LISTENING);
        } else {
            LOG_W(TAG_UI_IDF, "AI recording start failed: %s", esp_err_to_name(err));
            g_aiRecordingActive = false;
            g_aiUploadPending = false;
            g_aiAutoExitMs = lv_tick_get() + 1500;
            showAiLocked(AI_STATE_ERROR);
        }
        return;
    }

    if (event.action == AppIdfInput::KeyAction::LongPressEnd && event.keyId == AppIdfInput::KeyId::Key2 && onAi) {
        if (!g_aiRecordingActive) {
            return;
        }
        const esp_err_t err = AppIdfRecorder::stopRecordingAndUpload();
        g_aiRecordingActive = false;
        if (err == ESP_OK) {
            g_aiUploadPending = true;
            showAiLocked(AI_STATE_THINKING);
        } else {
            LOG_W(TAG_UI_IDF, "AI recording stop/upload failed: %s", esp_err_to_name(err));
            g_aiUploadPending = false;
            g_aiAutoExitMs = lv_tick_get() + 1500;
            showAiLocked(AI_STATE_ERROR);
        }
        return;
    }

    if (event.action != AppIdfInput::KeyAction::ShortPress) {
        return;
    }

    if (event.keyId == AppIdfInput::KeyId::Key1) {
        if (onMain) {
            focusMainIndex(g_mainFocusIndex + 1);
        } else if (onSettings && g_settingsPopupVisible) {
            focusNextSettingsPopupItemLocked();
        } else if (onSettings) {
            focusSettingsIndexLocked(g_settingsFocusIndex + 1);
        } else if (onScene) {
            focusSceneIndexLocked(g_sceneFocusIndex + 1);
        } else if (onLearn) {
            // 学习屏短按忽略，避免误触跳出。
            return;
        } else if (onBlePair) {
            focusNextBlePairItemLocked();
        } else if (onAi) {
            ++g_aiStateIndex;
            showAiLocked(aiStateAt(g_aiStateIndex));
        } else {
            showMainLocked();
        }
    } else if (event.keyId == AppIdfInput::KeyId::Key2) {
        if (onMain) {
            executeMainSelectionLocked();
        } else if (onSettings && g_settingsPopupVisible) {
            confirmSettingsPopupSelectionLocked();
        } else if (onSettings) {
            confirmSettingsSelectionLocked();
        } else if (onScene) {
            executeFocusedSceneLocked();
        } else if (onLearn) {
            // 学习屏短按忽略；用户应使用长按 KEY2 录音。
            return;
        } else if (onBlePair) {
            confirmBlePairSelectionLocked();
        } else {
            showMainLocked();
        }
    }
}

}  // namespace

void preloadThemePreference() {
    loadThemePreference();
    ui_theme_set(g_currentTheme);
}

esp_err_t start() {
    if (g_started) {
        return ESP_OK;
    }
    if (!AppIdfLvgl::isStarted()) {
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t err = AppIdfLvgl::runLocked(startLocked, nullptr, 500);
    if (err != ESP_OK) {
        LOG_E(TAG_UI_IDF, "IDF UI bridge start failed: %s", esp_err_to_name(err));
        return err;
    }

    g_started = true;
    LOG_I(TAG_UI_IDF, "IDF UI bridge started");
    return ESP_OK;
}

bool isStarted() {
    return g_started;
}

void handleKeyEvent(const AppIdfInput::KeyEvent& event) {
    if (!g_started) {
        return;
    }

    const esp_err_t err = AppIdfLvgl::runLocked(handleKeyLocked, const_cast<AppIdfInput::KeyEvent*>(&event), 100);
    if (err != ESP_OK) {
        LOG_W(TAG_UI_IDF,
              "Failed to handle key action=%s key=%s: %s",
              AppIdfInput::keyActionName(event.action),
              AppIdfInput::keyIdName(event.keyId),
              esp_err_to_name(err));
    }
}

esp_err_t applyPowerSaveEnter() {
    PowerSaveTimerOp op = {true};
    return AppIdfLvgl::runLocked(applyPowerSaveTimersLocked, &op, 200);
}

esp_err_t applyPowerSaveExit() {
    PowerSaveTimerOp op = {false};
    return AppIdfLvgl::runLocked(applyPowerSaveTimersLocked, &op, 200);
}

const char* currentThemeName() {
    return themeNameAscii(g_currentTheme);
}

struct AiStatusRequest {
    AiStatus status = AiStatus::Ack;
    bool trackRecorder = false;
    uint32_t autoExitDelayMs = 0;
};

void showAiStatusLocked(void* userData) {
    if (userData == nullptr) {
        return;
    }
    AiStatusRequest* request = static_cast<AiStatusRequest*>(userData);
    g_aiRecordingActive = false;
    g_aiUploadPending = request->trackRecorder;
    g_aiAutoExitMs = request->autoExitDelayMs > 0 ? lv_tick_get() + request->autoExitDelayMs : 0;
    // 维护 EXECUTING sticky flag：进入 EXECUTING 时置位，进入其它已知状态时清除
    // (THINKING 不动它，因为录音轮询会反复短暂经过 THINKING 不应误清)
    if (request->status == AiStatus::Executing) {
        g_aiExecutingActive = true;
    } else if (request->status == AiStatus::Listening || request->status == AiStatus::Ack ||
               request->status == AiStatus::Speaking || request->status == AiStatus::Success ||
               request->status == AiStatus::Error) {
        g_aiExecutingActive = false;
    }
    showAiLocked(mapAiStatus(request->status));
}

struct ThemeRequest {
    UiThemeMode mode = UI_THEME_DARK;
    bool persist = false;
    esp_err_t result = ESP_OK;
};

void setThemeLocked(void* userData) {
    if (userData == nullptr) {
        return;
    }
    ThemeRequest* request = static_cast<ThemeRequest*>(userData);
    request->result = applyThemePreferenceLocked(request->mode, request->persist, lv_scr_act() == g_settingsScreen);
}

esp_err_t setThemeLight(bool light, bool persist) {
    if (!g_started) {
        return ESP_ERR_INVALID_STATE;
    }

    ThemeRequest request;
    request.mode = light ? UI_THEME_LIGHT : UI_THEME_DARK;
    request.persist = persist;
    const esp_err_t err = AppIdfLvgl::runLocked(setThemeLocked, &request, 1000);
    if (err != ESP_OK) {
        return err;
    }
    return request.result;
}

const char* activeScreenName() {
    return g_activeScreenName;
}

uint32_t handledKeyCount() {
    return g_handledKeyCount;
}

esp_err_t showAiStatus(AiStatus status, bool trackRecorder, uint32_t autoExitDelayMs) {
    if (!g_started) {
        return ESP_ERR_INVALID_STATE;
    }

    AiStatusRequest request;
    request.status = status;
    request.trackRecorder = trackRecorder;
    request.autoExitDelayMs = autoExitDelayMs;
    return AppIdfLvgl::runLocked(showAiStatusLocked, &request, 250);
}

namespace {
void showLearningScreenLockedFn(void*) {
    showLearnScreenLocked();
}
}

esp_err_t showLearningScreen() {
    if (!g_started) {
        return ESP_ERR_INVALID_STATE;
    }
    return AppIdfLvgl::runLocked(showLearningScreenLockedFn, nullptr, 250);
}

namespace {

lv_obj_t* g_lowBatteryPopupMask = nullptr;
lv_obj_t* g_lowBatteryPopupCard = nullptr;
lv_obj_t* g_lowBatteryTitleLabel = nullptr;
lv_obj_t* g_lowBatterySecondsLabel = nullptr;
lv_obj_t* g_lowBatteryHintLabel = nullptr;

struct LowBatteryRequest {
    int seconds;
    bool show;
};

void createLowBatteryPopupLockedInternal() {
    if (g_lowBatteryPopupMask != nullptr) {
        return;
    }
    // 挂在 lv_layer_top()，盖住所有 active screen 内容
    lv_obj_t* topLayer = lv_layer_top();
    g_lowBatteryPopupMask = lv_obj_create(topLayer);
    lv_obj_remove_style_all(g_lowBatteryPopupMask);
    lv_obj_set_size(g_lowBatteryPopupMask, 240, 240);
    lv_obj_align(g_lowBatteryPopupMask, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(g_lowBatteryPopupMask, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(g_lowBatteryPopupMask, 200, 0);
    lv_obj_clear_flag(g_lowBatteryPopupMask, LV_OBJ_FLAG_SCROLLABLE);
    // 吃掉所有按键事件
    lv_obj_add_flag(g_lowBatteryPopupMask, LV_OBJ_FLAG_CLICKABLE);

    g_lowBatteryPopupCard = lv_obj_create(g_lowBatteryPopupMask);
    lv_obj_set_size(g_lowBatteryPopupCard, 200, 160);
    lv_obj_center(g_lowBatteryPopupCard);
    lv_obj_set_style_bg_color(g_lowBatteryPopupCard, lv_color_hex(0x1F2937), 0);
    lv_obj_set_style_bg_opa(g_lowBatteryPopupCard, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(g_lowBatteryPopupCard, lv_color_hex(0xDC2626), 0);
    lv_obj_set_style_border_width(g_lowBatteryPopupCard, 2, 0);
    lv_obj_set_style_border_opa(g_lowBatteryPopupCard, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(g_lowBatteryPopupCard, 12, 0);
    lv_obj_clear_flag(g_lowBatteryPopupCard, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    g_lowBatteryTitleLabel = lv_label_create(g_lowBatteryPopupCard);
    lv_label_set_text(g_lowBatteryTitleLabel, "电量不足");
    lv_obj_set_style_text_color(g_lowBatteryTitleLabel, lv_color_hex(0xFCA5A5), 0);
    lv_obj_set_style_text_font(g_lowBatteryTitleLabel, &my_font_misans_20, 0);
    lv_obj_align(g_lowBatteryTitleLabel, LV_ALIGN_TOP_MID, 0, 14);

    g_lowBatterySecondsLabel = lv_label_create(g_lowBatteryPopupCard);
    lv_obj_set_style_text_color(g_lowBatterySecondsLabel, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(g_lowBatterySecondsLabel, &my_font_misans_20, 0);
    lv_obj_align(g_lowBatterySecondsLabel, LV_ALIGN_CENTER, 0, 0);

    g_lowBatteryHintLabel = lv_label_create(g_lowBatteryPopupCard);
    lv_label_set_text(g_lowBatteryHintLabel, "请立即接通充电器");
    lv_obj_set_style_text_color(g_lowBatteryHintLabel, lv_color_hex(0xD1D5DB), 0);
    lv_obj_set_style_text_font(g_lowBatteryHintLabel, &my_font_misans_20, 0);
    lv_obj_align(g_lowBatteryHintLabel, LV_ALIGN_BOTTOM_MID, 0, -14);
}

void updateLowBatteryPopupLockedInternal(int seconds) {
    if (g_lowBatterySecondsLabel == nullptr) {
        return;
    }
    char buf[32] = {};
    if (seconds <= 0) {
        snprintf(buf, sizeof(buf), "即将关机");
    } else {
        snprintf(buf, sizeof(buf), "%d 秒后关机", seconds);
    }
    lv_label_set_text(g_lowBatterySecondsLabel, buf);
    lv_obj_move_foreground(g_lowBatteryPopupMask);
}

void hideLowBatteryPopupLockedInternal() {
    if (g_lowBatteryPopupMask == nullptr) {
        return;
    }
    lv_obj_del(g_lowBatteryPopupMask);
    g_lowBatteryPopupMask = nullptr;
    g_lowBatteryPopupCard = nullptr;
    g_lowBatteryTitleLabel = nullptr;
    g_lowBatterySecondsLabel = nullptr;
    g_lowBatteryHintLabel = nullptr;
}

void lowBatteryPopupLockedCb(void* userData) {
    LowBatteryRequest* req = static_cast<LowBatteryRequest*>(userData);
    if (req == nullptr) {
        return;
    }
    if (req->show) {
        createLowBatteryPopupLockedInternal();
        updateLowBatteryPopupLockedInternal(req->seconds);
    } else {
        hideLowBatteryPopupLockedInternal();
    }
}

}  // namespace

esp_err_t showLowBatteryCountdown(int seconds) {
    if (!g_started) {
        return ESP_ERR_INVALID_STATE;
    }
    LowBatteryRequest request;
    request.seconds = seconds;
    request.show = true;
    return AppIdfLvgl::runLocked(lowBatteryPopupLockedCb, &request, 250);
}

void hideLowBatteryCountdown() {
    if (!g_started) {
        return;
    }
    LowBatteryRequest request;
    request.seconds = 0;
    request.show = false;
    (void)AppIdfLvgl::runLocked(lowBatteryPopupLockedCb, &request, 250);
}

}  // namespace AppIdfUi
