#ifndef _WASM_MIGRATION_H
#define _WASM_MIGRATION_H

// #include "../common/wasm_exec_env.h"
#include "../interpreter/wasm_interp.h"

static inline uint8 *
get_global_addr_for_migration(uint8 *global_data, WASMGlobalInstance *global)
{
#if WASM_ENABLE_MULTI_MODULE == 0
    return global_data + global->data_offset;
#else
    return global->import_global_inst
               ? global->import_module_inst->global_data
                     + global->import_global_inst->data_offset
               : global_data + global->data_offset;
#endif
}

static uint32
get_addr_offset(void* target, void* base)
{
    if (target == NULL) return -1;
    else return target - base;
}

static void*
set_addr_offset(void* base, uint32 offset)
{
    if (offset == -1) return NULL;
    else return base + offset;
}

static FILE* open_image(const char* file, const char* flag) {
    FILE *fp = fopen(file, flag);
    if (fp == NULL) {
        fprintf(stderr, "failed to open %s\n", file);
        return NULL;
    }
    return fp;
}

// int wasm_dump(WASMExecEnv *exec_env,
//          WASMModuleInstance *module,
//          WASMMemoryInstance *memory,
//          WASMGlobalInstance *globals,
//          uint8 *global_data,
//          uint8 *global_addr,
//          WASMFunctionInstance *cur_func,
//          struct WASMInterpFrame *frame,
//          register uint8 *frame_ip,
//          register uint32 *frame_sp,
//          WASMBranchBlock *frame_csp,
//          uint8 *frame_ip_end,
//          uint8 *else_addr,
//          uint8 *end_addr,
//          uint8 *maddr,
//          bool done_flag);

// int wasm_restore(WASMModuleInstance *module,
//             WASMExecEnv *exec_env,
//             WASMFunctionInstance *cur_func,
//             WASMInterpFrame *prev_frame,
//             WASMMemoryInstance *memory,
//             WASMGlobalInstance *globals,
//             uint8 *global_data,
//             uint8 *global_addr,
//             WASMInterpFrame *frame,
//             register uint8 *frame_ip,
//             register uint32 *frame_lp,
//             register uint32 *frame_sp,
//             WASMBranchBlock *frame_csp,
//             uint8 *frame_ip_end,
//             uint8 *else_addr,
//             uint8 *end_addr,
//             uint8 *maddr,
//             bool *done_flag);
#endif // _WASM_MIGRATION_H
