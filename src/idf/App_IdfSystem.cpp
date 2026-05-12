#include "App_IdfSystem.h"

#include <inttypes.h>

#include "App_Log.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_check.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "nvs_flash.h"

namespace {

constexpr const char* TAG_IDF = "IDF";

const char* partitionTypeName(esp_partition_type_t type) {
    switch (type) {
        case ESP_PARTITION_TYPE_APP:
            return "app";
        case ESP_PARTITION_TYPE_DATA:
            return "data";
        default:
            return "other";
    }
}

void logPartition(const esp_partition_t* partition, const char* label) {
    if (partition == nullptr) {
        LOG_W(TAG_IDF, "%s partition not found", label);
        return;
    }

    LOG_I(TAG_IDF,
          "%s partition: type=%s subtype=0x%02x offset=0x%06" PRIx32 " size=0x%06" PRIx32,
          label,
          partitionTypeName(partition->type),
          partition->subtype,
          partition->address,
          partition->size);
}

}  // namespace

namespace AppIdfSystem {

esp_err_t initNvs() {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG_IDF, "nvs_flash_erase failed");
        err = nvs_flash_init();
    }
    return err;
}

HeapSnapshot getHeapSnapshot() {
    HeapSnapshot snapshot;
    snapshot.internalFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    snapshot.internalLargest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    snapshot.internalMinimumFree = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    snapshot.psramFree = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    snapshot.psramLargest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    snapshot.psramMinimumFree = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
    return snapshot;
}

void logHeapSnapshot(const char* phase) {
    const HeapSnapshot snapshot = getHeapSnapshot();
    LOG_I(TAG_IDF,
          "[%s] internal_free=%u internal_largest=%u internal_min_free=%u psram_free=%u psram_largest=%u psram_min_free=%u",
          phase ? phase : "unknown",
          static_cast<unsigned>(snapshot.internalFree),
          static_cast<unsigned>(snapshot.internalLargest),
          static_cast<unsigned>(snapshot.internalMinimumFree),
          static_cast<unsigned>(snapshot.psramFree),
          static_cast<unsigned>(snapshot.psramLargest),
          static_cast<unsigned>(snapshot.psramMinimumFree));
}

void logAppDescription() {
    const esp_app_desc_t* desc = esp_app_get_description();
    if (desc == nullptr) {
        LOG_W(TAG_IDF, "esp_app_get_description returned null");
        return;
    }

    LOG_I(TAG_IDF, "app project=%s version=%s idf=%s", desc->project_name, desc->version, desc->idf_ver);
}

void logChipInfo() {
    esp_chip_info_t chip;
    esp_chip_info(&chip);

    uint32_t flashSize = 0;
    const esp_err_t flashErr = esp_flash_get_size(nullptr, &flashSize);

    LOG_I(TAG_IDF,
          "chip cores=%u revision=%u features=0x%08" PRIx32,
          static_cast<unsigned>(chip.cores),
          static_cast<unsigned>(chip.revision),
          static_cast<uint32_t>(chip.features));
    if (flashErr == ESP_OK) {
        LOG_I(TAG_IDF, "flash_size=%u bytes", static_cast<unsigned>(flashSize));
    } else {
        LOG_W(TAG_IDF, "esp_flash_get_size failed: %s", esp_err_to_name(flashErr));
    }
}

void logKeyPartitions() {
    const esp_partition_t* running = esp_ota_get_running_partition();
    const esp_partition_t* boot = esp_ota_get_boot_partition();
    const esp_partition_t* nextOta = esp_ota_get_next_update_partition(nullptr);
    const esp_partition_t* model =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "model");
    const esp_partition_t* filesystem =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "spiffs");

    logPartition(running, "running");
    logPartition(boot, "boot");
    logPartition(nextOta, "next_ota");
    logPartition(model, "model");
    logPartition(filesystem, "filesystem");

    if (model != nullptr) {
        uint8_t header[16] = {};
        const esp_err_t err = esp_partition_read(model, 0, header, sizeof(header));
        if (err == ESP_OK) {
            LOG_I(TAG_IDF,
                  "model header: %02x %02x %02x %02x %02x %02x %02x %02x",
                  header[0],
                  header[1],
                  header[2],
                  header[3],
                  header[4],
                  header[5],
                  header[6],
                  header[7]);
        } else {
            LOG_W(TAG_IDF, "failed to read model partition header: %s", esp_err_to_name(err));
        }
    }
}

void logTaskWatermark(TaskHandle_t taskHandle, const char* taskName) {
    if (taskHandle == nullptr) {
        LOG_W(TAG_IDF, "%s task not created", taskName ? taskName : "unknown");
        return;
    }

    LOG_I(TAG_IDF,
          "%s stack watermark=%u bytes",
          taskName ? taskName : "unknown",
          static_cast<unsigned>(uxTaskGetStackHighWaterMark(taskHandle)));
}

}  // namespace AppIdfSystem
