#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

#include "../interpreter/wasm_runtime.h"
#include "wasm_migration.h"
#include "wasm_dump.h"
#include "wasm_dispatch.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_attr.h"

#define MOUNT_POINT ""
#define CHECKPOINT_DIR ""
#define CHECKPOINT_PATH MOUNT_POINT CHECKPOINT_DIR
#define BH_PLATFORM_LINUX 0

#define DUMP_PAGE_SIZE (16 * 1024)  // 16KB chunks
#define BUFFER_SIZE (32 * 1024)     // 32KB buffer


static const char *TAG = "wasm-dump";

static void dump_file_contents(const char* filepath, size_t max_bytes) {
    FILE* fp = fopen(filepath, "rb");
    if (!fp) {
        ESP_LOGE(TAG, "Failed to open file for debug: %s", filepath);
        return;
    }

    uint8_t* buffer = malloc(max_bytes);
    if (!buffer) {
        ESP_LOGE(TAG, "Failed to allocate buffer for debug");
        fclose(fp);
        return;
    }

    size_t bytes_read = fread(buffer, 1, max_bytes, fp);
    ESP_LOGI(TAG, "File %s contents (first %zu bytes):", filepath, bytes_read);
    for (size_t i = 0; i < bytes_read; i++) {
        printf("%02x ", buffer[i]);
        if ((i + 1) % 16 == 0) printf("\n");
    }
    printf("\n");

    free(buffer);
    fclose(fp);
}

// Helper structs
struct TimeMetrics {
    struct timeval start;
    struct timeval end;
};

// Time measurement functions
static inline void start_measurement(struct TimeMetrics* metrics) {
    gettimeofday(&metrics->start, NULL);
}

static inline uint64_t end_measurement(struct TimeMetrics* metrics) {
    gettimeofday(&metrics->end, NULL);
    return (metrics->end.tv_sec - metrics->start.tv_sec) * 1000000LL + 
           (metrics->end.tv_usec - metrics->start.tv_usec);
}

/* Memory page helper functions */
int is_dirty(uint64 pagemap_entry) {
    return (pagemap_entry>>62&1) | (pagemap_entry>>63&1);
}

int is_soft_dirty(uint64 pagemap_entry) {
    return (pagemap_entry >> 55 & 1);
}

int get_pagemap(unsigned long memory_addr) {
    int fd;
    fd = open("/proc/self/pagemap", O_RDONLY);
    if (fd == -1) {
        ESP_LOGE(TAG, "Error opening pagemap");
        return -1;
    }

    unsigned long pfn = memory_addr / WASM_PAGE_SIZE;
    off_t offset = sizeof(uint64) * pfn;
    if (lseek(fd, offset, SEEK_SET) == -1) {
        ESP_LOGE(TAG, "Error seeking to pagemap entry");
        close(fd);
        return -1;
    }
    return fd;
}

// wasm_dump.c
uint8* get_type_stack(uint32 fidx, uint32 offset, uint32* type_stack_size, bool is_return_address) {
    FILE *tablemap_func = fopen("FUNC", "rb");
    if (!tablemap_func) printf("not found tablemap_func\n");
    FILE *tablemap_offset = fopen("OFFSET", "rb");
    if (!tablemap_func) printf("not found tablemap_offset\n");
    FILE *type_table = fopen("TYPE", "rb");
    if (!tablemap_func) printf("not found type_table\n");

    /// tablemap_func
    fseek(tablemap_func, fidx*sizeof(uint32)*3, SEEK_SET);
    uint32 ffidx;
    uint64 tablemap_offset_addr;
    fread(&ffidx, sizeof(uint32), 1, tablemap_func);
    if (fidx != ffidx) {
        perror("tablemap_funcがおかしい\n");
        exit(1);
    }
    fread(&tablemap_offset_addr, sizeof(uint64), 1, tablemap_func);

    /// tablemap_offset
    fseek(tablemap_offset, tablemap_offset_addr, SEEK_SET);
    // 関数fidxのローカルを取得
    uint32 locals_size;
    fread(&locals_size, sizeof(uint32), 1, tablemap_offset);
    uint8 locals[locals_size];
    fread(locals, sizeof(uint8), locals_size, tablemap_offset);
    // 対応するoffsetまで移動
    uint32 ooffset;
    uint64 type_table_addr, pre_type_table_addr;
    while(!feof(tablemap_offset)) {
       fread(&ooffset, sizeof(uint32), 1, tablemap_offset); 
       fread(&type_table_addr, sizeof(uint64), 1, tablemap_offset); 
       if (offset == ooffset) break;
       pre_type_table_addr = type_table_addr;
    }
    if (feof(tablemap_offset)) {
        perror("tablemap_offsetがおかしい\n");
        exit(1);
    }
    // type_table_addr = pre_type_table_addr;

    /// type_table
    fseek(type_table, type_table_addr, SEEK_SET);
    uint32 stack_size;
    fread(&stack_size, sizeof(uint32), 1, type_table);
    uint8 stack[stack_size];
    fread(stack, sizeof(uint8), stack_size, type_table);

    if (is_return_address) {
        fread(&stack_size, sizeof(uint32), 1, type_table);
        fread(stack, sizeof(uint8), stack_size, type_table);
    }

    // uint8 type_stack[locals_size + stack_size];
    uint8* type_stack = malloc(locals_size + stack_size);
    for (uint32 i = 0; i < locals_size; ++i) type_stack[i] = locals[i];
    for (uint32 i = 0; i < stack_size; ++i) type_stack[locals_size + i] = stack[i];

    fclose(tablemap_func);
    fclose(tablemap_offset);
    fclose(type_table);

    *type_stack_size = locals_size + stack_size;
    return type_stack;
}

