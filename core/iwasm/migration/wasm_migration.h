#ifndef WASM_MIGRATION_H
#define WASM_MIGRATION_H

#include <stdint.h>
#include <time.h>
#include <stdbool.h>
#include "wasm_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

// ページサイズの定義を追加
#define WASM_PAGE_SIZE 4096


// 必要な関数宣言を追加
uint8* get_global_addr_for_migration(uint8* global_data, const WASMGlobalInstance* global);
FILE* open_image(const char* filename, const char* mode);

// その他の既存の宣言...
void wasm_runtime_checkpoint(void);
bool wasm_get_checkpoint(void);
void wasm_set_checkpoint(bool f);
int64_t get_time(struct timespec ts1, struct timespec ts2);
void print_memory_info(void);
void init_restore_system(void);
void set_restore_flag(bool f);
bool get_restore_flag(void);

#ifdef __cplusplus
}
#endif

#endif /* WASM_MIGRATION_H */
