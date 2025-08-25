#include <stdio.h>
#include <stdlib.h>

#include "../common/wasm_exec_env.h"
#include "../common/wasm_memory.h"
#include "../interpreter/wasm_runtime.h"
#include "wasm_migration.h"
#include "wasm_restore.h"
#include "helper.h"
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
static void
_restore_program_counter(WASMInterpFrame *frame, CallStackEntry *entry)
{
    // NOTE: WAMRはtop以外のフレームでは、call命令の次の命令にipが設定されているので、checkpointではcall命令を指すpcを保存した。
    // restore時は、Callの次の命令を指すように修正する
    CodePos ret_pos = next_pc(entry->pc);
    frame->ip = wasm_get_func_code(frame->function) + ret_pos.offset;
    printf("restore ip: (%d, %d)\n", entry->pc.fidx, entry->pc.offset);
}

// Initialize stack and call stack boundaries
static void
_initialize_frame_boundaries(WASMInterpFrame *frame, WASMFunctionInstance *func)
{
    frame->sp_bottom = frame->lp + func->param_cell_num + func->local_cell_num;
    frame->sp_boundary = frame->sp_bottom + func->u.func->max_stack_cell_num;
    frame->csp_bottom = frame->sp_boundary;
    frame->csp_boundary = frame->csp_bottom + func->u.func->max_block_num;
}

static void
_restore_value_stacks(WASMInterpFrame *frame, WASMFunctionInstance *func, CallStackEntry *entry)
{
    // 値スタック（SP）のサイズ復元
    uint32 stack_size = entry->value_stack.values.size;
    frame->sp = frame->sp_bottom + stack_size;
    wasmig_debug("restore sp");

    // restore locals
    uint32 local_cell_num = func->param_cell_num + func->local_cell_num;
    printf("local_cell_num: %d\n", local_cell_num);
    printf("locals.values.size: %d\n", entry->locals.values.size);
    memcpy(frame->lp, entry->locals.values.contents, entry->locals.values.size * sizeof(uint32_t));

    // restore value stack
    memcpy(frame->sp_bottom, entry->value_stack.values.contents, entry->value_stack.values.size * sizeof(uint32_t));
    wasmig_debug("restore value stack");
}

static void
_restore_label_stack(WASMInterpFrame *frame, CallStackEntry *entry)
{
    // ラベルスタックのサイズ設定
    uint32 ctrl_stack_size = entry->label_stack.size;
    frame->csp = frame->csp_bottom + ctrl_stack_size;

    // ラベルスタックの復元
    WASMBranchBlock *csp = frame->csp_bottom;
    for (int i = 0; i < ctrl_stack_size; ++i, ++csp) {
        uint64 offset;

        // begin_addr の復元
        offset = entry->label_stack.begins[i];
        csp->begin_addr = set_addr_offset(wasm_get_func_code(frame->function), offset);

        // target_addr の復元
        offset = entry->label_stack.targets[i];
        csp->target_addr = set_addr_offset(wasm_get_func_code(frame->function), offset);

        // frame_sp の復元
        offset = entry->label_stack.stack_pointers[i];
        csp->frame_sp = set_addr_offset(frame->sp_bottom, offset);

        // cell_num の復元
        offset = entry->label_stack.cell_nums[i];
        csp->cell_num = offset;
    }
    wasmig_info("restore label stack");
}

_restore_frame(WASMExecEnv *exec_env, WASMInterpFrame *frame, CallStackEntry *entry)
{
    WASMModuleInstance *module_inst = exec_env->module_inst;
    WASMFunctionInstance *func = frame->function;

    // restore a program counter
    _restore_program_counter(frame, entry);

    // Initialize frame boundaries
    _initialize_frame_boundaries(frame, func);

    // restore locals and value stack
    _restore_value_stacks(frame, func, entry);

    // restore label stack
    _restore_label_stack(frame, entry);
}

