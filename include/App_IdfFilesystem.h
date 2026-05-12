#pragma once

#include <stddef.h>

#include "esp_err.h"

namespace AppIdfFilesystem {

constexpr const char* kResourcePartitionLabel = "spiffs";
constexpr const char* kResourceBasePath = "/littlefs";

// 运行期用户数据分区：与 spiffs 资源分区分离，避免 idf.py flash 覆盖。
// 分区表 subtype = littlefs，mount 时 format_if_mount_failed = true：首次烧录后分区为 0xFF
// 必须自动格化为合法 littlefs，否则 scenes.json / ir_codes.json 无法落盘。
constexpr const char* kUserDataPartitionLabel = "userdata";
constexpr const char* kUserDataBasePath = "/userdata";

struct ResourceUsage {
    size_t totalBytes = 0;
    size_t usedBytes = 0;
};

esp_err_t mountResourcePartition(bool readOnly = true);
bool isResourcePartitionMounted();
esp_err_t getResourceUsage(ResourceUsage* usage);
esp_err_t makeResourcePath(const char* resourcePath, char* outPath, size_t outPathSize);
bool resourceExists(const char* resourcePath);
void logResourceInfo();

esp_err_t mountUserDataPartition();
bool isUserDataPartitionMounted();

}  // namespace AppIdfFilesystem
