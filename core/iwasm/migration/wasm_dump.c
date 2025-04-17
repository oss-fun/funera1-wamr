#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <wasmig/migration.h>

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
static void
_dump_stack(WASMExecEnv *exec_env, struct WASMInterpFrame *frame, uint32 call_stack_id)
{
    int i;
    WASMModuleInstance *module = exec_env->module_inst;

    // リターンアドレス
    // NOTE: 1番下のframeのときだけ、prev_frameではなくframeのリターンアドレスを出力する
    WASMInterpFrame* prev_frame = (frame->prev_frame->function ? frame->prev_frame : frame);
    CodePos ret_addr;
    ret_addr.fidx = prev_frame->function - module->e->functions;
    ret_addr.offset = prev_frame->ip - wasm_get_func_code(prev_frame->function);

    // 型スタックの中身
    uint32 type_stack_size_from_file;
    CodePos cur_addr;
    cur_addr.fidx = frame->function - module->e->functions;
    cur_addr.offset = frame->ip - wasm_get_func_code(frame->function);

    // 値スタックの中身
    WASMFunctionInstance *func = frame->function;
    uint32 local_size = func->param_count + func->local_count;
    uint32 value_stack_size = frame->sp - frame->sp_bottom;
    Array32 locals = {
        .size = local_size,
        .contents = frame->lp,
    };
    Array32 value_stack = {
        .size = value_stack_size,
        .contents = frame->sp_bottom,
    };

    // ラベルスタックの中身
    uint32 ctrl_stack_size = frame->csp - frame->csp_bottom;
    uint32_t begins[ctrl_stack_size];
    uint32_t targets[ctrl_stack_size];
    uint32_t stack_pointers[ctrl_stack_size];
    uint32_t cell_nums[ctrl_stack_size];

    WASMBranchBlock *csp = frame->csp_bottom;
    uint32 addr;
    uint8* ip_start = wasm_get_func_code(frame->function);
    for (i = 0; i < ctrl_stack_size; ++i, ++csp) {
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

    // dump stack
    uint32 entry_fidx = frame->function - module->e->functions;
    bool is_top = (bool)(call_stack_id == 1);
    checkpoint_stack(call_stack_id, entry_fidx, &cur_addr, &ret_addr, &locals, &value_stack, &labels, is_top);
}


int
wasm_dump_stack(WASMExecEnv *exec_env, struct WASMInterpFrame *frame)
{
    WASMModuleInstance *module =
        (WASMModuleInstance *)exec_env->module_inst;

    // frameをtopからbottomまで走査する
    int i = 0;
    do {
        // dummy framenならbreak
        if (frame->function == NULL) break;
        ++i;
        _dump_stack(exec_env, frame, i);
    } while(frame = frame->prev_frame);

    // frame stackのサイズを保存
    checkpoint_call_stack_size(i);

    return 0;
}


int wasm_dump_memory(WASMMemoryInstance *memory) {
    checkpoint_memory(memory->memory_data, memory->cur_page_count);
}

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

    checkpoint_global(values, types, module->e->global_count);
}

int wasm_dump_program_counter(
    WASMModuleInstance *module,
    WASMFunctionInstance *func,
    uint8 *frame_ip
)
{
    uint32 fidx, p_offset;
    fidx = func - module->e->functions;
    p_offset = frame_ip - wasm_get_func_code(func);

    checkpoint_pc(fidx, p_offset);
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
