#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
// #include "freertos/portmacro.h"
#include "esp_log.h"
//#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "freertos/semphr.h"
#include "esp_system.h"

#include "../interpreter/wasm_runtime.h"
#include "wasm_migration.h"
#include "wasm_restore.h"
#include "wasm_dispatch.h"
#include "../interpreter/wasm_interp.h"

#define MOUNT_POINT "/sdcard"
#define CHECKPOINT_DIR "/SDCARD/CHECKP~1"
#define CHECKPOINT_PATH MOUNT_POINT CHECKPOINT_DIR
#define BH_PLATFORM_LINUX 0
#define MEMORY_BUFFER_SIZE (32 * 1024)  // 32KB
#define WASM_MAX_PAGE_COUNT 32768
#define MAX_RETRY_COUNT 3

static const char *TAG = "wasm-restore";
static volatile bool pc_restored = false;

// メモリ整合性チェックマクロ
#define CHECK_MEMORY_INTEGRITY() \
    if (!heap_caps_check_integrity_all(true)) { \
        ESP_LOGE(TAG, "Memory corruption detected at %s:%d", __FILE__, __LINE__); \
        return -1; \
    }


// メモリインスタンス情報出力関数
static void print_memory_status(WASMMemoryInstance *memory) {
    if (!memory) {
        ESP_LOGE(TAG, "Invalid memory instance");
        return;
    }
    ESP_LOGI(TAG, "Memory Status:");
    ESP_LOGI(TAG, "  Current pages: %d", memory->cur_page_count);
    ESP_LOGI(TAG, "  Bytes per page: %d", memory->num_bytes_per_page);
    ESP_LOGI(TAG, "  Total size: %d bytes", memory->cur_page_count * memory->num_bytes_per_page);
    ESP_LOGI(TAG, "  Memory data start: %p", memory->memory_data);
    ESP_LOGI(TAG, "  Memory data end: %p", memory->memory_data_end);
}

// ファイル操作の安全なオープン
static FILE* safe_fopen(const char* path, const char* mode) {
    ESP_LOGI(TAG, "Opening file: %s", path);
    
    // ベーシックな検証
    if (!path || !mode) {
        ESP_LOGE(TAG, "Invalid parameters for safe_fopen");
        return NULL;
    }

    // ファイルの存在確認
    if (access(path, F_OK) != 0) {
        ESP_LOGE(TAG, "File does not exist: %s", path);
        return NULL;
    }

    // ファイルを開く（クリティカルセクションは避ける）
    FILE* fp = fopen(path, mode);
    if (fp) {
        setvbuf(fp, NULL, _IONBF, 0);
        ESP_LOGI(TAG, "Successfully opened file: %s", path);
    } else {
        ESP_LOGE(TAG, "Failed to open file: %s (errno: %d: %s)", 
                 path, errno, strerror(errno));
    }

    return fp;
}

// スタックフレームの復元

