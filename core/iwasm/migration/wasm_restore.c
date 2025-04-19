#include <stdio.h>
#include <stdlib.h>

#include "../common/wasm_exec_env.h"
#include "../common/wasm_memory.h"
#include "../interpreter/wasm_runtime.h"
#include "wasm_migration.h"
#include "wasm_restore.h"
#include <wasmig/migration.h>
#include <wasmig/log.h>

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
_restore_stack(WASMExecEnv *exec_env, WASMInterpFrame *frame, CallStackEntry *entry)
{
    WASMModuleInstance *module_inst = exec_env->module_inst;
    WASMFunctionInstance *func = frame->function;
    frame->ip = wasm_get_func_code(frame->function) + entry->pc.offset;
    printf("restore ip: (%d, %d)\n", entry->pc.fidx, entry->pc.offset);

    // 初期化
    frame->sp_bottom = frame->lp + func->param_cell_num + func->local_cell_num;
    frame->sp_boundary = frame->sp_bottom + func->u.func->max_stack_cell_num;
    frame->csp_bottom = frame->sp_boundary;
    frame->csp_boundary = frame->csp_bottom + func->u.func->max_block_num;
    // frame->tsp_bottom = frame->csp_boundary;
    // frame->tsp_boundary = frame->tsp_bottom + func->u.func->max_stack_cell_num;

    // NOTE: ここで復元すべきか
    // リターンアドレス
    // WASMInterpFrame* prev_frame = frame->prev_frame;
    // uint32 fidx, offset;
    // fread(&fidx, sizeof(uint32), 1, fp);
    // fread(&offset, sizeof(uint32), 1, fp);
    // if (prev_frame->function != NULL)
    //     prev_frame->ip = wasm_get_func_code(prev_frame->function) + entry->return_addr.offset;

    // NOTE: 型スタックのサイズはここではいらない
    // 型スタックのサイズ
    // uint32 locals = func->param_count + func->local_count;
    // uint32 full_type_stack_size, type_stack_size;
    // fread(&full_type_stack_size, sizeof(uint32), 1, fp);
    // type_stack_size = full_type_stack_size - locals;                                      // 統一フォーマットでは、ローカルも型/値スタックに入れているが、WAMRの型/値スタックのサイズはローカル抜き
    // frame->tsp = frame->tsp_bottom + type_stack_size;

    // 型スタックの中身
    // fseek(fp, sizeof(uint8)*locals, SEEK_CUR);                      // localのやつはWAMRでは必要ないので飛ばす

    // uint8 type_stack[type_stack_size];
    // uint32* tsp_bottom = frame->tsp_bottom;
    // for (uint32 i = 0; i < type_stack_size; ++i) {
    //     fread(&type_stack[i], sizeof(uint8), 1, fp);
    // }

    // ローカル+値スタックのサイズ復元
    // uint32 *tsp = frame->tsp_bottom;
    // uint32 value_stack_size = 0;
    // for (uint32 i = 0; i < entry->locals.size; ++i) {
    //     value_stack_size += type_stack[i];
    // }
    uint32 stack_size = entry->locals.values.size + entry->value_stack.values.size;
    frame->sp = frame->sp_bottom + stack_size;
    wasmig_info("restore sp");

    // 値スタックの中身
    uint32 local_cell_num = func->param_cell_num + func->local_cell_num;
    printf("local_cell_num: %d\n", local_cell_num);
    printf("locals.values.size: %d\n", entry->locals.values.size);
    memcpy(frame->lp, entry->locals.values.contents, entry->locals.values.size * sizeof(uint32_t));
    // fread(frame->lp, sizeof(uint32), local_cell_num, fp);
    // debug_local(frame);
    // fread(frame->sp_bottom, sizeof(uint32), value_stack_size, fp);
    memcpy(frame->sp_bottom, entry->value_stack.values.contents, entry->value_stack.values.size * sizeof(uint32_t));
    wasmig_info("restore value stack");


    // ラベルスタックのサイズ
    uint32 ctrl_stack_size = entry->label_stack.size;
    // fread(&ctrl_stack_size, sizeof(uint32), 1, fp);
    frame->csp = frame->csp_bottom + ctrl_stack_size;


    // ラベルスタックの中身
    WASMBranchBlock *csp = frame->csp_bottom;
    for (int i = 0; i < ctrl_stack_size; ++i, ++csp) {
        uint64 offset;

        // uint8 *begin_addr;
        // fread(&offset, sizeof(uint32), 1, fp);
        offset = entry->label_stack.begins[i];
        csp->begin_addr = set_addr_offset(wasm_get_func_code(frame->function), offset);

        // uint8 *target_addr;
        // fread(&offset, sizeof(uint32), 1, fp);
        offset = entry->label_stack.targets[i];
        csp->target_addr = set_addr_offset(wasm_get_func_code(frame->function), offset);

        // uint32 *frame_sp;
        // fread(&offset, sizeof(uint32), 1, fp);
        offset = entry->label_stack.stack_pointers[i];
        csp->frame_sp = set_addr_offset(frame->sp_bottom, offset);

        // uint32 cell_num;
        offset = entry->label_stack.cell_nums[i];
        csp->cell_num = offset;
        // fread(&csp->cell_num, sizeof(uint32), 1, fp);
    }
    wasmig_info("restore label stack");
}

