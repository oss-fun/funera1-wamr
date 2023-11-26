#include <stdio.h>
#include <stdlib.h>

#include "../interpreter/wasm_runtime.h"
#include "wasm_migration.h"
#include "wasm_dump.h"
#include "wasm_dispatch.h"

// #define skip_leb(p) while (*p++ & 0x80)
#define skip_leb(p)                     \
    while (1) {                         \
        if (*p & 0x80)p++;              \
        else break;                     \
    }                                   \

/* common_functions */
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

int debug_memories(WASMModuleInstance *module) {
    printf("=== debug memories ===\n");
    printf("memory_count: %d\n", module->memory_count);
    
    // bytes_per_page
    for (int i = 0; i < module->memory_count; i++) {
        WASMMemoryInstance *memory = (WASMMemoryInstance *)(module->memories[i]);
        printf("%d) bytes_per_page: %d\n", i, memory->num_bytes_per_page);
        printf("%d) cur_page_count: %d\n", i, memory->cur_page_count);
        printf("%d) max_page_count: %d\n", i, memory->max_page_count);
        printf("\n");
    }

    printf("=== debug memories ===\n");
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

    int cnt = 0;
    while(frame = frame->prev_frame) {
        cnt++;
        rf = (RevFrame*)malloc(sizeof(RevFrame));
        rf->frame = frame;
        rf->next = next_rf;
        next_rf = rf;
    }
    LOG_DEBUG("frame count is %d\n", cnt);
    
    return rf;
} 

RevFrame *walk_rev_frame(RevFrame *rf) {
    return rf->next;
}

// 積まれてるframe stackを出力する
void debug_frame_info(WASMExecEnv* exec_env, WASMInterpFrame *frame) {
    RevFrame *rf = init_rev_frame(frame);
    WASMModuleInstance *module = exec_env->module_inst;

    int cnt = 0;
    printf("=== DEBUG Frame Stack ===\n");
    do {
        cnt++;
        if (rf->frame->function == NULL) {
            printf("%d) func_idx: -1\n", cnt);
        }
        else {
            printf("%d) func_idx: %d\n", cnt, rf->frame->function - module->e->functions);
        }
    } while (rf = rf->next);
    printf("=== DEBUG Frame Stack ===\n");
}

// func_instの先頭からlimitまでのopcodeを出力する
int debug_function_opcodes(WASMModuleInstance *module, WASMFunctionInstance* func, uint32 limit) {
    FILE *fp = fopen("wamr_opcode.log", "a");
    if (fp == NULL) return -1;

    fprintf(fp, "fidx: %d\n", func - module->e->functions);
    uint8 *ip = wasm_get_func_code(func);
    uint8 *ip_end = wasm_get_func_code_end(func);
    
    for (int i = 0; i < limit; i++) {
        fprintf(fp, "%d) opcode: 0x%x\n", i+1, *ip);
        ip = dispatch(ip, ip_end);
        if (ip >= ip_end) break;
    }

    fclose(fp);
    return 0;
}

// int debug_flag = 0;
// ipからip_limまでにopcodeがいくつかるかを返す
int get_opcode_offset(uint8 *ip, uint8 *ip_lim) {
    uint32 cnt = 0;
    bh_assert(ip != NULL);
    bh_assert(ip_lim != NULL);
    bh_assert(ip <= ip_lim);
    if (ip > ip_lim) return -1;
    if (ip == ip_lim) return 0;
    while (1) {
        // LOG_DEBUG("get_opcode_offset::ip: 0x%x\n", *ip);
        // if (debug_flag) {
        //     printf("(cnt, opcode) = (%d, 0x%x)\n", cnt, *ip);
        // }
        ip = dispatch(ip, ip_lim);
        cnt++;
        if (ip >= ip_lim) break;
    }
    return cnt;
}