// スタックフレームの復元処理
void _restore_stack(WASMExecEnv *exec_env, struct WASMInterpFrame *frame, FILE *fp) {
    ESP_LOGI(TAG, "Starting stack frame restoration");
    print_memory_info();

    // 基本的な入力検証
    if (!exec_env || !frame || !fp) {
        ESP_LOGE(TAG, "Invalid parameters in stack restoration");
        return;
    }

    WASMModuleInstance *module_inst = (WASMModuleInstance *)exec_env->module_inst;
    if (!module_inst || !frame->function) {
        ESP_LOGE(TAG, "Invalid module instance or frame function");
        return;
    }

    WASMFunctionInstance *function = frame->function;
    uint32 fidx = frame->function - module_inst->e->functions;
    ESP_LOGI(TAG, "Processing function index: %d", fidx);

    // リターンアドレスの読み取り
    uint32 ret_fidx = 0, ret_offset = 0;
    if (fread(&ret_fidx, sizeof(uint32), 1, fp) != 1 ||
        fread(&ret_offset, sizeof(uint32), 1, fp) != 1) {
        ESP_LOGE(TAG, "Failed to read return address information");
        return;
    }

    // スタックの型情報サイズの読み取り
    uint32 type_stack_size = 0;
    if (fread(&type_stack_size, sizeof(uint32), 1, fp) != 1) {
        ESP_LOGE(TAG, "Failed to read type stack size");
        return;
    }

    // スタックフレームの設定
    frame->sp_bottom = frame->lp + function->param_cell_num + function->local_cell_num;
    frame->sp_boundary = frame->sp_bottom + function->u.func->max_stack_cell_num;
    frame->csp_bottom = (WASMBranchBlock *)frame->sp_boundary;
    frame->csp_boundary = frame->csp_bottom + function->u.func->max_block_num;

    // ローカル変数とスタック値の読み込み
    uint32 local_cell_num = function->param_cell_num + function->local_cell_num;
    if (fread(frame->lp, sizeof(uint32), local_cell_num, fp) != local_cell_num) {
        ESP_LOGE(TAG, "Failed to read local variables");
        return;
    }

    uint32 stack_size = frame->sp - frame->sp_bottom;
    if (fread(frame->sp_bottom, sizeof(uint32), stack_size, fp) != stack_size) {
        ESP_LOGE(TAG, "Failed to read stack values");
        return;
    }

    // コントロールスタックの復元
    uint32 ctrl_stack_size = 0;
    if (fread(&ctrl_stack_size, sizeof(uint32), 1, fp) != 1) {
        ESP_LOGE(TAG, "Failed to read control stack size");
        return;
    }

    frame->csp = frame->csp_bottom + ctrl_stack_size;

    // ブランチブロックの復元
    uint8* func_code = wasm_get_func_code(frame->function);
    for (uint32 i = 0; i < ctrl_stack_size; i++) {
        WASMBranchBlock* block = &frame->csp_bottom[i];
        uint32 offset = 0;

        if (fread(&offset, sizeof(uint32), 1, fp) != 1) {
            ESP_LOGE(TAG, "Failed to read block begin address");
            return;
        }
        block->begin_addr = func_code + offset;

        if (fread(&offset, sizeof(uint32), 1, fp) != 1) {
            ESP_LOGE(TAG, "Failed to read block target address");
            return;
        }
        block->target_addr = func_code + offset;

        if (fread(&offset, sizeof(uint32), 1, fp) != 1) {
            ESP_LOGE(TAG, "Failed to read block frame sp");
            return;
        }
        block->frame_sp = frame->sp_bottom + offset;

        if (fread(&block->cell_num, sizeof(uint32), 1, fp) != 1) {
            ESP_LOGE(TAG, "Failed to read block cell number");
            return;
        }
    }

    ESP_LOGI(TAG, "Stack frame restoration completed");
}
// メインのスタック復元関数
struct WASMInterpFrame* wasm_restore_stack(WASMExecEnv **_exec_env) {
    ESP_LOGI(TAG, "Entering wasm_restore_stack");
    print_memory_info();

    if (!_exec_env || !*_exec_env) {
        ESP_LOGE(TAG, "Invalid exec_env");
        return NULL;
    }

    WASMExecEnv *exec_env = *_exec_env;
    WASMModuleInstance *module_inst = (WASMModuleInstance *)exec_env->module_inst;
    struct WASMInterpFrame *frame = wasm_exec_env_get_cur_frame(exec_env);
    struct WASMInterpFrame *prev_frame = frame;

    // フレームカウントファイルの読み込み
    char frame_file[256];
    uint32 frame_stack_size = 0;
    snprintf(frame_file, sizeof(frame_file), CHECKPOINT_PATH"/FRAME.IMG");

    vTaskDelay(pdMS_TO_TICKS(10));  // ファイルシステムの安定化を待つ

    FILE* fp = fopen(frame_file, "rb");
    if (!fp) {
        ESP_LOGE(TAG, "Failed to open frame count file");
        return NULL;
    }

    if (fread(&frame_stack_size, sizeof(uint32), 1, fp) != 1) {
        ESP_LOGE(TAG, "Failed to read frame count");
        fclose(fp);
        return NULL;
    }
    fclose(fp);
    ESP_LOGI(TAG, "Frame stack size: %d", frame_stack_size);