WASMInterpFrame*
wasm_restore_stack(WASMExecEnv **_exec_env)
{
    wasmig_log_init(1);
    wasmig_info("wasm_restore_stack\n");
    WASMExecEnv *exec_env = *_exec_env;
    WASMModuleInstance *module_inst =
        (WASMModuleInstance *)exec_env->module_inst;
    WASMInterpFrame *frame, *prev_frame = wasm_exec_env_get_cur_frame(exec_env);
    frame = prev_frame;
    WASMFunctionInstance *function;
    uint32 func_idx, frame_size, all_cell_num;
    FILE *fp;
    
    CallStack cs = restore_stack();
    wasmig_debug("restore_stack: cs.size: %d\n", cs.size);
    print_call_stack(&cs);

    // uint32 frame_stack_size;
    // fp = open_image("frame.img", "rb");
    // fread(&frame_stack_size, sizeof(uint32), 1, fp);
    // fclose(fp);

    // char file[32];
    // uint32 fidx = 0;
    for (int i = 0; i < cs.size; i++) {
        CallStackEntry *entry = &cs.entries[i];
        function = module_inst->e->functions + entry->pc.fidx;

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
        wasmig_info("(call stack id, (fidx, offset)): ({}, ({}, {}))", 
            i, entry->pc.fidx, entry->pc.offset);
        frame->function = function;
        _restore_stack(exec_env, frame, entry);

        prev_frame = frame;
    //     fclose(fp);
    }
    wasmig_info("restore frame\n");

    // debug_wasm_interp_frame(frame, module_inst->e->functions);
    wasm_exec_env_set_cur_frame(exec_env, frame);
    
    _exec_env = &exec_env;

    wasmig_info("Finish to restore stack\n");
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
    Array8 mem = restore_memory();

    // restore page_count
    uint32 page_count = mem.size / (*memory)->num_bytes_per_page;
    wasm_enlarge_memory(module, page_count- (*memory)->cur_page_count);
    *maddr = page_count * (*memory)->num_bytes_per_page;

    // restore data
    memcpy((*memory)->memory_data, mem.contents, mem.size);
    return 0;
}

// TODO: wasmigを使う
int wasm_restore_global(const WASMModuleInstance *module, const WASMGlobalInstance *globals, uint8 **global_data, uint8 **global_addr) {
    FILE* fp = wamr_open_image("global.img", "rb");

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
    CodePos pc = restore_pc();
    *frame_ip = wasm_get_func_code(module->e->functions + pc.fidx) + pc.offset;

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
    struct timespec ts1, ts2;
    // restore memory
    clock_gettime(CLOCK_MONOTONIC, &ts1);
    wasm_restore_memory(*module, memory, maddr);
    clock_gettime(CLOCK_MONOTONIC, &ts2);
    fprintf(stderr, "memory, %lu\n", get_time(ts1, ts2));
    // printf("Success to restore linear memory\n");

    // restore globals
    clock_gettime(CLOCK_MONOTONIC, &ts1);
    wasm_restore_global(*module, *globals, global_data, global_addr);
    clock_gettime(CLOCK_MONOTONIC, &ts2);
    fprintf(stderr, "global, %lu\n", get_time(ts1, ts2));
    // printf("Success to restore globals\n");

    // restore program counter
    clock_gettime(CLOCK_MONOTONIC, &ts1);
    wasm_restore_program_counter(*module, frame_ip);
    clock_gettime(CLOCK_MONOTONIC, &ts2);
    fprintf(stderr, "program counter, %lu\n", get_time(ts1, ts2));
    // printf("Success to program counter\n");

    return 0;
}