/* wasm_dump for wasmedge */
int wasm_dump_memory_for_wasmedge(WASMMemoryInstance *memory) {
    FILE *fp;
    /* data */
    char *file = "memory_data_for_wasmedge.img";
    fp = fopen(file, "wb");
    if (fp == NULL) {
        fprintf(stderr, "failed to open %s\n", file);
        return -1;
    }

    // WASMMemoryInstance *memory = module->default_memory;
    fwrite(memory->memory_data, sizeof(uint8),
           memory->num_bytes_per_page * memory->cur_page_count, fp);

    fclose(fp);
    
    /* page_count */
    file = "memory_page_count_for_wasmedge.img";
    fp = fopen(file, "w");
    if (fp == NULL) {
        fprintf(stderr, "failed to open %s\n", file);
        return -1;
    }
    const uint32 wasmedge_page_size = 65536;
    uint32 page_count = (memory->num_bytes_per_page / wasmedge_page_size) * memory->cur_page_count;
    fprintf(fp, "%d", page_count);
    
    fclose(fp);
    
    return 0;
}


int wasm_dump_global_for_wasmedge(WASMModuleInstance *module, WASMGlobalInstance *globals, uint8* global_data) {
    FILE *fp;
    const char *file = "global_for_wasmedge.img";
    fp = fopen(file, "w");
    if (fp == NULL) {
        fprintf(stderr, "failed to open %s\n", file);
        return -1;
    }

    // WASMMemoryInstance *memory = module->default_memory;
    uint8 *global_addr;
    uint32 val32;
    uint64 val64;
    for (int i = 0; i < module->e->global_count; i++) {
        switch (globals[i].type) {
            case VALUE_TYPE_I32:
            case VALUE_TYPE_F32:
                global_addr = get_global_addr_for_migration(global_data, (globals+i));
                val32 = *(uint32*)global_addr;
                fprintf(fp, "%d\n", val32);
                break;
            case VALUE_TYPE_I64:
            case VALUE_TYPE_F64:
                global_addr = get_global_addr_for_migration(global_data, (globals+i));
                val64 = *(uint64*)global_addr;
                fprintf(fp, "%ld\n", val64);
                break;
            default:
                printf("type error:B\n");
                break;
        }
    }

    fclose(fp);
    return 0;
}

int wasm_dump_program_counter_for_wasmedge (
    WASMModuleInstance *module,
    WASMFunctionInstance *func, 
    uint8 *frame_ip
) {
    FILE *fp;
    char *file = "iter_for_wasmedge.img";
    fp = fopen(file, "w");
    if (fp == NULL) {
        fprintf(stderr, "failed to open %s\n", file);
        return -1;
    }
    
    // dump func_idx
    uint32 fidx = func - module->e->functions;
    fprintf(fp, "%d\n", fidx);

    // dump ip_offset
    uint32 ip_ofs = get_opcode_offset(wasm_get_func_code(func), frame_ip);
    fprintf(fp, "%d\n", ip_ofs);
    
    debug_function_opcodes(module, func, ip_ofs);

    fclose(fp);
    return 0;
}

int wasm_dump_stack_per_frame_for_wasmedge(WASMInterpFrame *frame, FILE *fp) {
    if (fp == NULL) {
        return -1;
    }
    
    /* param*/
    WASMFunctionInstance *func = frame->function;

    uint32 *lp = frame->lp;
    for (uint32 i = 0; i < func->param_count; i++) {
        switch (func->param_types[i]) {
            case VALUE_TYPE_I32:
            case VALUE_TYPE_F32:
                fprintf(fp, "%u\n", *(uint32 *)lp);
                lp++;
                break;
            case VALUE_TYPE_I64:
            case VALUE_TYPE_F64:
                fprintf(fp, "%lu\n", *(uint64 *)lp);
                lp += 2;
                break;
            default:
                printf("TYPE NULL\n");
                break;
        }
    }

    /* local */
    for (uint32 i = 0; i < func->local_count; i++) {
        switch (func->local_types[i]) {
            case VALUE_TYPE_I32:
            case VALUE_TYPE_F32:
                fprintf(fp, "%u\n", *(uint32 *)lp);
                lp++;
                break;
            case VALUE_TYPE_I64:
            case VALUE_TYPE_F64:
                fprintf(fp, "%lu\n", *(uint64 *)lp);
                lp += 2;
                break;
            default:
                printf("TYPE NULL\n");
                break;
        }
    }
    
    /* stack */
    uint32 tsp_num = frame->tsp - frame->tsp_bottom;
    uint32 sp_num = frame->sp - frame->sp_bottom;
    uint32 *cur_sp, *cur_tsp;
    cur_sp = frame->sp_bottom;
    cur_tsp = frame->tsp_bottom;
    
    for (uint32 i = 0; i < tsp_num; i++) {
        uint32 type = *(cur_tsp+i);
        // sp[i]: 32bit型
        if (type == 0) {
            fprintf(fp, "%u\n", *(uint32*)(cur_sp));
            cur_sp++;
        }
        // sp[i]: 64bit型
        else if (type == 1) {
            fprintf(fp, "%lu\n", *(uint64*)(cur_sp));
            cur_sp += 2;
        }
        cur_tsp++;
    }
    bh_assert(cur_sp == frame->sp);
    bh_assert(cur_tsp == frame->tsp);

    return 0;
}