    // 各フレームの復元
    for (uint32 i = frame_stack_size; i > 0; i--) {
        char stack_file[256];
        snprintf(stack_file, sizeof(stack_file), CHECKPOINT_PATH"/STACK%d.IMG", i);
        
        vTaskDelay(pdMS_TO_TICKS(10));  // ファイルシステムの安定化を待つ

        fp = fopen(stack_file, "rb");
        if (!fp) {
            ESP_LOGE(TAG, "Failed to open stack file: %s", stack_file);
            return NULL;
        }

        uint32 fidx;
        if (fread(&fidx, sizeof(uint32), 1, fp) != 1) {
            ESP_LOGE(TAG, "Failed to read function index");
            fclose(fp);
            return NULL;
        }

        WASMFunctionInstance *function = module_inst->e->functions + fidx;
        if (!function) {
            ESP_LOGE(TAG, "Invalid function pointer");
            fclose(fp);
            return NULL;
        }

        uint32 all_cell_num = function->param_cell_num + 
                             function->local_cell_num + 
                             function->u.func->max_stack_cell_num + 
                             (function->u.func->max_block_num * sizeof(WASMBranchBlock) / 4);

        uint32 frame_size = (uint32)wasm_interp_interp_frame_size(all_cell_num);
        frame = wasm_exec_env_alloc_wasm_frame(exec_env, frame_size);
        if (!frame) {
            ESP_LOGE(TAG, "Failed to allocate frame");
            fclose(fp);
            return NULL;
        }

        frame->function = function;
        frame->prev_frame = prev_frame;
        
        _restore_stack(exec_env, frame, fp);
        
        prev_frame = frame;
        fclose(fp);
        
        ESP_LOGI(TAG, "Frame %d processed", i);
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    wasm_exec_env_set_cur_frame(exec_env, frame);
    ESP_LOGI(TAG, "Stack restoration completed successfully");
    return frame;
}

// メモリの復元
int wasm_restore_memory(WASMModuleInstance *module, WASMMemoryInstance **memory, uint8** maddr) {
    ESP_LOGI(TAG, "Starting memory restoration");

    if (!module || !memory || !*memory) {
        ESP_LOGE(TAG, "Invalid parameters for memory restoration");
        return -1;
    }

    print_memory_status(*memory);

    // メモリカウントファイルの読み取り
    FILE* fp = safe_fopen(CHECKPOINT_PATH"/MEMCOUNT.IMG", "rb");
    if (!fp) {
        return -1;
    }

    uint32 page_count = 0;
    if (fread(&page_count, sizeof(uint32), 1, fp) != 1) {
        ESP_LOGE(TAG, "Failed to read page count");
        fclose(fp);
        return -1;
    }
    fclose(fp);

    ESP_LOGI(TAG, "Read page count: %d", page_count);

    // メモリの総サイズを計算
    uint32_t total_memory_size = (*memory)->cur_page_count * (*memory)->num_bytes_per_page;
    ESP_LOGI(TAG, "Total memory size: %u bytes", total_memory_size);

    // メモリデータファイルを開く
    fp = safe_fopen(CHECKPOINT_PATH"/MEMORY.IMG", "rb");
    if (!fp) {
        ESP_LOGE(TAG, "Failed to open memory data file");
        return -1;
    }

    // メモリバッファの割り当て
    const size_t BUFFER_SIZE = 4096;
    uint8_t* buffer = heap_caps_malloc(BUFFER_SIZE, MALLOC_CAP_SPIRAM);
    if (!buffer) {
        ESP_LOGE(TAG, "Failed to allocate memory buffer");
        fclose(fp);
        return -1;
    }

    bool success = true;
    size_t total_read = 0;
    uint32_t last_valid_offset = 0;

    while (!feof(fp) && success) {
        uint32_t offset;
        size_t read_size = fread(&offset, sizeof(uint32), 1, fp);
        
        if (read_size != 1) {
            if (feof(fp)) {
                break;
            }
            ESP_LOGE(TAG, "Failed to read offset");
            success = false;
            break;
        }

        // オフセットの妥当性チェック
        if (offset >= total_memory_size || offset % WASM_PAGE_SIZE != 0) {
            ESP_LOGE(TAG, "Invalid offset detected: %u (total size: %u)", 
                     offset, total_memory_size);
            success = false;
            break;
        }

        // データの読み取りとコピー
        read_size = fread(buffer, 1, WASM_PAGE_SIZE, fp);
        if (read_size != WASM_PAGE_SIZE) {
            ESP_LOGE(TAG, "Failed to read memory data at offset %u", offset);
            success = false;
            break;
        }

        // メモリへの書き込み
        if (offset + WASM_PAGE_SIZE <= total_memory_size) {
            memcpy((*memory)->memory_data + offset, buffer, WASM_PAGE_SIZE);
        } else {
            ESP_LOGE(TAG, "Memory copy would exceed bounds at offset %u", offset);
            success = false;
            break;
        }

        total_read += WASM_PAGE_SIZE;
        last_valid_offset = offset;

        if (total_read % (64 * 1024) == 0) {
            ESP_LOGI(TAG, "Memory restore progress: %d%%", 
                     (int)(total_read * 100 / total_memory_size));
            //esp_task_wdt_reset();
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }

    heap_caps_free(buffer);
    fclose(fp);

    if (!success) {
        ESP_LOGE(TAG, "Memory restoration failed");
        return -1;
    }

    *maddr = (uint8*)(page_count * (*memory)->num_bytes_per_page);
    ESP_LOGI(TAG, "Memory restoration completed successfully");
    ESP_LOGI(TAG, "Total bytes read: %u", total_read);
    ESP_LOGI(TAG, "Last valid offset: %u", last_valid_offset);

    return 0;
}

// グローバル変数の復元
int wasm_restore_global(const WASMModuleInstance *module, 
                       const WASMGlobalInstance *globals, 
                       uint8 **global_data, 
                       uint8 **global_addr) {
    ESP_LOGI(TAG, "Starting global restoration");

    if (!module || !globals || !global_data || !*global_data) {
        ESP_LOGE(TAG, "Invalid parameters for global restoration");
        return -1;
    }

    // バッファの割り当て
    uint8_t* buffer = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buffer) {
        ESP_LOGE(TAG, "Failed to allocate buffer");
        return -1;
    }

    bool success = false;
    FILE* fp = NULL;

    do {
        // グローバルデータファイルを開く
        vTaskDelay(pdMS_TO_TICKS(10));
        fp = safe_fopen(CHECKPOINT_PATH"/GLOBAL.IMG", "rb");
        if (!fp) break;

        // ファイルサイズを取得
        fseek(fp, 0, SEEK_END);
        size_t file_size = ftell(fp);
        fseek(fp, 0, SEEK_SET);

        // データ読み取り
        size_t bytes_read = fread(buffer, 1, file_size, fp);
        if (bytes_read != file_size) {
            ESP_LOGE(TAG, "Failed to read global data");
            break;
        }

        // データ処理
        uint8_t* data_ptr = buffer;
        for (int i = 0; i < module->e->global_count; i++) {
            uint8_t* addr = get_global_addr_for_migration(*global_data, globals + i);
            if (!addr) {
                ESP_LOGE(TAG, "Invalid global address for index %d", i);
                break;
            }

            size_t type_size = 0;
            switch (globals[i].type) {
                case VALUE_TYPE_I32:
                case VALUE_TYPE_F32:
                    type_size = sizeof(uint32);
                    break;
                case VALUE_TYPE_I64:
                case VALUE_TYPE_F64:
                    type_size = sizeof(uint64);
                    break;
                default:
                    ESP_LOGE(TAG, "Invalid global type: %d", globals[i].type);
                    continue;
            }

            if (data_ptr + type_size > buffer + file_size) {
                ESP_LOGE(TAG, "Buffer overflow detected");
                break;
            }

            memcpy(addr, data_ptr, type_size);
            data_ptr += type_size;
            CHECK_MEMORY_INTEGRITY();
        }

        success = true;

    } while(0);

    if (fp) {
        fclose(fp);
    }
    if (buffer) {
        heap_caps_free(buffer);
    }

    ESP_LOGI(TAG, "Global restoration %s", success ? "succeeded" : "failed");
    return success ? 0 : -1;
}

// プログラムカウンタの復元

int wasm_restore_program_counter(WASMModuleInstance *module, uint8 **frame_ip) {
    ESP_LOGI(TAG, "Starting program counter restoration");

    if (!module || !frame_ip) {
        ESP_LOGE(TAG, "Invalid parameters for program counter restoration");
        return -1;
    }

    FILE* fp = safe_fopen(CHECKPOINT_PATH"/PROGRAM.IMG", "rb");
    if (!fp) {
        ESP_LOGE(TAG, "Failed to open program counter file");
        return -1;
    }

    uint32 fidx = 0;
    uint32 offset = 0;

    if (fread(&fidx, sizeof(uint32), 1, fp) != 1 ||
        fread(&offset, sizeof(uint32), 1, fp) != 1) {
        ESP_LOGE(TAG, "Failed to read program counter data");
        fclose(fp);
        return -1;
    }

    fclose(fp);
    ESP_LOGI(TAG, "Read program counter: fidx=%d, offset=%d", fidx, offset);

    // 関数インデックスの検証
    if (fidx >= module->e->function_count) {
        ESP_LOGE(TAG, "Invalid function index: %d", fidx);
        return -1;
    }

    WASMFunctionInstance* func = module->e->functions + fidx;
    if (!func || !func->u.func) {
        ESP_LOGE(TAG, "Invalid function instance");
        return -1;
    }

    uint8* code = wasm_get_func_code(func);
    uint8* code_end = wasm_get_func_code_end(func);

    if (!code || !code_end || code >= code_end) {
        ESP_LOGE(TAG, "Invalid code pointers");
        return -1;
    }

    size_t code_size = code_end - code;
    ESP_LOGI(TAG, "Function code range: %p to %p (size: %d)",
             code, code_end, (int)code_size);

    // オフセットの検証
    if (offset >= code_size) {
        ESP_LOGE(TAG, "Invalid offset: %u (code size: %u)", offset, code_size);
        return -1;
    }

    // IPの計算とバウンダリチェック
    uint8* target_ip = code + offset;
    if (target_ip < code || target_ip >= code_end) {
        ESP_LOGE(TAG, "Target IP outside valid range: %p (valid range: %p - %p)",
                 target_ip, code, code_end);
        return -1;
    }

    // メモリ検証は簡略化
    if (!code || !target_ip || target_ip < code || target_ip >= code_end) {
        ESP_LOGE(TAG, "Invalid target IP");
        return -1;
    }

    *frame_ip = target_ip;
    ESP_LOGI(TAG, "Program counter restored to %p (offset: %d)", target_ip, offset);
    return 0;
}


// メインの復元関数

// メイン復元関数
int wasm_restore(WASMModuleInstance **p_module,
                 WASMExecEnv **exec_env,
                 WASMFunctionInstance **cur_func,
                 struct WASMInterpFrame **prev_frame,
                 WASMMemoryInstance **memory,
                 WASMGlobalInstance **globals,
                 uint8 **global_data,
                 uint8 **global_addr,
                 struct WASMInterpFrame **frame,
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
    ESP_LOGI(TAG, "Starting WASM restoration");
    WASMModuleInstance *module = *p_module;

    if (!pc_restored) {
        // メモリの復元
        ESP_LOGI(TAG, "Restoring memory state...");
        if (wasm_restore_memory(module, memory, maddr) != 0) {
            ESP_LOGE(TAG, "Memory restoration failed");
            return -1;
        }

        // グローバル変数の復元
        ESP_LOGI(TAG, "Restoring global state...");
        if (wasm_restore_global(module, *globals, global_data, global_addr) != 0) {
            ESP_LOGE(TAG, "Global state restoration failed");
            return -1;
        }

        // プログラムカウンタの復元
        ESP_LOGI(TAG, "Restoring program counter...");
        if (wasm_restore_program_counter(module, frame_ip) != 0) {
            ESP_LOGE(TAG, "Program counter restoration failed");
            return -1;
        }
        pc_restored = true;
    }

    ESP_LOGI(TAG, "WASM restoration completed successfully");
    return 0;
}
