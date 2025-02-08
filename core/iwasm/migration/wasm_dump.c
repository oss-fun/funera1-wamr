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
   ESP_LOGI(TAG, "Getting type stack for fidx=%u, offset=%u", fidx, offset);
  
   char base_path[128];
   char full_path[256];
   struct dirent *entry;
   FILE *tablemap_func = NULL;
   FILE *tablemap_offset = NULL;
   FILE *type_table = NULL;
   uint8* type_stack = NULL;
   uint8* locals = NULL;
   uint8* stack = NULL;

   snprintf(base_path, sizeof(base_path), "%s/%s", MOUNT_POINT, CHECKPOINT_DIR);

   DIR *dir = opendir(base_path);
   if (!dir) {
       ESP_LOGE(TAG, "Cannot open directory: %s", base_path);
       return NULL;
   }

   bool found_func = false;
   bool found_offset = false;
   bool found_type = false;

   while ((entry = readdir(dir)) != NULL) {
       ESP_LOGI(TAG, "Found file: %s", entry->d_name);
       if (entry->d_name[0] == '.' || entry->d_name[0] == '_') continue;
       if (strcasecmp(entry->d_name, "FUNC") == 0) found_func = true;
       if (strcasecmp(entry->d_name, "OFFSET") == 0) found_offset = true;
       if (strcasecmp(entry->d_name, "TYPE") == 0) found_type = true;
   }
   closedir(dir);

   if (!found_func || !found_offset || !found_type) {
       ESP_LOGE(TAG, "Required files not found: FUNC=%d, OFFSET=%d, TYPE=%d", 
               found_func, found_offset, found_type);
       return NULL;
   }

   // Open FUNC file
   snprintf(full_path, sizeof(full_path), "%s/FUNC", base_path);
   tablemap_func = fopen(full_path, "rb");
   if (!tablemap_func) {
       ESP_LOGE(TAG, "Failed to open FUNC file: %s", full_path);
       return NULL;
   }

   // Open OFFSET file
   snprintf(full_path, sizeof(full_path), "%s/OFFSET", base_path);
   tablemap_offset = fopen(full_path, "rb");
   if (!tablemap_offset) {
       ESP_LOGE(TAG, "Failed to open OFFSET file: %s", full_path);
       fclose(tablemap_func);
       return NULL;
   }

   // Open TYPE file
   snprintf(full_path, sizeof(full_path), "%s/TYPE", base_path);
   type_table = fopen(full_path, "rb");
   if (!type_table) {
       ESP_LOGE(TAG, "Failed to open TYPE file: %s", full_path);
       fclose(tablemap_func);
       fclose(tablemap_offset);
       return NULL;
   }

   // Get file sizes
   fseek(tablemap_func, 0, SEEK_END);
   long func_size = ftell(tablemap_func);
   fseek(tablemap_offset, 0, SEEK_END);
   long offset_size = ftell(tablemap_offset);
   fseek(type_table, 0, SEEK_END);
   long type_size = ftell(type_table);

   ESP_LOGI(TAG, "File sizes - FUNC: %ld, OFFSET: %ld, TYPE: %ld", 
            func_size, offset_size, type_size);

   // Reset file positions
   fseek(tablemap_func, 0, SEEK_SET);
   fseek(tablemap_offset, 0, SEEK_SET);
   fseek(type_table, 0, SEEK_SET);

   // Read from FUNC file
   const size_t FUNC_ENTRY_SIZE = sizeof(uint32) * 3;
   long func_seek_pos = fidx * FUNC_ENTRY_SIZE;
   uint32 ffidx;
   uint64 tablemap_offset_addr;

   if (func_seek_pos + FUNC_ENTRY_SIZE > func_size) {
       ESP_LOGE(TAG, "FUNC file seek position out of bounds: %ld > %ld", 
               func_seek_pos + FUNC_ENTRY_SIZE, func_size);
       goto error;
   }

   if (fseek(tablemap_func, func_seek_pos, SEEK_SET) != 0) {
       ESP_LOGE(TAG, "Failed to seek in FUNC file");
       goto error;
   }

   if (fread(&ffidx, sizeof(uint32), 1, tablemap_func) != 1) {
       ESP_LOGE(TAG, "Failed to read function index");
       goto error;
   }

   if (fidx != ffidx) {
       ESP_LOGE(TAG, "Function index mismatch: expected %u, got %u", fidx, ffidx);
       goto error;
   }

   if (fread(&tablemap_offset_addr, sizeof(uint64), 1, tablemap_func) != 1) {
       ESP_LOGE(TAG, "Failed to read offset address");
       goto error;
   }

   ESP_LOGI(TAG, "FUNC read: fidx=%u, offset_addr=0x%llx", ffidx, tablemap_offset_addr);

   // Read from OFFSET file
   const size_t ENTRY_SIZE = sizeof(uint32) + sizeof(uint64);

   if (tablemap_offset_addr >= offset_size) {
       ESP_LOGE(TAG, "Invalid offset address: 0x%llx >= %ld", tablemap_offset_addr, offset_size);
       goto error;
   }

   if (fseek(tablemap_offset, tablemap_offset_addr, SEEK_SET) != 0) {
       ESP_LOGE(TAG, "Failed to seek in OFFSET file");
       goto error;
   }

   // Read locals size and data
   uint32 locals_size;
   if (fread(&locals_size, sizeof(uint32), 1, tablemap_offset) != 1) {
       ESP_LOGE(TAG, "Failed to read locals size");
       goto error;
   }

   ESP_LOGI(TAG, "Locals size: %u", locals_size);

   locals = (uint8*)malloc(locals_size);
   if (!locals || locals_size == 0) {
       ESP_LOGE(TAG, "Failed to allocate locals buffer or invalid size");
       goto error;
   }

   if (fread(locals, 1, locals_size, tablemap_offset) != locals_size) {
       ESP_LOGE(TAG, "Failed to read locals data");
       goto error;
   }

   // Get current position for offset entries
   long entry_start_pos = ftell(tablemap_offset);
   size_t available_entries = (offset_size - entry_start_pos) / ENTRY_SIZE;

   ESP_LOGI(TAG, "Starting offset search at pos %ld, available entries: %zu", 
            entry_start_pos, available_entries);

   // Read offset entries
   uint32 current_offset;
   uint64 type_table_addr = 0;
   bool found = false;

   for (size_t i = 0; i < available_entries; i++) {
       if (fread(&current_offset, sizeof(uint32), 1, tablemap_offset) != 1) {
           ESP_LOGE(TAG, "Failed to read offset at entry %zu", i);
           break;
       }

       if (fread(&type_table_addr, sizeof(uint64), 1, tablemap_offset) != 1) {
           ESP_LOGE(TAG, "Failed to read address at entry %zu", i);
           break;
       }

       if (current_offset == offset) {
           found = true;
           ESP_LOGI(TAG, "Found matching offset at entry %zu: offset=%u, addr=0x%llx", 
                    i, current_offset, type_table_addr);
           break;
       }
   }

   if (!found) {
       ESP_LOGE(TAG, "Target offset %u not found", offset);
       goto error;
   }

   // Read from TYPE file using found address
   if (type_table_addr >= type_size) {
       ESP_LOGE(TAG, "Invalid type table address: 0x%llx >= %ld", type_table_addr, type_size);
       goto error;
   }

   if (fseek(type_table, type_table_addr, SEEK_SET) != 0) {
       ESP_LOGE(TAG, "Failed to seek in TYPE file");
       goto error;
   }

   uint32 stack_size;
   if (fread(&stack_size, sizeof(uint32), 1, type_table) != 1 || stack_size == 0) {
       ESP_LOGE(TAG, "Failed to read valid stack size");
       goto error;
   }

   ESP_LOGI(TAG, "Stack size: %u", stack_size);

   stack = (uint8*)malloc(stack_size);
   if (!stack) {
       ESP_LOGE(TAG, "Failed to allocate stack buffer");
       goto error;
   }

   if (fread(stack, 1, stack_size, type_table) != stack_size) {
       ESP_LOGE(TAG, "Failed to read stack data");
       goto error;
   }

   if (is_return_address) {
       uint32 new_stack_size;
       if (fread(&new_stack_size, sizeof(uint32), 1, type_table) != 1 || new_stack_size == 0) {
           ESP_LOGE(TAG, "Failed to read valid new stack size");
           goto error;
       }

       uint8* temp_stack = (uint8*)malloc(new_stack_size);
       if (!temp_stack) {
           ESP_LOGE(TAG, "Failed to allocate new stack buffer");
           goto error;
       }

       if (fread(temp_stack, 1, new_stack_size, type_table) != new_stack_size) {
           ESP_LOGE(TAG, "Failed to read new stack data");
           free(temp_stack);
           goto error;
       }

       free(stack);
       stack = temp_stack;
       stack_size = new_stack_size;
   }

   // Build final type stack
   type_stack = (uint8*)malloc(locals_size + stack_size);
   if (!type_stack) {
       ESP_LOGE(TAG, "Failed to allocate type stack buffer");
       goto error;
   }

   memcpy(type_stack, locals, locals_size);
   memcpy(type_stack + locals_size, stack, stack_size);
   *type_stack_size = locals_size + stack_size;

   ESP_LOGI(TAG, "Successfully built type stack: locals=%u, stack=%u, total=%u", 
            locals_size, stack_size, *type_stack_size);

   free(locals);
   free(stack);
   fclose(tablemap_func);
   fclose(tablemap_offset);
   fclose(type_table);
   return type_stack;

error:
   if (locals) free(locals);
   if (stack) free(stack);
   if (type_stack) free(type_stack);
   if (tablemap_func) fclose(tablemap_func);
   if (tablemap_offset) fclose(tablemap_offset);
   if (type_table) fclose(type_table);
   return NULL;
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
    
    if (frame->prev_frame && frame->prev_frame->function) {
        ret_fidx = frame->prev_frame->function - module->e->functions;
        ret_offset = frame->prev_frame->ip - wasm_get_func_code(frame->prev_frame->function);
    } else {
        ret_fidx = curr_fidx;
        ret_offset = frame->ip - wasm_get_func_code(frame->function);
    }

    if (fwrite(&ret_fidx, sizeof(uint32), 1, fp) != 1) return;
    if (fwrite(&ret_offset, sizeof(uint32), 1, fp) != 1) return;

    uint32 type_stack_size = 0;
    uint8* type_stack = get_type_stack(curr_fidx, ret_offset, &type_stack_size, !is_top);
    
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
