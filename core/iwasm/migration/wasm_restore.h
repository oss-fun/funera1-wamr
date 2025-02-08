#ifndef WASM_RESTORE_H
#define WASM_RESTORE_H

#include "../interpreter/wasm_runtime.h"
#include "wasm_migration.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"


extern SemaphoreHandle_t get_file_mutex(void);
#ifdef __cplusplus
extern "C" {
#endif

// wasm_interp_interp_frame_sizeの宣言は別のヘッダーファイルから提供されるので、ここでは宣言しない
// unsigned wasm_interp_interp_frame_size(unsigned all_cell_num);  // この行を削除

// ファイル操作関数の宣言
uint8_t* read_file_to_buffer(const char* filename, size_t* size_out);
FILE* safe_open_file(const char* filename);
void safe_close_file(FILE* fp);

// WASMInterpFrameの前方宣言
struct WASMInterpFrame;

// スタック操作関数の宣言
void _restore_stack(WASMExecEnv *exec_env, struct WASMInterpFrame *frame, FILE *fp);
struct WASMInterpFrame* wasm_restore_stack(WASMExecEnv **_exec_env);

// メモリ操作関数の宣言
int wasm_restore_memory(WASMModuleInstance *module, 
                       WASMMemoryInstance **memory, 
                       uint8** maddr);

// グローバル変数操作関数の宣言
int wasm_restore_global(const WASMModuleInstance *module,
                       const WASMGlobalInstance *globals,
                       uint8 **global_data,
                       uint8 **global_addr);

// プログラムカウンタ操作関数の宣言
int wasm_restore_program_counter(WASMModuleInstance *module,
                               uint8 **frame_ip);

// メインの復元関数の宣言
int wasm_restore(WASMModuleInstance **module,
                WASMExecEnv **exec_env,
                WASMFunctionInstance **cur_func,
                struct WASMInterpFrame **prev_frame,
                WASMMemoryInstance **memory,
                WASMGlobalInstance **globals,
                uint8 **global_data,
                uint8 **global_addr,
                struct WASMInterpFrame **frame,
                uint8 **frame_ip,
                uint32 **frame_lp,
                uint32 **frame_sp,
                WASMBranchBlock **frame_csp,
                uint8 **frame_ip_end,
                uint8 **else_addr,
                uint8 **end_addr,
                uint8 **maddr,
                bool *done_flag);

// フラグ操作関数の宣言
void set_restore_flag(bool f);
bool get_restore_flag(void);

// システム初期化関数の宣言
void init_restore_system(void);
void cleanup_restore_system(void);

#ifdef __cplusplus
}
#endif

#endif /* WASM_RESTORE_H */
