#include <stdio.h>
#include <stdlib.h>

#include "../common/wasm_exec_env.h"
#include "../common/wasm_memory.h"
#include "../interpreter/wasm_runtime.h"
#include "wasm_migration.h"
#include "wasm_restore.h"

static bool restore_flag;
void set_restore_flag(bool f)
{
    restore_flag = f;
}
bool get_restore_flag()
{
    return restore_flag;
}


static inline WASMInterpFrame *
wasm_alloc_frame(WASMExecEnv *exec_env, uint32 size, WASMInterpFrame *prev_frame)
{
    WASMInterpFrame *frame = wasm_exec_env_alloc_wasm_frame(exec_env, size);

    if (frame) {
        frame->prev_frame = prev_frame;
#if WASM_ENABLE_PERF_PROFILING != 0
        frame->time_started = os_time_get_boot_microsecond();
#endif
    }
    else {
        wasm_set_exception((WASMModuleInstance *)exec_env->module_inst,
                           "wasm operand stack overflow");
    }

    return frame;
}

static void
debug_frame(WASMInterpFrame* frame)
{
    // fprintf(stderr, "Return Address: (%d, %d)\n", fidx, offset);
    // fprintf(stderr, "TypeStack Content: [");
    // uint32* tsp_bottom = frame->tsp_bottom;
    // for (uint32 i = 0; i < type_stack_size; ++i) {
    //     uint8 type = *(tsp_bottom+i);
    //     fprintf(stderr, "%d, ", type);
    // }
    // fprintf(stderr, "]\n");
    // fprintf(stderr, "Value Stack Size: %d\n", value_stack_size);
    // fprintf(stderr, "Type Stack Size(Local含む): %d\n", full_type_stack_size);
    // fprintf(stderr, "Type Stack Size(Local含まず): %d\n", type_stack_size);
    // fprintf(stderr, "Label Stack Size: %d\n", ctrl_stack_size);
    
}

static void
debug_local(WASMInterpFrame *frame)
{
    WASMFunctionInstance *func = frame->function;
    uint32 *lp = frame->lp;
    uint32 param_count = func->param_count;
    uint32 local_count = func->local_count;

    fprintf(stderr, "locals: [");
    for (uint32 i = 0; i < param_count; i++) {
        switch (func->param_types[i]) {
            case VALUE_TYPE_I32:
            case VALUE_TYPE_F32:
                fprintf(stderr, "%u, ", *(uint32 *)lp);
                lp++;
                break;
            case VALUE_TYPE_I64:
            case VALUE_TYPE_F64:
                fprintf(stderr, "%lu, ", *(uint64 *)lp);
                lp += 2;
                break;
            default:
                printf("TYPE NULL\n");
                break;
        }
    }

    /* local */
    for (uint32 i = 0; i < local_count; i++) {
        switch (func->local_types[i]) {
            case VALUE_TYPE_I32:
            case VALUE_TYPE_F32:
                fprintf(stderr, "%u, ", *(uint32 *)lp);
                lp++;
                break;
            case VALUE_TYPE_I64:
            case VALUE_TYPE_F64:
                fprintf(stderr, "%lu, ", *(uint64 *)lp);
                lp += 2;
                break;
            default:
                printf("TYPE NULL\n");
                break;
        }
    }
    fprintf(stderr, "]\n");
}


static void
debug_label_stack(WASMInterpFrame *frame)
{
    WASMBranchBlock *csp = frame->csp_bottom;
    uint32 csp_num = frame->csp - csp;
    
    fprintf(stderr, "label stack: [\n");
    for (int i = 0; i < csp_num; i++, csp++) {
        // uint8 *begin_addr;
        fprintf(stderr, "\t{%d",
            // csp->begin_addr == NULL ? -1 : csp->begin_addr - wasm_get_func_code(frame->function);
            get_addr_offset(csp->begin_addr, wasm_get_func_code(frame->function))
        );

        // uint8 *target_addr;
        fprintf(stderr, ", %d",
            get_addr_offset(csp->target_addr, wasm_get_func_code(frame->function))
        );

        // uint32 *frame_sp;
        fprintf(stderr, ", %d",
            get_addr_offset(csp->frame_sp, frame->sp_bottom)
        );

        // uint32 *frame_tsp
        // // fprintf(stderr, ", %d",
        // //     get_addr_offset(csp->frame_tsp, frame->tsp_bottom)
        // );

        // uint32 cell_num;
        fprintf(stderr, ", %d", csp->cell_num);

        // uint32 count;
        // fprintf(stderr, ", %d}\n", csp->count);
    }
    fprintf(stderr, "]\n");
}

