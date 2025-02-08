#include "wasm_migration.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "wasm-migration";
static bool checkpoint_flag = false;
static bool restore_flag = false;
static SemaphoreHandle_t fs_semaphore = NULL;

void print_memory_info(void) {
    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "Free PSRAM: %d bytes", free_psram);
    ESP_LOGI(TAG, "Free Internal RAM: %d bytes", free_internal);
}

void wasm_runtime_checkpoint(void) {
    ESP_LOGI(TAG, "Checkpoint requested");
    wasm_set_checkpoint(true);
}

bool wasm_get_checkpoint(void) {
    return checkpoint_flag;
}

void wasm_set_checkpoint(bool f) {
    ESP_LOGI(TAG, "Setting checkpoint flag to: %d", f);
    checkpoint_flag = f;
}

void set_restore_flag(bool f) {
    ESP_LOGI(TAG, "Setting restore flag to: %d", f);
    restore_flag = f;
}

bool get_restore_flag(void) {
    ESP_LOGI(TAG, "get_restore_flag called, flag = %d", restore_flag);
    return restore_flag;
}

int64_t get_time(struct timespec ts1, struct timespec ts2) {
    int64_t sec = ts2.tv_sec - ts1.tv_sec;
    int64_t nsec = ts2.tv_nsec - ts1.tv_nsec;
    return sec * 1000000000 + nsec;
}

void init_restore_system(void) {
    fs_semaphore = xSemaphoreCreateMutex();
    if (!fs_semaphore) {
        ESP_LOGE(TAG, "Failed to create file mutex");
        return;
    }
    ESP_LOGI(TAG, "Restore system initialized");
}

// open_image の実装
FILE* open_image(const char* filename, const char* mode) {
    FILE* fp = fopen(filename, mode);
    if (!fp) {
        ESP_LOGE(TAG, "Failed to open file: %s", filename);
    }
    return fp;
}

// get_global_addr_for_migration の実装
uint8* get_global_addr_for_migration(uint8* global_data, const WASMGlobalInstance* global) {
    return global_data + global->data_offset;
}