int
wasm_dump_stack_for_wasmedge(struct WASMInterpFrame *frame)
{
    int rc;
    FILE *fp;
    char *file = "stack_for_wasmedge.img";
    fp = fopen(file, "w");
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
            continue;
        }
        else {
            rc = wasm_dump_stack_per_frame_for_wasmedge(frame, fp);
            if (rc < 0) {
                LOG_ERROR("failed to wasm_dump_stack_for_wasmedge");
                return rc;
            }
        }
    } while(rf = rf->next);

    fclose(fp);
    return 0;
}

int dump_frame_for_wasmedge(FILE *fp, const char* mod_name, uint32 fidx, uint32 ip_offset,
                uint32 locals, uint32 vpos, uint32 arity)
{
    if (fp == NULL) return -1;
    
    // mod name
    fprintf(fp, "%s\n", mod_name);
    // iterator
    fprintf(fp, "%u\n", fidx);
    fprintf(fp, "%u\n", ip_offset);
    // locals
    fprintf(fp, "%u\n", locals);
    // vpos
    fprintf(fp, "%u\n", vpos);
    // vpos
    fprintf(fp, "%u\n", arity);
    
    return 0;
}

int wasm_dump_frame_for_wasmedge(WASMModuleInstance *module, struct WASMInterpFrame *top_frame) {
    int rc;
    FILE *fp;
    char *file = "frame_for_wasmedge.img";
    fp = fopen(file, "w");
    if (fp == NULL) {
        fprintf(stderr, "failed to open %s\n", file);
        return -1;
    }
    
    /*
     * ModName: module_name
     * iter
     *  func_idx
     *  offset
     * Locals: param_count + local_count
     * VPos: stack_size
     * Arity: result_count
    */
    // TODO: importされたmoduleの場合どうする
    const char* mod_name = "";
    RevFrame *rf = init_rev_frame(top_frame);
    WASMInterpFrame *frame = rf->frame;
    WASMFunctionInstance *prev_func = NULL;
    uint32 stack_count = 0;
    uint32 fidx, ip_ofs;
    uint32 *ip_start, *ip_end;
    // frameをbottomからtopまで走査する
    
    // dummy frame
    {
        if (frame->function == NULL) {
            // WasmEdgeではダミーフレームの場合、NullModNameのみ出力するようにしている
            fprintf(fp, "null\n");
            fprintf(fp, "\n");
        }
        rf = rf->next;
        if (rf == NULL) return 0;
    }

    // start_funtionのinstr.endをpushする
    {
        frame = rf->frame;
        fidx = frame->function - module->e->functions;

        ip_start = wasm_get_func_code(frame->function);
        ip_end = wasm_get_func_code_end(frame->function);
        // wasmedgeは現在のiter-1をframeにpushするため-1しておく
        ip_ofs = get_opcode_offset(ip_start, ip_end) - 1;

        uint32 locals = frame->function->param_count + frame->function->local_count;
        uint32 vpos = frame->vpos;
        uint32 arity = frame->function->result_count;

        dump_frame_for_wasmedge(fp, mod_name, fidx, ip_ofs, locals, vpos, arity);
        fprintf(fp, "\n");

        // fidxとip_ofsは前のフレームのものをdumpするため、ここで更新
        // ただし最初のみfidxは同じ
        ip_ofs = get_opcode_offset(ip_start, frame->ip) - 1;
        prev_func = frame->function;
        rf = rf->next;
        if (rf == NULL) return 0;
    }
    
    do {
        frame = rf->frame;
        if (frame == NULL) {
            perror("wasm_dump_frame: frame is null\n");
            break;
        }

        // dummy frame
        if (frame->function == NULL) {
            // WasmEdgeではダミーフレームの場合、NullModNameのみ出力するようにしている
            fprintf(fp, "null\n");
        }
        else {
            uint32 locals = frame->function->param_count + frame->function->local_count;
            uint32 vpos = frame->vpos;
            uint32 arity = frame->function->result_count;

            // debug_function_opcodes(module, prev_func, ip_ofs);
            dump_frame_for_wasmedge(fp, mod_name, fidx, ip_ofs, locals, vpos, arity);

            // fidxとip_ofsは前のフレームのものをdumpするため、ここで更新
            prev_func = frame->function;
            fidx = frame->function - module->e->functions;
            ip_start = wasm_get_func_code(frame->function);
            ip_ofs = get_opcode_offset(ip_start, frame->ip) - 1;
        }
        // WasmEdgeでは、フレームの区切りで空行が入る
        fprintf(fp, "\n");
    } while(rf = rf->next);

    fclose(fp);
    return 0;
}