static void
_restore_stack(WASMExecEnv *exec_env, WASMInterpFrame *frame, FILE *fp)
{
    WASMModuleInstance *module_inst = exec_env->module_inst;
    WASMFunctionInstance *func = frame->function;

    // 初期化
    frame->sp_bottom = frame->lp + func->param_cell_num + func->local_cell_num;
    frame->sp_boundary = frame->sp_bottom + func->u.func->max_stack_cell_num;
    frame->csp_bottom = frame->sp_boundary;
    frame->csp_boundary = frame->csp_bottom + func->u.func->max_block_num;
    // frame->tsp_bottom = frame->csp_boundary;
    // frame->tsp_boundary = frame->tsp_bottom + func->u.func->max_stack_cell_num;

    // リターンアドレス
    WASMInterpFrame* prev_frame = frame->prev_frame;
    uint32 fidx, offset;
    fread(&fidx, sizeof(uint32), 1, fp);
    fread(&offset, sizeof(uint32), 1, fp);
    if (prev_frame->function != NULL)
        prev_frame->ip = wasm_get_func_code(prev_frame->function) + offset;

    // 型スタックのサイズ
    uint32 locals = func->param_count + func->local_count;
    uint32 full_type_stack_size, type_stack_size;
    fread(&full_type_stack_size, sizeof(uint32), 1, fp);
    type_stack_size = full_type_stack_size - locals;                                      // 統一フォーマットでは、ローカルも型/値スタックに入れているが、WAMRの型/値スタックのサイズはローカル抜き
    // frame->tsp = frame->tsp_bottom + type_stack_size;

    // 型スタックの中身
    fseek(fp, sizeof(uint8)*locals, SEEK_CUR);                      // localのやつはWAMRでは必要ないので飛ばす

    uint8 type_stack[type_stack_size];
    // uint32* tsp_bottom = frame->tsp_bottom;
    for (uint32 i = 0; i < type_stack_size; ++i) {
        fread(&type_stack[i], sizeof(uint8), 1, fp);
    }

    // 値スタックのサイズ
    // uint32 *tsp = frame->tsp_bottom;
    uint32 value_stack_size = 0;
    for (uint32 i = 0; i < type_stack_size; ++i) {
        value_stack_size += type_stack[i];
    }
    frame->sp = frame->sp_bottom + value_stack_size;

    // 値スタックの中身
    uint32 local_cell_num = func->param_cell_num + func->local_cell_num;
    fread(frame->lp, sizeof(uint32), local_cell_num, fp);
    // debug_local(frame);
    fread(frame->sp_bottom, sizeof(uint32), value_stack_size, fp);

    // ラベルスタックのサイズ
    uint32 ctrl_stack_size;
    fread(&ctrl_stack_size, sizeof(uint32), 1, fp);
    frame->csp = frame->csp_bottom + ctrl_stack_size;


    // ラベルスタックの中身
    WASMBranchBlock *csp = frame->csp_bottom;
    for (int i = 0; i < ctrl_stack_size; ++i, ++csp) {
        uint64 offset;

        // uint8 *begin_addr;
        fread(&offset, sizeof(uint32), 1, fp);
        csp->begin_addr = set_addr_offset(wasm_get_func_code(frame->function), offset);

        // uint8 *target_addr;
        fread(&offset, sizeof(uint32), 1, fp);
        csp->target_addr = set_addr_offset(wasm_get_func_code(frame->function), offset);

        // uint32 *frame_sp;
        fread(&offset, sizeof(uint32), 1, fp);
        csp->frame_sp = set_addr_offset(frame->sp_bottom, offset);

        // uint32 *frame_tsp
        // fread(&offset, sizeof(uint32), 1, fp);
        // csp->frame_tsp = set_addr_offset(frame->tsp_bottom, offset);

        // uint32 cell_num;
        fread(&csp->cell_num, sizeof(uint32), 1, fp);

        // uint32 count;
        // fread(&csp->count, sizeof(uint32), 1, fp);
    }
}

