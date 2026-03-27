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
#include "wasm_migration_helper.h"

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


// Get fidx and offset from the code addres by metadata address map
CodePos get_call_position(uint8 *frame_ip)
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

uint64 get_call_address(uint32 fidx, uint32 offset)
{
    AddressMap address_map = wasmig_address_map_load();

    uint64_t pc_value = 0;
    if (!wasmig_address_map_get_value(address_map, fidx, offset, &pc_value)) {
        wasmig_error("Failed to get key from address map");
        return -1;
    }
    return pc_value;
}

// Get type stack from 'stack-table.msgpack'
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


#if WASM_ENABLE_FAST_INTERP == 0
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
#endif

static WASMCSPFrameStack* WASMCallStack;
WASMCSPFrameStack* load_wasm_call_stack() {
    return WASMCallStack;
}

// Clone CSPEntry array
CSPEntry* csp_entry_clone(CSPEntry* src, uint32 csp_height) {
    if (!src) return NULL;

    CSPEntry* dst = malloc(sizeof(CSPEntry) * csp_height);
    if (!dst) return NULL;

    memcpy(dst, src, sizeof(CSPEntry) * csp_height);
    return dst;
}

void store_wasm_call_stack(WASMCSPFrameStack* stack) {
    WASMCallStack = stack;
}
