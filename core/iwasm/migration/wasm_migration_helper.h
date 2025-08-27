/*
 * Copyright (C) 2019 Intel Corporation.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#ifndef _WASM_MIGRATION_HELPER_H
#define _WASM_MIGRATION_HELPER_H

#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <wasmig/state.h>
#include "../interpreter/wasm_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Time utility functions
 */

/**
 * Calculate time difference in nanoseconds between two timespec structures
 * @param ts1 start time
 * @param ts2 end time 
 * @return time difference in nanoseconds
 */
int64_t get_time(struct timespec ts1, struct timespec ts2);

/*
 * File I/O utility functions  
 */

/**
 * Write data to file stream
 * @param ptr pointer to data to write
 * @param size size of each element
 * @param nmemb number of elements
 * @param stream file stream to write to
 * @return number of elements written, or -1 on error
 */
int dump_value(void *ptr, size_t size, size_t nmemb, FILE *stream);

/*
 * Debug utility functions
 */

/**
 * Debug and print memory instance information
 * @param module WASM module instance
 * @return 0 on success
 */
int debug_memories(WASMModuleInstance *module);

/**
 * Debug and print frame stack information
 * @param exec_env execution environment
 * @param frame current frame
 */
void debug_frame_info(WASMExecEnv* exec_env, WASMRuntimeFrame *frame);

/**
 * Debug and print function opcodes
 * @param module WASM module instance
 * @param func function instance
 * @param limit maximum number of opcodes to print
 * @return 0 on success, -1 on error
 */
int debug_function_opcodes(WASMModuleInstance *module, WASMFunctionInstance* func, uint32 limit);

/*
 * Stack table utility functions
 */

// Get fidx and offset from the code addres by metadata address map
CodePos get_call_position(uint8 *frame_ip);

// Get code address from the call position by metadata address map
uint64 get_call_address(uint32 fidx, uint32 offset);

/**
 * Get type stack from stack table
 * @param fidx function index
 * @param _offset instruction offset
 * @param is_top_frame whether this is the top frame
 * @return Array8 containing type stack
 */
Array8 get_type_stack(uint32_t fidx, uint32_t _offset, bool is_top_frame);

/**
 * Calculate stack size from type stack
 * @param type_stack array of types
 * @return calculated stack size
 */
uint32 wamr_get_stack_size(Array8 type_stack);

typedef struct CSPEntry {
    uint8 label_type;
    uint8* begin_addr;
    uint8* target_addr;
    uint32 sp_offset;
    uint32 cell_num;
} CSPEntry;

typedef struct WASMCSPFrame {
    CallStackEntry entry;
    uint32 csp_size;
    CSPEntry* csp;
} WASMCSPFrame;

typedef struct WASMCSPFrameStack {
    uint32 size;
    WASMCSPFrame* frames;
} WASMCSPFrameStack;

WASMCSPFrameStack* load_wasm_call_stack();
bool store_wasm_call_stack(WASMCSPFrameStack* stack, int size);

CSPEntry* csp_entry_clone(CSPEntry* src, uint32 csp_height);

#ifdef __cplusplus
}
#endif

#endif /* end of _WASM_MIGRATION_HELPER_H */
