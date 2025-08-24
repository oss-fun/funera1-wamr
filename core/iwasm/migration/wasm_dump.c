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

// #define skip_leb(p) while (*p++ & 0x80)
#define skip_leb(p)                     \
    while (1) {                         \
        if (*p & 0x80)p++;              \
        else break;                     \
    }                                   \

int64_t get_time(struct timespec ts1, struct timespec ts2) {
  int64_t sec = ts2.tv_sec - ts1.tv_sec;
  int64_t nsec = ts2.tv_nsec - ts1.tv_nsec;
  // std::cerr << sec << ", " << nsec << std::endl;
  return sec * 1e9 + nsec;
}

/* common_functions */
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

// 積まれてるframe stackを出力する
void debug_frame_info(WASMExecEnv* exec_env, WASMInterpFrame *frame) {
    WASMModuleInstance *module = exec_env->module_inst;

    int cnt = 0;
    printf("=== DEBUG Frame Stack ===\n");
    do {
        cnt++;
        if (frame->function == NULL) {
            printf("%d) func_idx: -1\n", cnt);
        }
        else {
            printf("%d) func_idx: %d\n", cnt, frame->function - module->e->functions);
        }
    } while (frame = frame->prev_frame);
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



/* wasm_dump */
static CodePos _get_call_position(uint8 *frame_ip)
{
    uint32 fidx, offset;
    AddressMap metadata_address_map = wasmig_address_map_load();
    if (!wasmig_address_map_get_key(metadata_address_map, (uint64_t)(uintptr_t)frame_ip, &fidx, &offset)) {
        wasmig_error("address %p not found\n", (void*)frame_ip);
        return (CodePos){0, 0};
    }
    wasmig_debug("frame_ip: %p, fidx: %u, p_offset: %u\n", (void*)frame_ip, fidx, offset);
    return (CodePos){fidx, offset};
}

Array8 get_type_stack(uint32_t fidx, uint32_t _offset, bool is_top_frame) {

    uint32_t offset = (is_top_frame) ? _offset : _offset + 1;
    StackTable table = get_stack_table(fidx, offset);
    Array8 type_stack;
    type_stack.size = table.size;
    type_stack.contents = (uint8_t *)malloc(type_stack.size * sizeof(uint8_t));
    for (size_t i = 0; i < table.size; i++) {
      StackTableEntry entry = table.data[i];
      type_stack.contents[i] = entry.ty;
    }
    return type_stack;
}

// Calculate stack size from type stack
uint32 wamr_get_stack_size(Array8 type_stack) {
    uint32 size = 0;
    for (size_t i = 0; i < type_stack.size; i++) {
        if (type_stack.contents[i] > 4) {
            wasmig_error("Unknown type: %d\n", type_stack.contents[i]);
            return 0;
        }
        size += type_stack.contents[i];
    }
    return size;
}

static void
_setup_value_stacks(struct WASMInterpFrame *frame, CodePos call_pos, bool is_stack_top,
                   TypedArray *out_locals, TypedArray *out_value_stack)
{
    WASMFunctionInstance *func = frame->function;
    uint32 local_count = func->param_count + func->local_count;
    uint32 local_size = func->param_cell_num + func->local_cell_num;

    // StackTable stack_table = get_stack_table(call_pos.fidx, call_pos.offset);
    // uint32 value_stack_size = get_stack_size(stack_table);
    // コールスタックのトップ以外は引数・返り値の処理が必要
    // if (!is_stack_top) {
    //     uint32_t result_size = get_result_size(stack_table);    
    //     value_stack_size -= result_size;
    // }
    Array8 locals_types = get_local_types(call_pos.fidx);
    Array8 value_stack_types = get_type_stack(call_pos.fidx, call_pos.offset, is_stack_top);
    uint32 value_stack_size = wamr_get_stack_size(value_stack_types);
    uint8* type_buf = value_stack_types.contents;

    out_locals->types = locals_types;
    out_locals->values = (Array32){local_size, frame->lp};
    out_value_stack->types = value_stack_types; 
    out_value_stack->values = (Array32){value_stack_size, frame->sp_bottom};
    wasmig_info("locals: {count=%d, size=%d}\n", local_count, local_size);
    wasmig_info("value_stack: {count=%d, size=%d}\n", value_stack_types.size, value_stack_size);

    // locals->size = local_size;
    // locals->contents = frame->lp;
    
    // value_stack->size = value_stack_size;
    // value_stack->contents = frame->sp_bottom;
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
    CodePos call_pos = _get_call_position(frame->ip);
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
// int wasm_dump_memory(WASMMemoryInstance *memory) {
//     FILE *mem_size_fp = wamr_open_image("mem_page_count.img", "wb");

//     // dump_dirty_memory(memory);

//     printf("page_count: %d\n", memory->cur_page_count);
//     fwrite(&(memory->cur_page_count), sizeof(uint32), 1, mem_size_fp);

//     fclose(mem_size_fp);

//     // デバッグのために、すべてのメモリも保存
//     FILE *all_memory_fp = wamr_open_image("all_memory.img", "wb");
//     fwrite(memory->memory_data, sizeof(uint8),
//            memory->num_bytes_per_page * memory->cur_page_count, all_memory_fp);
//     fclose(all_memory_fp);
//     return 0;
// }

int wasm_dump_global(WASMModuleInstance *module, WASMGlobalInstance *globals, uint8* global_data) {
    uint64_t values[module->e->global_count];
    uint32_t types[module->e->global_count];
    uint8 *global_addr;
    for (int i = 0; i < module->e->global_count; i++) {
        switch (globals[i].type) {
            case VALUE_TYPE_I32:
            case VALUE_TYPE_F32:
                values[i] = *get_global_addr_for_migration(global_data, (globals+i));
                types[i] = sizeof(uint32);
                break;
            case VALUE_TYPE_I64:
            case VALUE_TYPE_F64:
                values[i] = *get_global_addr_for_migration(global_data, (globals+i));
                types[i] = sizeof(uint64);
                break;
            default:
                printf("type error:B\n");
                break;
        }
    }

    wasmig_checkpoint_global(values, types, module->e->global_count);
}
// int wasm_dump_global(WASMModuleInstance *module, WASMGlobalInstance *globals, uint8* global_data) {
//     FILE *fp;
//     const char *file = "global.img";
//     fp = fopen(file, "wb");
//     if (fp == NULL) {
//         fprintf(stderr, "failed to open %s\n", file);
//         return -1;
//     }

//     // WASMMemoryInstance *memory = module->default_memory;
//     uint8 *global_addr;
//     for (int i = 0; i < module->e->global_count; i++) {
//         switch (globals[i].type) {
//             case VALUE_TYPE_I32:
//             case VALUE_TYPE_F32:
//                 global_addr = get_global_addr_for_migration(global_data, (globals+i));
//                 fwrite(global_addr, sizeof(uint32), 1, fp);
//                 break;
//             case VALUE_TYPE_I64:
//             case VALUE_TYPE_F64:
//                 global_addr = get_global_addr_for_migration(global_data, (globals+i));
//                 fwrite(global_addr, sizeof(uint64), 1, fp);
//                 break;
//             default:
//                 printf("type error:B\n");
//                 break;
//         }
//     }

//     fclose(fp);
//     return 0;
// }

int wasm_dump_program_counter(
    WASMModuleInstance *module,
    WASMFunctionInstance *func,
    uint8 *frame_ip
)
{
    CodePos pc = _get_call_position(frame_ip);
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
