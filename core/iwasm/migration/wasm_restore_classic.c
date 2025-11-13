#include <stdio.h>
#include <stdlib.h>

#include "../common/wasm_exec_env.h"
#include "../common/wasm_memory.h"
#include "../interpreter/wasm_runtime.h"
#include "wasm_migration.h"
#include "wasm_restore.h"
#include "wasm_migration_helper.h"
#include <wasmig/migration.h>
#include <wasmig/log.h>
#include <wasmig/table_v3.h>
#include <wasmig/registry.h>

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

#if WASM_ENABLE_FAST_INTERP == 0
static void
_restore_program_counter(WASMInterpFrame *frame, CallStackEntry *entry)
{
    CodePos ret_pos = entry->pc;
    frame->ip = get_call_address(ret_pos.fidx, ret_pos.offset);
    wasmig_debug("restore ip: (%d, %d)\n", entry->pc.fidx, entry->pc.offset);
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
_restore_label_stack_v1(WASMInterpFrame *frame, WASMCSPFrame *csp_frame)
{
    // ラベルスタックのサイズ設定
    CallStackEntry *entry = &csp_frame->entry;
    uint32 ctrl_stack_size = entry->label_stack.size;
    if (ctrl_stack_size != csp_frame->csp_size) {
        wasmig_error("control stack size mismatch: (expect=%d, actual=%d)",
                     ctrl_stack_size, csp_frame->csp_size);
    } else {
        wasmig_info("control stack size match: (expect=%d, actual=%d)",
                     ctrl_stack_size, csp_frame->csp_size);
    }
    frame->csp = frame->csp_bottom + ctrl_stack_size;

    // ラベルスタックの復元
    WASMBranchBlock *csp = frame->csp_bottom;
    for (int i = 0; i < ctrl_stack_size; ++i, ++csp) {
        uint64 offset;
        CSPEntry *csp_entry = &csp_frame->csp[i];

        // begin_addr の復元
        offset = entry->label_stack.begins[i];
        csp->begin_addr = set_addr_offset(wasm_get_func_code(frame->function), offset);
        if (csp->begin_addr != csp_entry->begin_addr) {
            wasmig_error("control stack begin_addr mismatch: (actual=%p, expect=%p)",
                         csp_entry->begin_addr, csp->begin_addr);
        }

        // target_addr の復元
        offset = entry->label_stack.targets[i];
        csp->target_addr = set_addr_offset(wasm_get_func_code(frame->function), offset);
        if (csp->target_addr != csp_entry->target_addr) {
            wasmig_error("control stack target_addr mismatch: (actual=%p, expect=%p)",
                         csp_entry->target_addr, csp->target_addr);
        } else {
            wasmig_info("control stack target_addr match: (actual=%p, expect=%p)",
                         csp_entry->target_addr, csp->target_addr);
        }

        // frame_sp の復元
        offset = entry->label_stack.stack_pointers[i];
        csp->frame_sp = set_addr_offset(frame->sp_bottom, offset);
        if (offset != csp_entry->sp_offset) {
            wasmig_error("control stack sp_offset mismatch: (actual=%d, expect=%d)",
                         csp_entry->sp_offset, offset);
        } else {
            wasmig_info("control stack sp_offset match: (actual=%d, expect=%d)",
                         csp_entry->sp_offset, offset);
        }

        // cell_num の復元
        offset = entry->label_stack.cell_nums[i];
        csp->cell_num = offset;
        if (csp->cell_num != csp_entry->cell_num) {
            wasmig_error("control stack cell_num mismatch: (actual=%d, expect=%d)",
                         csp_entry->cell_num, csp->cell_num);
        } else {
            wasmig_info("control stack cell_num match: (actual=%d, expect=%d)",
                         csp_entry->cell_num, csp->cell_num);
        }
    }
    wasmig_info("Correct restore label stack");
    wasmig_info("restore label stack");
}

// restore label stack without dumped state
static void
_restore_label_stack_v2(WASMInterpFrame *frame, WASMCSPFrame *csp_frame)
{
    // ラベルスタックのサイズ設定
    uint32 ctrl_stack_size = csp_frame->csp_size;
    frame->csp = frame->csp_bottom + ctrl_stack_size;

    // ラベルスタックの復元
    WASMBranchBlock *csp = frame->csp_bottom;
    for (int i = 0; i < ctrl_stack_size; ++i, ++csp) {
        uint64 offset;
        CSPEntry *csp_entry = &csp_frame->csp[i];

        // begin_addr の復元
        csp->begin_addr = csp_entry->begin_addr;

        // target_addr の復元
        csp->target_addr = csp_entry->target_addr;

        // frame_sp の復元
        csp->frame_sp = set_addr_offset(frame->sp_bottom, csp_entry->sp_offset);

        // cell_num の復元
        csp->cell_num = csp_entry->cell_num;
    }
    wasmig_info("restore label stack");
}

static void
_restore_frame(WASMExecEnv *exec_env, WASMInterpFrame *frame, WASMCSPFrame *csp)
{
    WASMModuleInstance *module_inst = exec_env->module_inst;
    WASMFunctionInstance *func = frame->function;

    // restore a program counter
    _restore_program_counter(frame, &csp->entry);

    // Initialize frame boundaries
    _initialize_frame_boundaries(frame, func);

    // restore locals and value stack
    _restore_value_stacks(frame, func, &csp->entry);

    // restore label stack
    _restore_label_stack_v2(frame, csp);
}

// Allocate frame
static WASMInterpFrame *
_create_frame(WASMExecEnv *exec_env, WASMModuleInstance *module_inst, 
              CodePos pc, WASMInterpFrame *prev_frame)
{
    WASMFunctionInstance *function = module_inst->e->functions + pc.fidx;
    
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
_restore_all_frames(WASMExecEnv *exec_env, WASMModuleInstance *module_inst, WASMCSPFrameStack *cs)
{
    WASMInterpFrame *frame, *prev_frame = wasm_exec_env_get_cur_frame(exec_env);

    // Iterate call stack entries
    for (int i = 0; i < cs->size; i++) {
        WASMCSPFrame *csp_frame = &cs->frames[i];
        // CallStackEntry *entry = &cs->frames[i].entry;
        
        // allocate frame
        frame = _create_frame(exec_env, module_inst, csp_frame->entry.pc, prev_frame);
        
        // restore frame
        _restore_frame(exec_env, frame, csp_frame);
        
        prev_frame = frame;
    }
    
    // 最新のフレームを設定
    wasm_exec_env_set_cur_frame(exec_env, frame);
    wasmig_debug("restore frame\n");
}

WASMInterpFrame*
wasm_restore_stack(WASMExecEnv **_exec_env)
{
    wasmig_info("wasm_restore_stack\n");
    
    WASMExecEnv *exec_env = *_exec_env;
    WASMModuleInstance *module_inst = (WASMModuleInstance *)exec_env->module_inst;
    
    // コールスタックの復元
    // CallStack cs = wasmig_restore_stack();
    WASMCSPFrameStack* cs = load_wasm_call_stack();
    if (cs == NULL) {
        wasmig_error("Failed to load call stack");
        return NULL;
    }
    wasmig_debug("restore_stack: cs.size: %d\n", cs->size);
    // print_call_stack(&cs);
    
    // 全フレームの復元
    _restore_all_frames(exec_env, module_inst, cs);
    
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
    *frame_ip = get_call_address(pc.fidx, pc.offset);

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
#endif // end of WASM_ENABLE_FAST_INTERP != 0  