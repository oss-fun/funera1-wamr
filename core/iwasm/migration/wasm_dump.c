#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <wasmig/migration.h>
#include <wasmig/stack_tables.h>
#include <wasmig/log.h>
#include <wasmig/table_v3.h>
#include <wasmig/registry.h>
#include <wasmig/state.h>

#include "../interpreter/wasm_runtime.h"
#include "wasm_migration.h"
#include "wasm_dump.h"
#include "wasm_dispatch.h"
#include "helper.h"

// #define skip_leb(p) while (*p++ & 0x80)
#define skip_leb(p)                     \
    while (1) {                         \
        if (*p & 0x80)p++;              \
        else break;                     \
    }                                   \


/* wasm_dump */

static void
_setup_value_stacks(struct WASMInterpFrame *frame, CodePos call_pos, bool is_stack_top,
                   TypedArray *out_locals, TypedArray *out_value_stack)
{
    WASMFunctionInstance *func = frame->function;
    uint32 local_count = func->param_count + func->local_count;
    uint32 local_size = func->param_cell_num + func->local_cell_num;

    // get states
    Array8 locals_types = get_local_types(call_pos.fidx);
    Array8 value_stack_types = get_type_stack(call_pos.fidx, call_pos.offset, is_stack_top);
    uint32 value_stack_size = wamr_get_stack_size(value_stack_types);
    uint8* type_buf = value_stack_types.contents;

    // restore stack
    out_locals->types = locals_types;
    out_locals->values = (Array32){local_size, frame->lp};
    out_value_stack->types = value_stack_types; 
    out_value_stack->values = (Array32){value_stack_size, frame->sp_bottom};
    
    // print log
    wasmig_info("locals: {count=%d, size=%d}\n", local_count, local_size);
    wasmig_info("value_stack: {count=%d, size=%d}\n", value_stack_types.size, value_stack_size);
}

static LabelStack
_setup_label_stack(struct WASMInterpFrame *frame)
{
    uint32 ctrl_stack_size = frame->csp - frame->csp_bottom;
    uint32_t* begins = (uint32_t *)malloc(ctrl_stack_size * sizeof(uint32_t));
    uint32_t* targets = (uint32_t *)malloc(ctrl_stack_size * sizeof(uint32_t));
    uint32_t* stack_pointers = (uint32_t *)malloc(ctrl_stack_size * sizeof(uint32_t));
    uint32_t* cell_nums = (uint32_t *)malloc(ctrl_stack_size * sizeof(uint32_t));

    WASMBranchBlock *csp = frame->csp_bottom;
    uint8* ip_start = wasm_get_func_code(frame->function);
    for (int i = 0; i < ctrl_stack_size; ++i, ++csp) {
        begins[i] = get_addr_offset(csp->begin_addr, ip_start);
        targets[i] = get_addr_offset(csp->target_addr, ip_start);
        stack_pointers[i] = get_addr_offset(csp->frame_sp, frame->sp_bottom);
        cell_nums[i] = csp->cell_num;
    }

    LabelStack labels;
    labels.size = ctrl_stack_size;
    labels.begins = begins;
    labels.targets = targets;
    labels.stack_pointers = stack_pointers;
    labels.cell_nums = cell_nums;
    
    return labels;
}

_dump_stack(WASMExecEnv *exec_env, struct WASMInterpFrame *frame, uint32 call_stack_id, CallStackEntry *entry, bool is_stack_top)
{
    WASMModuleInstance *module = exec_env->module_inst;

    // プログラムカウンタの処理
    CodePos call_pos = get_call_position(frame->ip);
    wasmig_debug("call_stack_id: %d, fidx: %d, offset: %d\n", call_stack_id, call_pos.fidx, call_pos.offset);

    // 値スタックの設定
    TypedArray locals, value_stack;
    _setup_value_stacks(frame, call_pos, is_stack_top, &locals, &value_stack);

    // ラベルスタックの設定
    LabelStack labels = _setup_label_stack(frame);

    // エントリに情報を設定
    entry->pc = call_pos;
    entry->locals = locals;
    entry->value_stack = value_stack;
    entry->label_stack = labels;
}