int check_soft_dirty(int fd, uint8* addr) {
#if BH_PLATFORM_LINUX == 1
    // Linuxの場合の実装（変更なし）
    const int PAGEMAP_LENGTH = 8;
    uint64 pagemap_entry;
    unsigned long pfn = (unsigned long)addr / WASM_PAGE_SIZE;
    off_t offset = sizeof(uint64) * pfn;
    
    if (lseek(fd, offset, SEEK_SET) == -1) {
        ESP_LOGE(TAG, "Error seeking to pagemap entry");
        return -1;
    }

    if (read(fd, &pagemap_entry, PAGEMAP_LENGTH) != PAGEMAP_LENGTH) {
        ESP_LOGE(TAG, "Error reading pagemap entry");
        return -1;
    }

    return is_soft_dirty(pagemap_entry);
#else
    // ESP32環境では常に1を返す（ダーティページチェックをスキップ）
    (void)fd;  // 未使用パラメータの警告を抑制
    (void)addr; // 未使用パラメータの警告を抑制
    return 1;
#endif
}

int dump_dirty_memory(WASMMemoryInstance *memory) {
   struct TimeMetrics sd_metrics;
   start_measurement(&sd_metrics);

   FILE *memory_fp = fopen(CHECKPOINT_PATH"/memory.img", "wb");
   if (!memory_fp) {
       ESP_LOGE(TAG, "Failed to open memory.img");
       return -1;
   }

   // バッファリングを追加
   uint8_t* write_buffer = malloc(BUFFER_SIZE);
   if (write_buffer) {
       setvbuf(memory_fp, (char*)write_buffer, _IOFBF, BUFFER_SIZE);
   }

#if BH_PLATFORM_LINUX == 1
   int fd = get_pagemap((unsigned long)memory->memory_data);
   if (fd < 0) {
       if (write_buffer) free(write_buffer);
       fclose(memory_fp);
       return -1;
   }
#else
   int fd = 0; // ESP32環境ではfdは使用しない
#endif

   uint8* memory_data = memory->memory_data;
   uint8* memory_data_end = memory->memory_data_end;
   size_t total_bytes_written = 0;
   int pages_written = 0;

   // ダンプ用のバッファを確保
   uint8_t* dump_buffer = malloc(DUMP_PAGE_SIZE);
   if (!dump_buffer) {
       ESP_LOGE(TAG, "Failed to allocate dump buffer");
       if (write_buffer) free(write_buffer);
       fclose(memory_fp);
       return -1;
   }

   size_t buffer_offset = 0;

   for (uint8* addr = memory->memory_data; addr < memory_data_end; addr += WASM_PAGE_SIZE) {
       if (check_soft_dirty(fd, addr)) {
           uint32 offset = (uint32)((uintptr_t)addr - (uintptr_t)memory_data);

           // バッファがいっぱいになった場合、書き込みを実行
           if (buffer_offset + WASM_PAGE_SIZE + sizeof(uint32) > DUMP_PAGE_SIZE) {
               size_t written = fwrite(dump_buffer, 1, buffer_offset, memory_fp);
               if (written != buffer_offset) {
                   ESP_LOGE(TAG, "Failed to write dump buffer: %zu/%zu bytes written",
                           written, buffer_offset);
                   goto error;
               }
               total_bytes_written += written;
               buffer_offset = 0;
           }

           // オフセットとデータをバッファにコピー
           memcpy(dump_buffer + buffer_offset, &offset, sizeof(uint32));
           buffer_offset += sizeof(uint32);
           memcpy(dump_buffer + buffer_offset, addr, WASM_PAGE_SIZE);
           buffer_offset += WASM_PAGE_SIZE;

           pages_written++;

           // 進捗状況を定期的に出力
           if (pages_written % 50 == 0) {
               ESP_LOGI(TAG, "Progress: %d pages written", pages_written);
           }
       }
   }

   // 残りのバッファを書き込み
   if (buffer_offset > 0) {
       size_t written = fwrite(dump_buffer, 1, buffer_offset, memory_fp);
       if (written != buffer_offset) {
           ESP_LOGE(TAG, "Failed to write final buffer: %zu/%zu bytes written",
                   written, buffer_offset);
           goto error;
       }
       total_bytes_written += written;
   }

   uint64_t sd_write_time = end_measurement(&sd_metrics);
   float write_speed = (total_bytes_written * 1000000.0f) / (sd_write_time * 1024.0f);

   ESP_LOGI(TAG, "Memory dump statistics:");
   ESP_LOGI(TAG, "- Pages written: %d", pages_written);
   ESP_LOGI(TAG, "- Total bytes written: %zu", total_bytes_written);
   ESP_LOGI(TAG, "- Write time: %llu microseconds", sd_write_time);
   ESP_LOGI(TAG, "- Write speed: %.2f KB/s", write_speed);

   free(dump_buffer);
   if (write_buffer) free(write_buffer);
#if BH_PLATFORM_LINUX == 1
   if (fd > 0) close(fd);
#endif
   fclose(memory_fp);
   return 0;

error:
   if (dump_buffer) free(dump_buffer);
   if (write_buffer) free(write_buffer);
#if BH_PLATFORM_LINUX == 1
   if (fd > 0) close(fd);
#endif
   fclose(memory_fp);
   return -1;
}

