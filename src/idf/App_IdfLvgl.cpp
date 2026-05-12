#include "App_IdfLvgl.h"

#include <string.h>

#include "App_IdfDisplay.h"
#include "App_IdfSystem.h"
#include "App_IdfTasks.h"
#include "App_Log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "ui.h"

SemaphoreHandle_t xGuiSemaphore = nullptr;

namespace AppIdfLvgl {
namespace {

constexpr const char* TAG_LVGL = "IDF_LVGL";
constexpr uint32_t kLvglTaskStackWords = 4096;
constexpr int kDrawBufferLines = AppIdfDisplay::kScreenHeight;  // 全帧双缓冲，渲染与 SPI flush 并行
constexpr int kDmaBounceLines = 16;
constexpr int kDmaBounceCount = 2;  // ping-pong; SPI panel trans_queue_depth=4 留有余量
constexpr uint32_t kLvglTaskDelayMinMs = 5;
constexpr uint32_t kLvglTaskDelayMaxMs = 20;

lv_disp_draw_buf_t g_drawBuffer;
lv_disp_drv_t g_displayDriver;
lv_color_t* g_drawBuf1 = nullptr;
lv_color_t* g_drawBuf2 = nullptr;
uint16_t* g_dmaBounceBuffers[kDmaBounceCount] = {nullptr, nullptr};
size_t g_dmaBounceWriteIndex = 0;
size_t g_dmaInflightCount = 0;  // 已 queue 但未 fence 的块数（跨 flush 持久）
lv_disp_t* g_display = nullptr;
AppIdfTasks::StaticTaskMemory g_lvglTaskMemory;
bool g_started = false;
volatile bool g_backlightPending = false;

// === LVGL render/flush profiling ===
// 每 kProfWindowFrames 帧（isLast）自动打印一次窗口聚合，开销 ≈ 每帧若干次 esp_timer_get_time()。
struct ProfStats {
    int64_t windowStartUs = 0;
    uint32_t handlerCalls = 0;
    int64_t handlerTotalUs = 0;
    int64_t handlerMaxUs = 0;
    uint32_t flushCalls = 0;
    int64_t flushTotalUs = 0;
    int64_t flushMaxUs = 0;
    int64_t memcpyTotalUs = 0;
    int64_t drawcallTotalUs = 0;
    int64_t fenceTotalUs = 0;
    uint64_t pixelsTotal = 0;
    uint32_t frames = 0;
};
ProfStats g_prof;
constexpr uint32_t kProfWindowFrames = 20;
constexpr int64_t kProfWindowMaxUs = 3000000;  // 3s 兜底，静态屏也能定期 dump

size_t drawBufferBytes() {
    return static_cast<size_t>(AppIdfDisplay::kScreenWidth) * kDrawBufferLines * sizeof(lv_color_t);
}

size_t dmaBounceBytes() {
    return static_cast<size_t>(AppIdfDisplay::kScreenWidth) * kDmaBounceLines * sizeof(uint16_t);
}

uint32_t clampDelay(uint32_t delayMs) {
    if (delayMs < kLvglTaskDelayMinMs) {
        return kLvglTaskDelayMinMs;
    }
    if (delayMs > kLvglTaskDelayMaxMs) {
        return kLvglTaskDelayMaxMs;
    }
    return delayMs;
}

lv_obj_t* activeScreenLocked() {
    lv_disp_t* display = g_display != nullptr ? g_display : lv_disp_get_default();
    if (display == nullptr) {
        return nullptr;
    }
    return lv_disp_get_scr_act(display);
}

void dumpProfWindowLocked(int64_t nowUs) {
    if (g_prof.frames == 0 && g_prof.handlerCalls == 0) {
        g_prof.windowStartUs = nowUs;
        return;
    }
    const int64_t windowUs = nowUs - g_prof.windowStartUs;
    const float windowMs = static_cast<float>(windowUs) / 1000.0f;
    const float fps = (windowUs > 0) ? (g_prof.frames * 1000000.0f / static_cast<float>(windowUs)) : 0.0f;
    const float handlerAvg = g_prof.handlerCalls ? static_cast<float>(g_prof.handlerTotalUs) / g_prof.handlerCalls / 1000.0f : 0.0f;
    const float flushAvg = g_prof.flushCalls ? static_cast<float>(g_prof.flushTotalUs) / g_prof.flushCalls / 1000.0f : 0.0f;
    LOG_D(TAG_LVGL,
          "PROF frames=%u dur=%.0fms FPS=%.1f | handler n=%u avg=%.2fms max=%.2fms total=%.0fms | "
          "flush n=%u avg=%.2fms max=%.2fms total=%.0fms | memcpy=%.0fms drawcall=%.0fms fence=%.0fms | "
          "pixels=%llu",
          static_cast<unsigned>(g_prof.frames), windowMs, fps,
          static_cast<unsigned>(g_prof.handlerCalls), handlerAvg, g_prof.handlerMaxUs / 1000.0f,
          g_prof.handlerTotalUs / 1000.0f,
          static_cast<unsigned>(g_prof.flushCalls), flushAvg, g_prof.flushMaxUs / 1000.0f,
          g_prof.flushTotalUs / 1000.0f,
          g_prof.memcpyTotalUs / 1000.0f, g_prof.drawcallTotalUs / 1000.0f, g_prof.fenceTotalUs / 1000.0f,
          static_cast<unsigned long long>(g_prof.pixelsTotal));
    g_prof = ProfStats{};
    g_prof.windowStartUs = nowUs;
}

void flushDisplay(lv_disp_drv_t*, const lv_area_t* area, lv_color_t* colorP) {
    if (area == nullptr || colorP == nullptr || g_dmaBounceBuffers[0] == nullptr) {
        lv_disp_flush_ready(&g_displayDriver);
        return;
    }

    const int x1 = area->x1 < 0 ? 0 : area->x1;
    const int y1 = area->y1 < 0 ? 0 : area->y1;
    const int x2 = area->x2 >= AppIdfDisplay::kScreenWidth ? AppIdfDisplay::kScreenWidth - 1 : area->x2;
    const int y2 = area->y2 >= AppIdfDisplay::kScreenHeight ? AppIdfDisplay::kScreenHeight - 1 : area->y2;
    if (x1 > x2 || y1 > y2) {
        lv_disp_flush_ready(&g_displayDriver);
        return;
    }

    const int areaWidth = area->x2 - area->x1 + 1;
    const int copyWidth = x2 - x1 + 1;
    esp_err_t err = ESP_OK;

    const int64_t flushStartUs = esp_timer_get_time();
    int64_t memcpyAccUs = 0;
    int64_t drawcallAccUs = 0;
    int64_t fenceAccUs = 0;

    for (int y = y1; y <= y2 && err == ESP_OK; y += kDmaBounceLines) {
        const int lines = ((y + kDmaBounceLines - 1) <= y2) ? kDmaBounceLines : (y2 - y + 1);

        if (g_dmaInflightCount >= static_cast<size_t>(kDmaBounceCount)) {
            const int64_t fenceStart = esp_timer_get_time();
            err = AppIdfDisplay::waitForPendingTransfers();
            fenceAccUs += esp_timer_get_time() - fenceStart;
            g_dmaInflightCount = 0;
            if (err != ESP_OK) {
                break;
            }
        }

        uint16_t* bounce = g_dmaBounceBuffers[g_dmaBounceWriteIndex];
        const int64_t memcpyStart = esp_timer_get_time();
        for (int row = 0; row < lines; ++row) {
            const lv_color_t* src = colorP + (y + row - area->y1) * areaWidth + (x1 - area->x1);
            memcpy(bounce + row * copyWidth, src, static_cast<size_t>(copyWidth) * sizeof(lv_color_t));
        }
        memcpyAccUs += esp_timer_get_time() - memcpyStart;

        const int64_t drawStart = esp_timer_get_time();
        err = AppIdfDisplay::drawRgb565Bitmap(x1, y, x2 + 1, y + lines, bounce);
        drawcallAccUs += esp_timer_get_time() - drawStart;
        if (err == ESP_OK) {
            g_dmaBounceWriteIndex = (g_dmaBounceWriteIndex + 1) % kDmaBounceCount;
            ++g_dmaInflightCount;
        }
    }

    if (err != ESP_OK) {
        LOG_W(TAG_LVGL, "LCD flush failed: %s", esp_err_to_name(err));
    }

    const bool isLast = lv_disp_flush_is_last(&g_displayDriver);
    lv_disp_flush_ready(&g_displayDriver);

    const int64_t flushEndUs = esp_timer_get_time();
    const int64_t flushUs = flushEndUs - flushStartUs;
    const uint64_t pixels = static_cast<uint64_t>(copyWidth) * static_cast<uint64_t>(y2 - y1 + 1);
    ++g_prof.flushCalls;
    g_prof.flushTotalUs += flushUs;
    if (flushUs > g_prof.flushMaxUs) g_prof.flushMaxUs = flushUs;
    g_prof.memcpyTotalUs += memcpyAccUs;
    g_prof.drawcallTotalUs += drawcallAccUs;
    g_prof.fenceTotalUs += fenceAccUs;
    g_prof.pixelsTotal += pixels;
    if (isLast) {
        ++g_prof.frames;
        if (g_prof.frames >= kProfWindowFrames) {
            dumpProfWindowLocked(flushEndUs);
        }
    }

    if (isLast && g_backlightPending) {
        AppIdfDisplay::waitForPendingTransfers();
        g_dmaInflightCount = 0;
        g_backlightPending = false;
        AppIdfDisplay::setBacklight(true);
    }
}

esp_err_t allocateBuffers() {
    const size_t drawBytes = drawBufferBytes();
    g_drawBuf1 = static_cast<lv_color_t*>(heap_caps_malloc(drawBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    g_drawBuf2 = static_cast<lv_color_t*>(heap_caps_malloc(drawBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (g_drawBuf1 == nullptr || g_drawBuf2 == nullptr) {
        LOG_E(TAG_LVGL, "Failed to allocate LVGL draw buffers in PSRAM (%u bytes each)",
              static_cast<unsigned>(drawBytes));
        return ESP_ERR_NO_MEM;
    }

    const size_t bounceBytes = dmaBounceBytes();
    for (int i = 0; i < kDmaBounceCount; ++i) {
        g_dmaBounceBuffers[i] =
            static_cast<uint16_t*>(heap_caps_malloc(bounceBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
        if (g_dmaBounceBuffers[i] == nullptr) {
            LOG_E(TAG_LVGL,
                  "Failed to allocate LVGL internal DMA bounce buffer #%d (%u bytes)",
                  i, static_cast<unsigned>(bounceBytes));
            for (int j = 0; j < i; ++j) {
                heap_caps_free(g_dmaBounceBuffers[j]);
                g_dmaBounceBuffers[j] = nullptr;
            }
            return ESP_ERR_NO_MEM;
        }
    }
    g_dmaBounceWriteIndex = 0;
    g_dmaInflightCount = 0;

    return ESP_OK;
}

esp_err_t registerDisplayDriver() {
    lv_disp_draw_buf_init(&g_drawBuffer,
                          g_drawBuf1,
                          g_drawBuf2,
                          AppIdfDisplay::kScreenWidth * kDrawBufferLines);

    lv_disp_drv_init(&g_displayDriver);
    g_displayDriver.hor_res = AppIdfDisplay::kScreenWidth;
    g_displayDriver.ver_res = AppIdfDisplay::kScreenHeight;
    g_displayDriver.flush_cb = flushDisplay;
    g_displayDriver.draw_buf = &g_drawBuffer;
    g_display = lv_disp_drv_register(&g_displayDriver);
    if (g_display == nullptr) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

void lvglTask(void*) {
    AppIdfTasks::registerCurrentTaskWatchdog("IDF_LVGL");

    while (true) {
        uint32_t delayMs = kLvglTaskDelayMaxMs;
        if (xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (activeScreenLocked() != nullptr) {
                const int64_t handlerStartUs = esp_timer_get_time();
                delayMs = lv_timer_handler();
                const int64_t handlerEndUs = esp_timer_get_time();
                const int64_t handlerUs = handlerEndUs - handlerStartUs;
                ++g_prof.handlerCalls;
                g_prof.handlerTotalUs += handlerUs;
                if (handlerUs > g_prof.handlerMaxUs) g_prof.handlerMaxUs = handlerUs;
                if (g_prof.windowStartUs == 0) {
                    g_prof.windowStartUs = handlerEndUs;
                } else if (handlerEndUs - g_prof.windowStartUs >= kProfWindowMaxUs &&
                           g_prof.handlerCalls > 0) {
                    dumpProfWindowLocked(handlerEndUs);
                }
            }
            xSemaphoreGive(xGuiSemaphore);
        }

        AppIdfTasks::feedCurrentTaskWatchdog();
        vTaskDelay(pdMS_TO_TICKS(clampDelay(delayMs)));
    }
}

}  // namespace

esp_err_t start() {
    if (g_started) {
        return ESP_OK;
    }

    static_assert(sizeof(lv_color_t) == sizeof(uint16_t), "LVGL color depth must be RGB565");

    esp_err_t err = AppIdfDisplay::init();
    if (err != ESP_OK) {
        return err;
    }

    xGuiSemaphore = xSemaphoreCreateMutex();
    if (xGuiSemaphore == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    lv_init();

    err = allocateBuffers();
    if (err != ESP_OK) {
        return err;
    }

    err = registerDisplayDriver();
    if (err != ESP_OK) {
        LOG_E(TAG_LVGL, "LVGL display driver registration failed: %s", esp_err_to_name(err));
        return err;
    }

    if (xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    ui_init();
    if (activeScreenLocked() == nullptr) {
        xSemaphoreGive(xGuiSemaphore);
        LOG_E(TAG_LVGL, "LVGL UI init finished without an active screen");
        return ESP_FAIL;
    }
    xSemaphoreGive(xGuiSemaphore);

    err = AppIdfTasks::createPinnedToCoreInternal(lvglTask, "IDF_LVGL", kLvglTaskStackWords, nullptr, 3, 1,
                                                  &g_lvglTaskMemory);
    if (err != ESP_OK) {
        LOG_E(TAG_LVGL, "LVGL task creation failed: %s", esp_err_to_name(err));
        return err;
    }

    g_started = true;
    LOG_I(TAG_LVGL,
          "LVGL started, draw buffers=%u bytes x2 PSRAM, DMA bounce=%u bytes x%d internal (ping-pong)",
          static_cast<unsigned>(drawBufferBytes()),
          static_cast<unsigned>(dmaBounceBytes()),
          kDmaBounceCount);
    AppIdfSystem::logHeapSnapshot("lvgl-start");
    return ESP_OK;
}

bool isStarted() {
    return g_started;
}

uint32_t taskStackHighWatermark() {
    if (g_lvglTaskMemory.handle == nullptr) {
        return 0;
    }
    return uxTaskGetStackHighWaterMark(g_lvglTaskMemory.handle);
}

esp_err_t requestRefresh() {
    if (!g_started || xGuiSemaphore == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(500)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    lv_obj_t* activeScreen = lv_scr_act();
    if (activeScreen == nullptr) {
        xSemaphoreGive(xGuiSemaphore);
        return ESP_ERR_INVALID_STATE;
    }
    lv_obj_invalidate(activeScreen);
    lv_refr_now(nullptr);
    xSemaphoreGive(xGuiSemaphore);
    return ESP_OK;
}

esp_err_t runLocked(LockedCallback callback, void* userData, uint32_t timeoutMs) {
    if (callback == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!g_started || xGuiSemaphore == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(timeoutMs)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    callback(userData);
    xSemaphoreGive(xGuiSemaphore);
    return ESP_OK;
}

void enableBacklightAfterNextFrame() {
    g_backlightPending = true;
}


}  // namespace AppIdfLvgl
