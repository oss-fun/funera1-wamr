#include <stdio.h>
#include <stdlib.h>

#include "../interpreter/wasm_runtime.h"
#include "wasm_migration.h"
#include "wasm_dump.h"

int all_cell_num_of_dummy_frame = -1;
void set_all_cell_num_of_dummy_frame(int all_cell_num) {
    all_cell_num_of_dummy_frame = all_cell_num;
}

int dump_value(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    if (stream == NULL) {
        return -1;
    }
    return fwrite(ptr, size, nmemb, stream);
}


static void
dump_WASMInterpFrame(struct WASMInterpFrame *frame, WASMExecEnv *exec_env, FILE *fp)
{
    if (fp == NULL) {
        perror("dump_WASMIntperFrame:fp is null\n");
        return;
    }
    int i;
    WASMModuleInstance *module_inst = exec_env->module_inst;

    // struct WASMInterpFrame *prev_frame;
    // struct WASMFunctionInstance *function;
    // uint8 *ip;
    uint32 ip_offset = frame->ip - wasm_get_func_code(frame->function);
    fwrite(&ip_offset, sizeof(uint32), 1, fp);

    // uint32 *sp_bottom;
    // uint32 *sp_boundary;
    // uint32 *sp;
    uint32 sp_offset = frame->sp - frame->sp_bottom;
    fwrite(&sp_offset, sizeof(uint32), 1, fp);

    // WASMBranchBlock *csp_bottom;
    // WASMBranchBlock *csp_boundary;
    // WASMBranchBlock *csp;
    uint32 csp_offset = frame->csp - frame->csp_bottom;
    fwrite(&csp_offset, sizeof(uint32), 1, fp);

    // uint32 lp[1];
    WASMFunctionInstance *func = frame->function;

    uint32 *lp = frame->lp;
    // VALUE_TYPE_I32
    // VALUE_TYPE_F32
    // VALUE_TYPE_I64
    // VALUE_TYPE_F64
    for (i = 0; i < func->param_count; i++) {
        switch (func->param_types[i]) {
            case VALUE_TYPE_I32:
            case VALUE_TYPE_F32:
                fwrite(lp, sizeof(uint32), 1, fp);
                lp++;
                break;
            case VALUE_TYPE_I64:
            case VALUE_TYPE_F64:
                fwrite(lp, sizeof(uint64), 1, fp);
                lp += 2;
                break;
            default:
                printf("TYPE NULL\n");
                break;
        }
    }
    for (i = 0; i < func->local_count; i++) {
        switch (func->local_types[i]) {
            case VALUE_TYPE_I32:
            case VALUE_TYPE_F32:
                fwrite(lp, sizeof(uint32), 1, fp);
                lp++;
                break;
            case VALUE_TYPE_I64:
            case VALUE_TYPE_F64:
                fwrite(lp, sizeof(uint64), 1, fp);
                lp += 2;
                break;
            default:
                printf("TYPE NULL\n");
                break;
        }
    }

    fwrite(frame->sp_bottom, sizeof(uint32), sp_offset, fp);

    WASMBranchBlock *csp = frame->csp_bottom;
    uint32 csp_num = frame->csp - frame->csp_bottom;
    

    for (i = 0; i < csp_num; i++, csp++) {
        // uint8 *begin_addr;
        uint64 addr;
        if (csp->begin_addr == NULL) {
            addr = -1;
            fwrite(&addr, sizeof(uint64), 1, fp);
        }
        else {
            addr = csp->begin_addr - wasm_get_func_code(frame->function);
            fwrite(&addr, sizeof(uint64), 1, fp);
        }

        // uint8 *target_addr;
        if (csp->target_addr == NULL) {
            addr = -1;
            fwrite(&addr, sizeof(uint64), 1, fp);
        }
        else {
            addr = csp->target_addr - wasm_get_func_code(frame->function);
            fwrite(&addr, sizeof(uint64), 1, fp);
        }

        // uint32 *frame_sp;
        if (csp->frame_sp == NULL) {
            addr = -1;
            fwrite(&addr, sizeof(uint64), 1, fp);
        }
        else {
            addr = csp->frame_sp - frame->sp_bottom;
            fwrite(&addr, sizeof(uint64), 1, fp);
        }

        // uint32 cell_num;
        fwrite(&csp->cell_num, sizeof(uint32), 1, fp);
    }
}

// WASMIntrerpFrameを逆から走査できるようにするもの
typedef struct RevFrame {
    WASMInterpFrame *frame;
    struct RevFrame *next;
} RevFrame;

RevFrame *init_rev_frame(WASMInterpFrame *top_frame) {
    WASMInterpFrame *frame = top_frame;
    RevFrame *rf, *next_rf;

    // top_rev_frame
    // TODO: freeする必要ある
    next_rf = (RevFrame*)malloc(sizeof(RevFrame));
    next_rf->frame = top_frame;
    next_rf->next = NULL;

    while(frame = frame->prev_frame) {
        rf = (RevFrame*)malloc(sizeof(RevFrame));
        rf->frame = frame;
        rf->next = next_rf;
        next_rf = rf;
    }
    
    return rf;
} 

RevFrame *walk_rev_frame(RevFrame *rf) {
    return rf->next;
}