wasm_dump_stack(WASMExecEnv *exec_env, struct WASMInterpFrame *frame)
{
    wasmig_log_init(1);
    WASMModuleInstance *module =
        (WASMModuleInstance *)exec_env->module_inst;

    // Call Stackのサイズを取得
    int call_stack_size = 0;
    struct WASMInterpFrame *cur_frame = frame;
    do {
        if (cur_frame->function == NULL) break;
        call_stack_size++;
    } while(cur_frame = cur_frame->prev_frame);

    // frameをtopからbottomまで走査する
    CallStackEntry entries[call_stack_size];
    cur_frame = frame;
    for (int i = 0; i < call_stack_size; i++) {
        // dump_stackは上から順に呼ばれるので、entryは下から順に格納する
        _dump_stack(exec_env, cur_frame, i, &entries[call_stack_size-i-1], (i == 0));
        cur_frame = cur_frame->prev_frame;
    };

    // frame stackのサイズを保存
    wasmig_checkpoint_stack_v4(call_stack_size, entries);
    wasmig_info("Success to dump frame stack\n");

    return 0;
}


int wasm_dump_memory(WASMMemoryInstance *memory) {
    wasmig_checkpoint_memory(memory->memory_data, memory->cur_page_count);
}

int wasm_dump_global(WASMModuleInstance *module, WASMGlobalInstance *globals, uint8* global_data) {
    uint64_t values[module->e->global_count];
    uint32_t types[module->e->global_count];
    uint8 *global_addr;
    for (int global_idx = 0; global_idx < module->e->global_count; global_idx++) {
        switch (globals[global_idx].type) {
            case VALUE_TYPE_I32:
            case VALUE_TYPE_F32:
                global_addr = get_global_addr_for_migration(global_data, (globals+global_idx));
                values[global_idx] = (*(uint32 *)global_addr);
                types[global_idx] = sizeof(uint32);
                break;
            case VALUE_TYPE_I64:
            case VALUE_TYPE_F64:
                global_addr = get_global_addr_for_migration(global_data, (globals+global_idx));
                values[global_idx] = (*(uint64 *)global_addr);
                types[global_idx] = sizeof(uint64);
                break;
            default:
                printf("type error:B\n");
                break;
        }
    }

    wasmig_checkpoint_global(values, types, module->e->global_count);
}

int wasm_dump_program_counter(
    WASMModuleInstance *module,
    WASMFunctionInstance *func,
    uint8 *frame_ip
)
{
    CodePos pc = get_call_position(frame_ip);
    return wasmig_checkpoint_pc(pc.fidx, pc.offset);
}

int wasm_dump(WASMExecEnv *exec_env,
         WASMModuleInstance *module,
         WASMMemoryInstance *memory,
         WASMGlobalInstance *globals,
         uint8 *global_data,
         WASMFunctionInstance *cur_func,
         struct WASMInterpFrame *frame,
         register uint8 *frame_ip)
{
    int rc;
    struct timespec ts1, ts2;

    // dump linear memory
    clock_gettime(CLOCK_MONOTONIC, &ts1);
    rc = wasm_dump_memory(memory);
    clock_gettime(CLOCK_MONOTONIC, &ts2);
    fprintf(stderr, "memory, %lu\n", get_time(ts1, ts2));
    if (rc < 0) {
        LOG_ERROR("Failed to dump linear memory\n");
        return rc;
    }

    // dump globals
    clock_gettime(CLOCK_MONOTONIC, &ts1);
    rc = wasm_dump_global(module, globals, global_data);
    clock_gettime(CLOCK_MONOTONIC, &ts2);
    fprintf(stderr, "global, %lu\n", get_time(ts1, ts2));
    if (rc < 0) {
        LOG_ERROR("Failed to dump globals\n");
        return rc;
    }

    // dump program counter
    clock_gettime(CLOCK_MONOTONIC, &ts1);
    rc = wasm_dump_program_counter(module, cur_func, frame_ip);
    clock_gettime(CLOCK_MONOTONIC, &ts2);
    fprintf(stderr, "program counter, %lu\n", get_time(ts1, ts2));
    if (rc < 0) {
        LOG_ERROR("Failed to dump program_counter\n");
        return rc;
    }

    // dump stack
    clock_gettime(CLOCK_MONOTONIC, &ts1);
    rc = wasm_dump_stack(exec_env, frame);
    clock_gettime(CLOCK_MONOTONIC, &ts2);
    fprintf(stderr, "stack, %lu\n", get_time(ts1, ts2));
    if (rc < 0) {
        LOG_ERROR("Failed to dump frame\n");
        return rc;
    }

    LOG_VERBOSE("Success to dump img for wamr\n");
    return 0;
}
