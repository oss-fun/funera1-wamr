#ifndef _WASM_RESTORE_H
#define _WASM_RESTORE_H

#include "../common/wasm_exec_env.h"
#include "../interpreter/wasm_interp.h"

void set_restore_flag(bool f);
bool get_restore_flag();

WASMInterpFrame*
wasm_restore_stack(WASMExecEnv **exec_env);

static inline void
debug_wasm_interp_frame(WASMInterpFrame *frame, WASMFunctionInstance* base_func_addr) {
    int cnt = 0;
    printf("===         dump frames         ===\n");
    do {
        cnt++;
        if (frame->function == NULL)
            printf("frame: %d,          func idx: DUMMY\n", cnt);
        else 
            printf("frame: %d          func idx: %d\n", cnt, frame->function-base_func_addr);
    } while(frame = frame->prev_frame);
    printf("===         dump frames         ===\n");
};

int wasm_restore(WASMModuleInstance **module,
            WASMExecEnv **exec_env,
            WASMFunctionInstance **cur_func,
            WASMInterpFrame **prev_frame,
            WASMMemoryInstance **memory,
            WASMGlobalInstance **globals,
            uint8 **global_data,
            uint8 **global_addr,
            WASMInterpFrame **frame,
            uint8 **frame_ip,
            uint32 **frame_lp,
            uint32 **frame_sp,
            WASMBranchBlock **frame_csp,
            uint8 **frame_ip_end,
            uint8 **else_addr,
            uint8 **end_addr,
            uint8 **maddr,
            bool *done_flag);
#endif // _WASM_RESTORE_H