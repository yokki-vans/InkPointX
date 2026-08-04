#include <esp_err.h>
#include <esp_partition.h>

#include <cstddef>

namespace {
// The preserved OEM bootloader may update OTA state, and this app marks a
// healthy image valid through the same partition. No other on-device flash
// mutation is required in the dedicated X4 Pro field build: books, settings,
// credentials and caches live on SD.
bool isAllowedRuntimeWrite(const esp_partition_t* partition) {
  return partition != nullptr && partition->type == ESP_PARTITION_TYPE_DATA &&
         partition->subtype == ESP_PARTITION_SUBTYPE_DATA_OTA;
}
}  // namespace

extern "C" esp_err_t __real_esp_partition_write(const esp_partition_t* partition, size_t dstOffset, const void* source,
                                                size_t size);
extern "C" esp_err_t __real_esp_partition_write_raw(const esp_partition_t* partition, size_t dstOffset,
                                                    const void* source, size_t size);
extern "C" esp_err_t __real_esp_partition_erase_range(const esp_partition_t* partition, size_t offset, size_t size);

extern "C" esp_err_t __wrap_esp_partition_write(const esp_partition_t* partition, size_t dstOffset, const void* source,
                                                size_t size) {
  if (!isAllowedRuntimeWrite(partition)) return ESP_ERR_NOT_ALLOWED;
  return __real_esp_partition_write(partition, dstOffset, source, size);
}

extern "C" esp_err_t __wrap_esp_partition_write_raw(const esp_partition_t* partition, size_t dstOffset,
                                                    const void* source, size_t size) {
  if (!isAllowedRuntimeWrite(partition)) return ESP_ERR_NOT_ALLOWED;
  return __real_esp_partition_write_raw(partition, dstOffset, source, size);
}

extern "C" esp_err_t __wrap_esp_partition_erase_range(const esp_partition_t* partition, size_t offset, size_t size) {
  if (!isAllowedRuntimeWrite(partition)) return ESP_ERR_NOT_ALLOWED;
  return __real_esp_partition_erase_range(partition, offset, size);
}
