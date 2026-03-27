#ifndef _WASM_DUMP_H
#define _WASM_DUMP_H

#include "../common/wasm_exec_env.h"
#include "../interpreter/wasm_interp.h"

int wasm_print_program_counter(uint8 *frame_ip);

int wasm_dump(WASMExecEnv *exec_env,
         WASMModuleInstance *module,
         WASMMemoryInstance *memory,
         WASMGlobalInstance *globals,
         uint8 *global_data,
         WASMFunctionInstance *cur_func,
         struct WASMInterpFrame *frame,
         register uint8 *frame_ip);

int64_t get_time(struct timespec ts1, struct timespec ts2);

#endif // _WASM_CHECKPOINT_H