int wasm_dump_memory(WASMMemoryInstance *memory) {
    struct TimeMetrics mem_metrics;
    start_measurement(&mem_metrics);

    ESP_LOGI(TAG, "Starting memory dump");

    if (!memory) {
        ESP_LOGE(TAG, "Invalid memory instance");
        return -1;
    }

    FILE *mem_size_fp = fopen(CHECKPOINT_PATH"/memcount.img", "wb");
    if (!mem_size_fp) {
        ESP_LOGE(TAG, "Failed to open memcount.img");
        return -1;
    }

    ESP_LOGI(TAG, "Current page count: %d", memory->cur_page_count);
    if (fwrite(&(memory->cur_page_count), sizeof(uint32), 1, mem_size_fp) != 1) {
        ESP_LOGE(TAG, "Failed to write memory page count");
        fclose(mem_size_fp);
        return -1;
    }
    fclose(mem_size_fp);

    int ret = dump_dirty_memory(memory);
    if (ret < 0) {
        ESP_LOGE(TAG, "Failed to dump dirty memory pages");
        return ret;
    }

    uint64_t mem_dump_time = end_measurement(&mem_metrics);
    ESP_LOGI(TAG, "Memory dump completed in %llu microseconds", mem_dump_time);

    return 0;
}

/* Global variables dump */
int wasm_dump_global(WASMModuleInstance *module, WASMGlobalInstance *globals, uint8* global_data) {
    struct TimeMetrics global_metrics;
    start_measurement(&global_metrics);

    FILE *fp = fopen(CHECKPOINT_PATH"/global.img", "wb");
    if (!fp) {
        ESP_LOGE(TAG, "Failed to open global.img");
        return -1;
    }

    for (int i = 0; i < module->e->global_count; i++) {
        uint8 *global_addr = get_global_addr_for_migration(global_data, (globals+i));
        size_t write_size = 0;

        switch (globals[i].type) {
            case VALUE_TYPE_I32:
            case VALUE_TYPE_F32:
                write_size = fwrite(global_addr, sizeof(uint32), 1, fp);
                if (write_size != 1) goto error;
                break;
            case VALUE_TYPE_I64:
            case VALUE_TYPE_F64:
                write_size = fwrite(global_addr, sizeof(uint64), 1, fp);
                if (write_size != 1) goto error;
                break;
            default:
                ESP_LOGE(TAG, "Unsupported global type");
                goto error;
        }
    }

    uint64_t global_dump_time = end_measurement(&global_metrics);
    ESP_LOGI(TAG, "Global variables dump time: %llu microseconds", global_dump_time);

    fclose(fp);
    return 0;

error:
    ESP_LOGE(TAG, "Failed to write global variables");
    fclose(fp);
    return -1;
}

