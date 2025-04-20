#ifndef _WASM_DUMP_H
#define _WASM_DUMP_H

#include "../common/wasm_exec_env.h"
#include "../interpreter/wasm_interp.h"

void wasm_set_checkpoint(bool f);
bool wasm_get_checkpoint();

int wasm_dump(WASMExecEnv *exec_env,
         WASMModuleInstance *module,
         WASMMemoryInstance *memory,
         WASMGlobalInstance *globals,
         uint8 *global_data,
         WASMFunctionInstance *cur_func,
         struct WASMInterpFrame *frame,
         register uint8 *frame_ip);


#endif // _WASM_CHECKPOINT_H
