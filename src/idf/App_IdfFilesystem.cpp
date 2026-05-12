#include "App_IdfFilesystem.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "App_Log.h"
#include "esp_littlefs.h"

namespace AppIdfFilesystem {
namespace {

constexpr const char* TAG_FS = "IDF_FS";
bool g_resourceMounted = false;
bool g_userDataMounted = false;

bool startsWith(const char* value, const char* prefix) {
    return strncmp(value, prefix, strlen(prefix)) == 0;
}

void logMountError(esp_err_t err) {
    if (err == ESP_FAIL) {
        LOG_W(TAG_FS, "LittleFS mount failed on partition '%s'", kResourcePartitionLabel);
    } else if (err == ESP_ERR_NOT_FOUND) {
        LOG_W(TAG_FS, "LittleFS partition '%s' not found", kResourcePartitionLabel);
    } else {
        LOG_W(TAG_FS, "LittleFS init failed on partition '%s': %s", kResourcePartitionLabel, esp_err_to_name(err));
    }
}

void logUserDataMountError(esp_err_t err) {
    if (err == ESP_FAIL) {
        LOG_W(TAG_FS, "LittleFS mount failed on partition '%s'", kUserDataPartitionLabel);
    } else if (err == ESP_ERR_NOT_FOUND) {
        LOG_W(TAG_FS, "LittleFS partition '%s' not found", kUserDataPartitionLabel);
    } else {
        LOG_W(TAG_FS, "LittleFS init failed on partition '%s': %s", kUserDataPartitionLabel,
              esp_err_to_name(err));
    }
}

}  // namespace

esp_err_t mountResourcePartition(bool readOnly) {
    if (isResourcePartitionMounted()) {
        return ESP_OK;
    }

    const esp_vfs_littlefs_conf_t config = {
        .base_path = kResourceBasePath,
        .partition_label = kResourcePartitionLabel,
        .partition = nullptr,
        .format_if_mount_failed = false,
        .read_only = static_cast<uint8_t>(readOnly ? 1 : 0),
        .dont_mount = false,
        .grow_on_mount = false,
    };

    const esp_err_t err = esp_vfs_littlefs_register(&config);
    if (err != ESP_OK) {
        g_resourceMounted = false;
        logMountError(err);
        return err;
    }

    g_resourceMounted = true;
    LOG_I(TAG_FS, "Mounted LittleFS resources at %s from partition '%s' readOnly=%d", kResourceBasePath,
          kResourcePartitionLabel, readOnly ? 1 : 0);
    logResourceInfo();
    return ESP_OK;
}

bool isResourcePartitionMounted() {
    return g_resourceMounted && esp_littlefs_mounted(kResourcePartitionLabel);
}

esp_err_t getResourceUsage(ResourceUsage* usage) {
    if (usage == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!isResourcePartitionMounted()) {
        return ESP_ERR_INVALID_STATE;
    }

    return esp_littlefs_info(kResourcePartitionLabel, &usage->totalBytes, &usage->usedBytes);
}

esp_err_t makeResourcePath(const char* resourcePath, char* outPath, size_t outPathSize) {
    if (resourcePath == nullptr || outPath == nullptr || outPathSize == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const int written = startsWith(resourcePath, kResourceBasePath)
                            ? snprintf(outPath, outPathSize, "%s", resourcePath)
                            : snprintf(outPath, outPathSize, "%s/%s", kResourceBasePath,
                                       resourcePath[0] == '/' ? resourcePath + 1 : resourcePath);
    if (written < 0 || static_cast<size_t>(written) >= outPathSize) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

bool resourceExists(const char* resourcePath) {
    if (!isResourcePartitionMounted()) {
        return false;
    }

    char fullPath[160] = {};
    if (makeResourcePath(resourcePath, fullPath, sizeof(fullPath)) != ESP_OK) {
        return false;
    }

    struct stat st = {};
    return stat(fullPath, &st) == 0;
}

void logResourceInfo() {
    if (!isResourcePartitionMounted()) {
        LOG_W(TAG_FS, "LittleFS resources are not mounted");
        return;
    }

    ResourceUsage usage;
    const esp_err_t usageErr = getResourceUsage(&usage);
    if (usageErr == ESP_OK) {
        LOG_I(TAG_FS, "LittleFS usage total=%u used=%u free=%u", static_cast<unsigned>(usage.totalBytes),
              static_cast<unsigned>(usage.usedBytes), static_cast<unsigned>(usage.totalBytes - usage.usedBytes));
    } else {
        LOG_W(TAG_FS, "LittleFS usage unavailable: %s", esp_err_to_name(usageErr));
    }

    LOG_I(TAG_FS, "LittleFS audio manifest present=%d", resourceExists("/audio_cues/manifest.json") ? 1 : 0);
}

esp_err_t mountUserDataPartition() {
    if (isUserDataPartitionMounted()) {
        return ESP_OK;
    }

    // userdata 分区不绑定 image，首次烧录后内容为 0xFF。format_if_mount_failed=true 让 littlefs
    // 在第一次 mount 失败时自动格化为合法 FS；后续启动数据已经存在则正常 mount。
    // 与 spiffs 资源分区不同：spiffs 由 littlefs_create_partition_image(... FLASH_IN_PROJECT)
    // 提供 image，所以 format_if_mount_failed 保持 false 即可；userdata 没 image 必须 true。
    const esp_vfs_littlefs_conf_t config = {
        .base_path = kUserDataBasePath,
        .partition_label = kUserDataPartitionLabel,
        .partition = nullptr,
        .format_if_mount_failed = true,
        .read_only = 0,
        .dont_mount = false,
        .grow_on_mount = false,
    };

    const esp_err_t err = esp_vfs_littlefs_register(&config);
    if (err != ESP_OK) {
        g_userDataMounted = false;
        logUserDataMountError(err);
        return err;
    }

    g_userDataMounted = true;
    LOG_I(TAG_FS, "Mounted LittleFS userdata at %s from partition '%s'", kUserDataBasePath,
          kUserDataPartitionLabel);

    size_t total = 0;
    size_t used = 0;
    const esp_err_t infoErr = esp_littlefs_info(kUserDataPartitionLabel, &total, &used);
    if (infoErr == ESP_OK) {
        LOG_I(TAG_FS, "userdata usage total=%u used=%u free=%u", static_cast<unsigned>(total),
              static_cast<unsigned>(used), static_cast<unsigned>(total - used));
    }
    return ESP_OK;
}

bool isUserDataPartitionMounted() {
    return g_userDataMounted && esp_littlefs_mounted(kUserDataPartitionLabel);
}

}  // namespace AppIdfFilesystem