WASMInterpFrame*
wasm_restore_stack(WASMExecEnv **_exec_env)
{
    WASMExecEnv *exec_env = *_exec_env;
    WASMModuleInstance *module_inst =
        (WASMModuleInstance *)exec_env->module_inst;
    WASMInterpFrame *frame, *prev_frame = wasm_exec_env_get_cur_frame(exec_env);
    frame = prev_frame;
    WASMFunctionInstance *function;
    uint32 func_idx, frame_size, all_cell_num;
    FILE *fp;

    uint32 frame_stack_size;
    fp = open_image("frame.img", "rb");
    fread(&frame_stack_size, sizeof(uint32), 1, fp);
    fclose(fp);

    char file[32];
    uint32 fidx = 0;
    for (uint32 i = frame_stack_size; i > 0; --i) {
        sprintf(file, "stack%d.img", i);
        fp = open_image(file, "rb");

        fread(&fidx, sizeof(uint32), 1, fp);
        // 関数からスタックサイズを計算し,ALLOC
        // 前のframe2のenter_func_idxが、このframe->functionに対応
        function = module_inst->e->functions + fidx;

        // TODO: uint64になってるけど、多分uint32
        all_cell_num = (uint32)function->param_cell_num
                        + (uint32)function->local_cell_num
                        + (uint32)function->u.func->max_stack_cell_num
                        + ((uint32)function->u.func->max_block_num)
                                * sizeof(WASMBranchBlock) / 4
                        + (uint32)function->u.func->max_stack_cell_num;
        frame_size = wasm_interp_interp_frame_size(all_cell_num);
        frame = wasm_alloc_frame(exec_env, frame_size,
                            (WASMInterpFrame *)prev_frame);

        // フレームをrestore
        frame->function = function;
        _restore_stack(exec_env, frame, fp);

        prev_frame = frame;
        fclose(fp);
    }

    // debug_wasm_interp_frame(frame, module_inst->e->functions);
    wasm_exec_env_set_cur_frame(exec_env, frame);
    
    _exec_env = &exec_env;

    return frame;
}

void restore_dirty_memory(WASMMemoryInstance **memory, FILE* memory_fp) {
    const int PAGE_SIZE = 4096;
    while (!feof(memory_fp)) {
        if (feof(memory_fp)) break;
        uint32 offset;
        uint32 len;
        len = fread(&offset, sizeof(uint32), 1, memory_fp);
        if (len == 0) break;
        // printf("len: %d\n", len);
        // printf("i: %d\n", offset);

        uint8* addr = (*memory)->memory_data + offset;
        len = fread(addr, PAGE_SIZE, 1, memory_fp);
        // printf("PAGESIZE: %d\n", len);
    }
}

int wasm_restore_memory(WASMModuleInstance *module, WASMMemoryInstance **memory, uint8** maddr) {
    FILE* memory_fp = open_image("memory.img", "rb");
    FILE* mem_size_fp = open_image("mem_page_count.img", "rb");

    // restore page_count
    uint32 page_count;
    fread(&page_count, sizeof(uint32), 1, mem_size_fp);
    wasm_enlarge_memory(module, page_count- (*memory)->cur_page_count);
    *maddr = page_count * (*memory)->num_bytes_per_page;

    restore_dirty_memory(memory, memory_fp);
    // restore memory_data
    // fread((*memory)->memory_data, sizeof(uint8),
    //         (*memory)->num_bytes_per_page * (*memory)->cur_page_count, memory_fp);

    fclose(memory_fp);
    fclose(mem_size_fp);
    return 0;
}

int wasm_restore_global(const WASMModuleInstance *module, const WASMGlobalInstance *globals, uint8 **global_data, uint8 **global_addr) {
    FILE* fp = open_image("global.img", "rb");

    for (int i = 0; i < module->e->global_count; i++) {
        switch (globals[i].type) {
            case VALUE_TYPE_I32:
            case VALUE_TYPE_F32:
                *global_addr = get_global_addr_for_migration(*global_data, globals + i);
                fread(*global_addr, sizeof(uint32), 1, fp);
                break;
            case VALUE_TYPE_I64:
            case VALUE_TYPE_F64:
                *global_addr = get_global_addr_for_migration(*global_data, globals + i);
                fread(*global_addr, sizeof(uint64), 1, fp);
                break;
            default:
                perror("wasm_restore_global:type error:A\n");
                break;
        }
    }

    fclose(fp);
    return 0;
}

void debug_addr(const char* name, const char* func_name, int value) {
    if (value == NULL) {
        fprintf(stderr, "debug_addr: %s value is NULL\n", name);
        return;
    }
    printf("%s in %s: %p\n", name, func_name, (int)value);
}

int wasm_restore_program_counter(
    WASMModuleInstance *module,
    uint8 **frame_ip)
{
    FILE* fp = open_image("program_counter.img", "rb");

    uint32 fidx, offset;
    fread(&fidx, sizeof(uint32), 1, fp);
    fread(&offset, sizeof(uint32), 1, fp);

    *frame_ip = wasm_get_func_code(module->e->functions + fidx) + offset;

    return 0;
}

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
            bool *done_flag)
{
    // restore memory
    wasm_restore_memory(*module, memory, maddr);
    // printf("Success to restore linear memory\n");

    // restore globals
    wasm_restore_global(*module, *globals, global_data, global_addr);
    // printf("Success to restore globals\n");

    // restore program counter
    wasm_restore_program_counter(*module, frame_ip);
    // printf("Success to program counter\n");

    return 0;
}