/* Program counter dump */
int wasm_dump_program_counter(WASMModuleInstance *module, WASMFunctionInstance *func, uint8 *frame_ip) {
    struct TimeMetrics pc_metrics;
    start_measurement(&pc_metrics);

    if (!module || !func || !frame_ip) {
        ESP_LOGE(TAG, "Invalid parameters for program counter dump");
        return -1;
    }

    uint32 fidx = func - module->e->functions;
    uint32 offset = frame_ip - wasm_get_func_code(func);

    ESP_LOGI(TAG, "Dumping program counter: fidx=%d, offset=%d", fidx, offset);

    FILE* fp = fopen(CHECKPOINT_PATH"/PROGRAM.IMG", "wb");
    if (!fp) {
        ESP_LOGE(TAG, "Failed to open program counter file");
        return -1;
    }

    bool success = (fwrite(&fidx, sizeof(uint32), 1, fp) == 1) &&
                  (fwrite(&offset, sizeof(uint32), 1, fp) == 1);

    fclose(fp);

    uint64_t pc_dump_time = end_measurement(&pc_metrics);
    ESP_LOGI(TAG, "Program counter dump time: %llu microseconds", pc_dump_time);

    return success ? 0 : -1;
}

/* Stack dump */
static void _dump_stack(WASMExecEnv *exec_env, struct WASMInterpFrame *frame, FILE *fp, bool is_top) {
    struct TimeMetrics stack_metrics;
    start_measurement(&stack_metrics);
    
    if (!exec_env || !frame || !fp) return;
    if (!exec_env->module_inst) return;
    
    WASMModuleInstance *module = (WASMModuleInstance*)exec_env->module_inst;
    if (!module->e || !module->e->functions) return;

    if (!frame->function) return;
    // if (!frame->sp || !frame->sp_bottom || !frame->lp) return;
    if (!frame->csp || !frame->csp_bottom) return;

    uint32 curr_fidx = frame->function - module->e->functions;

    uint32 ret_fidx = 0;
    uint32 ret_offset = 0;

    ret_fidx = curr_fidx;
    ret_offset = frame->ip - wasm_get_func_code(frame->function);

    if (fwrite(&ret_fidx, sizeof(uint32), 1, fp) != 1) return;
    if (fwrite(&ret_offset, sizeof(uint32), 1, fp) != 1) return;

    uint32 type_stack_size = 0;
    uint8* type_stack = get_type_stack(ret_fidx, ret_offset, &type_stack_size, !is_top);
    
    if (!type_stack || type_stack_size == 0) {
        if (type_stack) free(type_stack);
        return;
    }

    if (fwrite(&type_stack_size, sizeof(uint32), 1, fp) != 1 ||
        fwrite(type_stack, sizeof(uint8), type_stack_size, fp) != type_stack_size) {
        free(type_stack);
        return;
    }
    free(type_stack);

    WASMFunctionInstance* func = frame->function;
    uint32 local_count = func->param_cell_num + func->local_cell_num;
    uint32 stack_size = frame->sp - frame->sp_bottom;

    if (local_count > 0 && fwrite(frame->lp, sizeof(uint32), local_count, fp) != local_count) return;
    if (stack_size > 0 && fwrite(frame->sp_bottom, sizeof(uint32), stack_size, fp) != stack_size) return;

    uint32 block_count = frame->csp - frame->csp_bottom;
    if (block_count == 0) return;
    if (fwrite(&block_count, sizeof(uint32), 1, fp) != 1) return;

    uint8* code_start = wasm_get_func_code(frame->function);
    for (uint32 i = 0; i < block_count; i++) {
        WASMBranchBlock* block = &frame->csp_bottom[i];
        if (!block || !block->begin_addr || !block->target_addr || !block->frame_sp) continue;
        
        uint32 begin_offset = block->begin_addr - code_start;
        uint32 target_offset = block->target_addr - code_start;
        uint32 sp_offset = block->frame_sp - frame->sp_bottom;
        
        if (fwrite(&begin_offset, sizeof(uint32), 1, fp) != 1 ||
            fwrite(&target_offset, sizeof(uint32), 1, fp) != 1 ||
            fwrite(&sp_offset, sizeof(uint32), 1, fp) != 1 ||
            fwrite(&block->cell_num, sizeof(uint32), 1, fp) != 1) return;
    }

    uint64_t stack_dump_time = end_measurement(&stack_metrics);
    ESP_LOGI(TAG, "Stack frame dump time: %llu microseconds", stack_dump_time);
}