// Allocate frame
WASMInterpFrame*
static WASMInterpFrame *
_create_frame(WASMExecEnv *exec_env, WASMModuleInstance *module_inst, 
              CallStackEntry *entry, WASMInterpFrame *prev_frame)
{
    WASMFunctionInstance *function = module_inst->e->functions + entry->pc.fidx;
    
    // Calculate frame size
    uint32 all_cell_num = (uint32)function->param_cell_num
                        + (uint32)function->local_cell_num
                        + (uint32)function->u.func->max_stack_cell_num
                        + ((uint32)function->u.func->max_block_num)
                                * sizeof(WASMBranchBlock) / 4
                        + (uint32)function->u.func->max_stack_cell_num;
    uint32 frame_size = wasm_interp_interp_frame_size(all_cell_num);
    
    // Allocate this frame
    WASMInterpFrame *frame = wasm_alloc_frame(exec_env, frame_size, prev_frame);
    frame->function = function;
    
    return frame;
}

static void
_restore_all_frames(WASMExecEnv *exec_env, WASMModuleInstance *module_inst, CallStack *cs)
{
    WASMInterpFrame *frame, *prev_frame = wasm_exec_env_get_cur_frame(exec_env);

    // Scan call stack entries
    for (int i = 0; i < cs->size; i++) {
        CallStackEntry *entry = &cs->entries[i];
        
        // allocate frame
        frame = _create_frame(exec_env, module_inst, entry, prev_frame);
        
        // restore frame
        _restore_frame(exec_env, frame, entry);
        
        prev_frame = frame;
    }
    
    // 最新のフレームを設定
    wasm_exec_env_set_cur_frame(exec_env, frame);
    wasmig_debug("restore frame\n");
}

wasm_restore_stack(WASMExecEnv **_exec_env)
{
    wasmig_log_init(1);
    wasmig_info("wasm_restore_stack\n");
    
    WASMExecEnv *exec_env = *_exec_env;
    WASMModuleInstance *module_inst = (WASMModuleInstance *)exec_env->module_inst;
    
    // コールスタックの復元
    CallStack cs = wasmig_restore_stack();
    wasmig_debug("restore_stack: cs.size: %d\n", cs.size);
    // print_call_stack(&cs);
    
    // 全フレームの復元
    _restore_all_frames(exec_env, module_inst, &cs);
    
    _exec_env = &exec_env;
    
    wasmig_info("Finish to restore stack\n");
    return wasm_exec_env_get_cur_frame(exec_env);
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
    Array8 mem = wasmig_restore_memory();

    // restore page_count
    uint32 page_count = mem.size / (*memory)->num_bytes_per_page;
    wasmig_debug("[Restore memory] page_count: %d", page_count);
    wasm_enlarge_memory(module, page_count- (*memory)->cur_page_count);
    *maddr = page_count * (*memory)->num_bytes_per_page;

    // restore data
    // NOTE: Can it replace memcpy to memmove?
    memcpy((*memory)->memory_data, mem.contents, mem.size);
    return 0;
}
// int wasm_restore_memory(WASMModuleInstance *module, WASMMemoryInstance **memory, uint8** maddr) {
//     FILE* memory_fp = wamr_open_image("memory.img", "rb");
//     FILE* mem_size_fp = wamr_open_image("mem_page_count.img", "rb");

//     // restore page_count
//     uint32 page_count;
//     fread(&page_count, sizeof(uint32), 1, mem_size_fp);
//     wasm_enlarge_memory(module, page_count- (*memory)->cur_page_count);
//     *maddr = page_count * (*memory)->num_bytes_per_page;

//     // restore_dirty_memory(memory, memory_fp);
//     // restore memory_data
//     fread((*memory)->memory_data, sizeof(uint8),
//             (*memory)->num_bytes_per_page * (*memory)->cur_page_count, memory_fp);

//     fclose(memory_fp);
//     fclose(mem_size_fp);
//     return 0;
// }

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
    CodePos pc = wasmig_restore_pc();
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