int
wasm_dump_frame(WASMExecEnv *exec_env, struct WASMInterpFrame *frame)
{
    WASMModuleInstance *module =
        (WASMModuleInstance *)exec_env->module_inst;
    WASMFunctionInstance *function;

    FILE *fp;
    const char *file = "frame.img";
    fp = fopen(file, "wb");
    if (fp == NULL) {
        fprintf(stderr, "failed to open %s\n", file);
        return -1;
    }

    RevFrame *rf = init_rev_frame(frame);
    // frameをbottomからtopまで走査する
    do {
        WASMInterpFrame *frame = rf->frame;
        if (frame == NULL) {
            perror("wasm_dump_frame: frame is null\n");
            break;
        }

        if (frame->function == NULL) {
            // 初期フレーム
            uint32 func_idx = -1;
            fwrite(&func_idx, sizeof(uint32), 1, fp);
            fwrite(&all_cell_num_of_dummy_frame, sizeof(uint32), 1, fp);
        }
        else {
            uint32 func_idx = frame->function - module->e->functions;
            fwrite(&func_idx, sizeof(uint32), 1, fp);
            dump_WASMInterpFrame(frame, exec_env, fp);
        }
    } while(rf = rf->next);

    fclose(fp);
    return 0;
}

int wasm_dump_memory(WASMMemoryInstance *memory) {
    FILE *fp;
    const char *file = "memory.img";
    fp = fopen(file, "wb");
    if (fp == NULL) {
        fprintf(stderr, "failed to open %s\n", file);
        return -1;
    }

    // WASMMemoryInstance *memory = module->default_memory;
    fwrite(memory->memory_data, sizeof(uint8),
           memory->num_bytes_per_page * memory->cur_page_count, fp);

    fclose(fp);
}

int wasm_dump_global(WASMModuleInstance *module, WASMGlobalInstance *globals, uint8* global_data) {
    FILE *fp;
    const char *file = "global.img";
    fp = fopen(file, "wb");
    if (fp == NULL) {
        fprintf(stderr, "failed to open %s\n", file);
        return -1;
    }

    // WASMMemoryInstance *memory = module->default_memory;
    uint8 *global_addr;
    for (int i = 0; i < module->e->global_count; i++) {
        switch (globals[i].type) {
            case VALUE_TYPE_I32:
            case VALUE_TYPE_F32:
                global_addr = get_global_addr_for_migration(global_data, (globals+i));
                fwrite(global_addr, sizeof(uint32), 1, fp);
                break;
            case VALUE_TYPE_I64:
            case VALUE_TYPE_F64:
                global_addr = get_global_addr_for_migration(global_data, (globals+i));
                fwrite(global_addr, sizeof(uint64), 1, fp);
                break;
            default:
                printf("type error:B\n");
                break;
        }
    }

    fclose(fp);
    return 0;
}

int dump_value_investigate(void *ptr, size_t size, size_t nmemb, const char* file_name) {
    FILE *fp;
    fp = fopen(file_name, "wb");
    if (fp == NULL) {
        fprintf(stderr, "failed to open %s\n", file_name);
        return -1;
    }

    fwrite(ptr, size, nmemb, fp);
    
    return fclose(fp);
}

int wasm_dump_addrs(
        WASMInterpFrame *frame,
        WASMFunctionInstance *func,
        WASMMemoryInstance *memory,
        uint8 *frame_ip,
        uint32 *frame_sp,
        WASMBranchBlock *frame_csp,
        uint8 *frame_ip_end,
        uint8 *else_addr,
        uint8 *end_addr,
        uint8 *maddr,
        bool done_flag) 
{
    FILE *fp;
    const char *file = "addr.img";
    fp = fopen(file, "wb");
    if (fp == NULL) {
        fprintf(stderr, "failed to open %s\n", file);
        return -1;
    }

    uint32 p_offset;
    // register uint8 *frame_ip = &opcode_IMPDEP;
    p_offset = frame_ip - wasm_get_func_code(func);
    dump_value(&p_offset, sizeof(uint32), 1, fp);
    // register uint32 *frame_lp = NULL;
    // register uint32 *frame_sp = NULL;
    p_offset = frame_sp - frame->sp_bottom;
    dump_value(&p_offset, sizeof(uint32), 1, fp);
    // dump_value_investigate(frame->sp_bottom, sizeof(uint32), p_offset, "sp.img");

    // WASMBranchBlock *frame_csp = NULL;
    p_offset = frame_csp - frame->csp_bottom;
    dump_value(&p_offset, sizeof(uint32), 1, fp);

    p_offset = else_addr - wasm_get_func_code(func);
    dump_value(&p_offset, sizeof(uint32), 1, fp);

    p_offset = end_addr - wasm_get_func_code(func);
    dump_value(&p_offset, sizeof(uint32), 1, fp);

    p_offset = maddr - memory->memory_data;
    dump_value(&p_offset, sizeof(uint32), 1, fp);

    dump_value(&done_flag, sizeof(done_flag), 1, fp);

    fclose(fp);
    return 0;
}

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
         bool done_flag) 
{
    int rc;

    // dump linear memory
    rc = wasm_dump_memory(memory);
    if (rc < 0) {
        return rc;
    }
    printf("Success to dump linear memory\n");

    // dump globals
    rc = wasm_dump_global(module, globals, global_data);
    if (rc < 0) {
        return rc;
    }
    printf("Success to dump globals\n");
    
    // dump frame
    rc = wasm_dump_frame(exec_env, frame);
    if (rc < 0) {
        return rc;
    }
    printf("Success to dump frame\n");

    // dump addrs
    rc = wasm_dump_addrs(frame, cur_func, memory, 
                    frame_ip, frame_sp, frame_csp, frame_ip_end,
                    else_addr, end_addr, maddr, done_flag);
    if (rc < 0) {
        return rc;
    }
    printf("Success to dump addrs\n");

    return 0;
}
