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

#if WASM_ENABLE_FAST_INTERP != 0
static inline void
word_copy(uint32 *dest, uint32 *src, unsigned num)
{
    bh_assert(dest != NULL);
    bh_assert(src != NULL);
    bh_assert(num > 0);
    if (dest != src) {
        /* No overlap buffer */
        bh_assert(!((src < dest) && (dest < src + num)));
        for (; num > 0; num--)
            *dest++ = *src++;
    }
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
_restore_program_counter(WASMInterpFrame *frame, CallStackEntry *entry)
{
    CodePos ret_pos = entry->pc;
    frame->ip = get_call_address(ret_pos.fidx, ret_pos.offset);
    // wasmig_debug("restore ip: (%d, %d)\n", entry->pc.fidx, entry->pc.offset);
}

// Initialize stack and call stack boundaries
// static void
// _initialize_frame_boundaries(WASMInterpFrame *frame, WASMFunctionInstance *func)
// {
//     frame->lp =
//         frame->operand + func->const_cell_num;
// }
// 

static bool 
rematerialize_stack_values(Stack addr_stack, Stack type_stack, Array32 stack, uint32** out_sp) {
    uint32 stack_ptr = 0;
    StackIterator addr_it = wasmig_stack_iterator_create(addr_stack);
    StackIterator type_it = wasmig_stack_iterator_create(type_stack);
    if (!addr_it || !type_it) {
        wasmig_error("failed to create stack iterators\n");
        if (addr_it) wasmig_stack_iterator_destroy(addr_it);
        if (type_it) wasmig_stack_iterator_destroy(type_it);
        return false;
    }

    uint32 stack_size = stack.size;
    uint32* value_buf = stack.contents;
    stack_ptr = stack_size;
    uint32 index = 0;
    while (wasmig_stack_iterator_has_next(addr_it) && wasmig_stack_iterator_has_next(type_it)) {
        index++;
        uint64_t address = wasmig_stack_iterator_next(addr_it);
        uint32 type = (uint32)wasmig_stack_iterator_next(type_it);
        stack_ptr -= type;

        switch (type) {
            case 1: // i32
            {
                // wasmig_debug("reconstruct stack[%u]: i32 %u\n", stack_ptr, (uint32)address);
                // value_buf[stack_ptr] = (uint32)sp[(size_t)address];
                (*out_sp)[(size_t)address] = (uint32)value_buf[stack_ptr];
                break;
            }
            case 2: // i64
            {
                // wasmig_debug("reconstruct stack[%u]: i64 %" PRIu64 "\n", stack_ptr, (uint64_t)address);
                (*out_sp)[(size_t)address] = (uint32)value_buf[stack_ptr];
                (*out_sp)[(size_t)address + 1] = (uint32)value_buf[stack_ptr+1];
                break;
            }
            default:
                wasmig_error("unknown type: %d\n", type);
                break;
        }
    }
    wasmig_stack_iterator_destroy(addr_it);
    wasmig_stack_iterator_destroy(type_it);
    return true;
}

Array32 merge_locals_and_value_stack(TypedArray locals, TypedArray value_stack) {
    uint32 total_size = locals.values.size + value_stack.values.size;
    uint32* merged_contents = malloc(total_size * sizeof(uint32));
    if (!merged_contents) {
        wasmig_error("failed to allocate memory for merged stack\n");
        return (Array32){0, NULL};
    }

    memcpy(merged_contents, locals.values.contents, locals.values.size * sizeof(uint32));
    memcpy(merged_contents + locals.values.size, value_stack.values.contents,
           value_stack.values.size * sizeof(uint32));

    return (Array32){total_size, merged_contents};
}

static void
_restore_value_stacks(WASMInterpFrame *frame, WASMFunctionInstance *func, CallStackEntry *entry, CodePos pc, bool is_stack_top)
{
    Stack addr_stack, type_stack;
    uint32 fidx = pc.fidx;
    uint32 offset = is_stack_top ? pc.offset : pc.offset + 1;

    if (!load_metadata_stacks(fidx, offset, &addr_stack, &type_stack)) {
        wasmig_error("failed to load metadata stacks\n");
        return false;
    }

    Array32 stack = merge_locals_and_value_stack(entry->locals, entry->value_stack);
    uint32** sp = &frame->lp;
    if (!rematerialize_stack_values(addr_stack, type_stack, stack, sp)) {
        wasmig_error("failed to rematerialize stack values\n");
        return false;
    }
    
    return true;
}

static void
_restore_frame(WASMExecEnv *exec_env, WASMInterpFrame *frame, WASMCSPFrame *csp, bool is_stack_top)
{
    WASMModuleInstance *module_inst = exec_env->module_inst;
    WASMFunctionInstance *func = frame->function;

    // restore a program counter
    _restore_program_counter(frame, &csp->entry);

    // Initialize frame boundaries
    // _initialize_frame_boundaries(frame, func);

    // restore locals and value stack
    _restore_value_stacks(frame, func, &csp->entry, csp->entry.pc, is_stack_top);
}

// Allocate frame
static WASMInterpFrame *
_create_frame(WASMExecEnv *exec_env, WASMModuleInstance *module_inst, 
              CodePos pc, WASMInterpFrame *prev_frame)
{
    WASMFunctionInstance *cur_func = module_inst->e->functions + pc.fidx;
    
    // Calculate frame size
    WASMFunction *cur_wasm_func = cur_func->u.func;
    uint32 all_cell_num = cur_func->param_cell_num + cur_func->local_cell_num
                           + cur_func->const_cell_num
                           + cur_wasm_func->max_stack_cell_num;
    uint32 frame_size = wasm_interp_interp_frame_size(all_cell_num);
    
    // Allocate this frame
    WASMInterpFrame *frame;
    if (!(frame = wasm_alloc_frame(exec_env, frame_size, prev_frame))) {
        frame = prev_frame;
        wasmig_error("Failed to allocate frame");
        exit(1);
    }
    frame->function = cur_func;
    frame->lp = frame->operand + cur_func->const_cell_num;

    /* Initialize the consts */
    if (cur_wasm_func->const_cell_num > 0) {
        word_copy(frame->operand, (uint32 *)cur_wasm_func->consts,
                  cur_wasm_func->const_cell_num);
    }

    /* Initialize the local variables */
    memset(frame->lp + cur_func->param_cell_num, 0,
           (uint32)(cur_func->local_cell_num * 4));
    
    printf("Allocated frame for function index: %d, frame address: %p\n", pc.fidx, frame);
    return frame;
}

static void
_restore_all_frames(WASMExecEnv *exec_env, WASMModuleInstance *module_inst, WASMCSPFrameStack *cs)
{
    WASMInterpFrame *frame, *prev_frame = wasm_exec_env_get_cur_frame(exec_env);

    // Iterate call stack entries
    for (int i = 0; i < cs->size; i++) {
        WASMCSPFrame *csp_frame = &cs->frames[i];
        
        // allocate frame
        frame = _create_frame(exec_env, module_inst, csp_frame->entry.pc, prev_frame);
        
        // restore frame
        bool is_stack_top = (i == cs->size - 1);
        _restore_frame(exec_env, frame, csp_frame, is_stack_top);
        
        prev_frame = frame;
    }
    
    // 最新のフレームを設定
    wasm_exec_env_set_cur_frame(exec_env, frame);
    // wasmig_debug("restore frame\n");
}

void
wasm_restore_stack(WASMExecEnv **_exec_env)
{
    // wasmig_info("wasm_restore_stack\n");
    
    WASMExecEnv *exec_env = *_exec_env;
    WASMModuleInstance *module_inst = (WASMModuleInstance *)exec_env->module_inst;
    
    // コールスタックの復元
    // CallStack cs = wasmig_restore_stack();
    WASMCSPFrameStack* cs = load_wasm_call_stack();
    if (cs == NULL) {
        wasmig_error("Failed to load call stack");
        return NULL;
    }
    // wasmig_debug("restore_stack: cs.size: %d\n", cs->size);
    // print_call_stack(&cs);
    
    // 全フレームの復元
    _restore_all_frames(exec_env, module_inst, cs);
    
    _exec_env = &exec_env;
    
    // wasmig_info("Finish to restore stack\n");
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
    // wasmig_debug("[Restore memory] page_count: %d", page_count);
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