int wasm_dump_stack(WASMExecEnv *exec_env, struct WASMInterpFrame *frame) {
    struct TimeMetrics total_metrics;
    start_measurement(&total_metrics);

    if (!exec_env || !frame) return -1;

    WASMModuleInstance *module = (WASMModuleInstance *)exec_env->module_inst;
    if (!module || !module->e || !module->e->functions) return -1;

    static const char* frame_fmt = "%s/stack%d.img";
    static const char* frame_count_file = "%s/frame.img";
    char file_path[128];
    int frame_count = 0;
    FILE *fp = NULL;

    WASMInterpFrame *curr = frame;
    // frameをtopからbottomまで走査する
    char file[32];
    int i = 0;
    do {
        // dummy framenならbreak
        if (frame->function == NULL) break;

        ++i;
        // sprintf(file, "stack%d.img", i);
        // FILE *fp = open_image(file, "wb");
        snprintf(file_path, sizeof(file_path), frame_fmt, CHECKPOINT_PATH, i);
        FILE *fp = fopen(file_path, "wb");
        if (!fp) {
            return -1;
        }

        uint32 entry_fidx = frame->function - module->e->functions;
        fwrite(&entry_fidx, sizeof(uint32), 1, fp);

        _dump_stack(exec_env, frame, fp, (i==1));
        fclose(fp);
    } while((frame = frame->prev_frame));
//     while (curr) {
//         if (curr->function) frame_count++;
//         curr = curr->prev_frame;
//     }
//     if (frame_count == 0) return -1;

//     curr = frame;
//     for (int i = 1; i <= frame_count && curr; i++) {
//         snprintf(file_path, sizeof(file_path), frame_fmt, CHECKPOINT_PATH, i);
//         fp = fopen(file_path, "wb");
//         if (!fp) {
//             return -1;
//         }

//         if (!curr->function || 
//             curr->function < module->e->functions || 
//             curr->function >= module->e->functions + module->e->function_count) {
//             fclose(fp);
//             return -1;
//         }

//         uint32_t entry_fidx = (uint32_t)(curr->function - module->e->functions);
//         if (fwrite(&entry_fidx, sizeof(uint32_t), 1, fp) != 1) {
//            fclose(fp);
//            return -1;
//         }

//        _dump_stack(exec_env, curr, fp, (i == 1));
//        fclose(fp);
//        fp = NULL;

//        curr = curr->prev_frame;
//    }

   snprintf(file_path, sizeof(file_path), frame_count_file, CHECKPOINT_PATH);
   fp = fopen(file_path, "wb");
   if (!fp) return -1;

   if (fwrite(&i, sizeof(uint32_t), 1, fp) != 1) {
       fclose(fp);
       return -1;
   }
   fclose(fp);

   uint64_t total_time = end_measurement(&total_metrics);
   ESP_LOGI(TAG, "Total stack dump time: %llu microseconds", total_time);

   return 0;
}

/* Main dump function */
int wasm_dump(WASMExecEnv *exec_env,
        WASMModuleInstance *module,
        WASMMemoryInstance *memory,
        WASMGlobalInstance *globals,
        uint8 *global_data,
        uint8 *global_addr,
        WASMFunctionInstance *cur_func,
        struct WASMInterpFrame *frame,
        register uint8 *frame_ip,
        register uint32 *frame_sp,
        WASMBranchBlock *frame_csp,
        uint8 *frame_ip_end,
        uint8 *else_addr,
        uint8 *end_addr,
        uint8 *maddr,
        bool done_flag)
{
   struct TimeMetrics total_time;
   start_measurement(&total_time);
   int ret = 0;

   ESP_LOGI(TAG, "Starting checkpoint creation");

//    ret = wasm_dump_memory(memory);
//    if (ret < 0) {
//        ESP_LOGE(TAG, "Failed to dump memory");
//        return ret;
//    }

   ret = wasm_dump_global(module, globals, global_data);
   if (ret < 0) {
       ESP_LOGE(TAG, "Failed to dump globals");
       return ret;
   }

   ret = wasm_dump_program_counter(module, cur_func, frame_ip);
   if (ret < 0) {
       ESP_LOGE(TAG, "Failed to dump program counter");
       return ret;
   }

   ret = wasm_dump_stack(exec_env, frame);
   if (ret < 0) {
       ESP_LOGE(TAG, "Failed to dump stack");
       return ret;
   }

   uint64_t total_time_us = end_measurement(&total_time);
   ESP_LOGI(TAG, "Checkpoint creation completed:");
   ESP_LOGI(TAG, "Total time: %llu microseconds", total_time_us);

   return 0;
}