int wasm_dump_for_wasmedge(
    WASMExecEnv *exec_env,
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
    time_t start, end;
    
    start = clock();
    rc = wasm_dump_global_for_wasmedge(module, globals, global_data);
    if (rc < 0) {
        LOG_ERROR("Failed to dump globals for wasmedge\n");
        return rc;
    }
    end = clock();
    printf("wasmedge global, %f\n", (double)(end-start)/CLOCKS_PER_SEC*1000.0);
    
    start = clock();
    rc = wasm_dump_program_counter_for_wasmedge(exec_env->module_inst, cur_func, frame_ip);
    if (rc < 0) {
        LOG_ERROR("Failed to dump program counter for wasmedge\n");
        return rc;
    }
    end = clock();
    printf("wasmedge program counter, %f\n", (double)(end-start)/CLOCKS_PER_SEC*1000.0);

    // debug
    // debug_frame_info(exec_env, frame);

    start = clock();
    rc = wasm_dump_frame_for_wasmedge(exec_env->module_inst, frame);
    if (rc < 0) {
        LOG_ERROR("Failed to dump frame for wasmedge\n");
        return rc;
    }
    end = clock();
    printf("wasmedge frame stack, %f\n", (double)(end-start)/CLOCKS_PER_SEC*1000.0);

    start = clock();
    rc = wasm_dump_stack_for_wasmedge(frame);
    if (rc < 0) {
        LOG_ERROR("Failed to dump stack for wasmedge\n");
        return rc;
    }
    end = clock();
    printf("wasmedge value stack, %f\n", (double)(end-start)/CLOCKS_PER_SEC*1000.0);
    
    LOG_VERBOSE("Success to dump img for wasmedge\n");
    return 0;
}

/* wasm_dump for webassembly micro runtime */
static void
dump_WASMInterpFrame(struct WASMInterpFrame *frame, WASMExecEnv *exec_env, FILE *fps[3])
{
    FILE *fp, *csp_tsp_fp, *tsp_fp;
    fp = fps[0];
    csp_tsp_fp = fps[1];
    tsp_fp = fps[2];
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

    // uint32 *tsp_bottom;
    // uint32 *tsp_boundary;
    // uint32 *tsp;
    uint32 tsp_offset = frame->tsp - frame->tsp_bottom;
    fwrite(&tsp_offset, sizeof(uint32), 1, tsp_fp);

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
    fwrite(frame->tsp_bottom, sizeof(uint32), tsp_offset, tsp_fp);

    WASMBranchBlock *csp = frame->csp_bottom;
    uint32 csp_num = frame->csp - frame->csp_bottom;
    

    uint64 addr;
    for (i = 0; i < csp_num; i++, csp++) {
        // uint8 *begin_addr;
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

        // uint32 *frame_tsp;
        if (csp->frame_tsp == NULL) {
            addr = -1;
            fwrite(&addr, sizeof(uint64), 1, csp_tsp_fp);
        }
        else {
            addr = csp->frame_tsp - frame->tsp_bottom;
            fwrite(&addr, sizeof(uint64), 1, csp_tsp_fp);
        }
        
        // uint32 cell_num;
        fwrite(&csp->cell_num, sizeof(uint32), 1, fp);
        // uint32 count;
        fwrite(&csp->count, sizeof(uint32), 1, csp_tsp_fp);
    }
}

FILE* open_image(const char* file) {
    FILE *fp = fopen(file, "wb");
    if (fp == NULL) {
        fprintf(stderr, "failed to open %s\n", file);
        return NULL;
    }
    return fp;
}

