#ifndef WASM_DUMP_H
#define WASM_DUMP_H

#include "../common/wasm_exec_env.h"
#include "../interpreter/wasm_interp.h"

#ifdef __cplusplus
extern "C" {
#endif

// チェックポイント制御
void wasm_set_checkpoint(bool f);
bool wasm_get_checkpoint();

// メインダンプ機能
int wasm_dump(WASMExecEnv *exec_env,
         WASMModuleInstance *module,
         WASMMemoryInstance *memory,
         WASMGlobalInstance *globals,
         uint8 *global_data,
         uint8 *global_addr,
         WASMFunctionInstance *cur_func,
         struct WASMInterpFrame *frame,
         register uint8 *frame_ip,
         register uint32 *frame_sp,
         WASMBranchBlock *frame_csp,
         uint8 *frame_ip_end,
         uint8 *else_addr,
         uint8 *end_addr,
         uint8 *maddr,
         bool done_flag);

// ダンプユーティリティ関数
uint8* get_type_stack(uint32 fidx, uint32 offset, uint32* type_stack_size, bool is_return_address);
int wasm_dump_memory(WASMMemoryInstance *memory);
int wasm_dump_global(WASMModuleInstance *module, WASMGlobalInstance *globals, uint8* global_data);
int wasm_dump_program_counter(WASMModuleInstance *module, WASMFunctionInstance *func, uint8 *frame_ip);
int wasm_dump_stack(WASMExecEnv *exec_env, struct WASMInterpFrame *frame);

// メモリページ関連の関数
int check_soft_dirty(int fd, uint8* addr);
int is_dirty(uint64 pagemap_entry);
int is_soft_dirty(uint64 pagemap_entry);
int get_pagemap(unsigned long memory_addr);
int dump_dirty_memory(WASMMemoryInstance *memory);

#ifdef __cplusplus
}
#endif

#endif /* WASM_DUMP_H */