int
wasm_dump_frame(WASMExecEnv *exec_env, struct WASMInterpFrame *frame)
{
    WASMModuleInstance *module =
        (WASMModuleInstance *)exec_env->module_inst;
    WASMFunctionInstance *function;

    FILE *fp = open_image("frame.img");
    FILE *csp_tsp_fp = open_image("ctrl_tsp.img");
    FILE *tsp_fp = open_image("type_stack.img");
    FILE *fps[3] = {fp, csp_tsp_fp, tsp_fp};

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
            dump_WASMInterpFrame(frame, exec_env, fps);
        }
    } while(rf = rf->next);

    fclose(fp);
    fclose(csp_tsp_fp);
    fclose(tsp_fp);
    return 0;
}

int wasm_dump_memory(WASMMemoryInstance *memory) {
    FILE *memory_fp = open_image("memory.img");
    FILE *mem_size_fp = open_image("mem_page_count.img");

    // WASMMemoryInstance *memory = module->default_memory;
    fwrite(memory->memory_data, sizeof(uint8),
           memory->num_bytes_per_page * memory->cur_page_count, memory_fp);

    printf("page_count: %d\n", memory->cur_page_count);
    fwrite(&(memory->cur_page_count), sizeof(uint32), 1, mem_size_fp);

    fclose(memory_fp);
    fclose(mem_size_fp);
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

int wasm_dump_tsp_addr(uint32 *frame_tsp, struct WASMInterpFrame *frame)
{
    FILE *fp;
    const char *file = "tsp_addr.img";
    fp = fopen(file, "wb");
    if (fp == NULL) {
        fprintf(stderr, "failed to open %s\n", file);
        return -1;
    }

    uint32_t p_offset = frame_tsp - frame->tsp_bottom;
    dump_value(&p_offset, sizeof(uint32), 1, fp);

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
         uint32 *frame_tsp,
         uint8 *frame_ip_end,
         uint8 *else_addr,
         uint8 *end_addr,
         uint8 *maddr,
         bool done_flag)
{
    int rc;
    time_t start, end;
    time_t tstart, tend;
    tstart = clock();

    rc = wasm_dump_for_wasmedge(
        exec_env, module, memory,
        globals, global_data, global_addr,
        cur_func, frame, frame_ip, frame_sp, frame_csp,
        frame_ip_end, else_addr, end_addr, maddr, done_flag
    );
    if (rc < 0) {
        LOG_ERROR("Failed to dump for wasmedge\n");
        return rc;
    }

    // dump linear memory
    start = clock();
    rc = wasm_dump_memory(memory);
    if (rc < 0) {
        LOG_ERROR("Failed to dump linear memory\n");
        return rc;
    }
    end = clock();
    printf("unifie memory, %f\n", (double)(end-start)/CLOCKS_PER_SEC*1000.0);

    // dump globals
    start = clock();
    rc = wasm_dump_global(module, globals, global_data);
    if (rc < 0) {
        LOG_ERROR("Failed to dump globals\n");
        return rc;
    }
    end = clock();
    printf("wamr global, %f\n", (double)(end-start)/CLOCKS_PER_SEC*1000.0);

    // dump frame
    start = clock();
    rc = wasm_dump_frame(exec_env, frame);
    if (rc < 0) {
        LOG_ERROR("Failed to dump frame\n");
        return rc;
    }

    // dump tsp addrs
    rc = wasm_dump_tsp_addr(frame_tsp, frame);
    if (rc < 0) {
        perror("failed to dump_tsp_addr\n");
        exit(1);
    }

    // dump addrs
    rc = wasm_dump_addrs(frame, cur_func, memory, 
                    frame_ip, frame_sp, frame_csp, frame_ip_end,
                    else_addr, end_addr, maddr, done_flag);
    if (rc < 0) {
        LOG_ERROR("Failed to dump addrs\n");
        return rc;
    }
    end = clock();
    printf("wamr frame stack, %f\n", (double)(end-start)/CLOCKS_PER_SEC*1000.0);

    LOG_VERBOSE("Success to dump img for wamr\n");
    tend = clock();
    printf("total, %f\n", (double)(end-start)/CLOCKS_PER_SEC*1000.0);
    return 0;
}
