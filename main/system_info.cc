#include "system_info.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include <esp_app_desc.h>
#include <esp_flash.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_pm.h>
#include <esp_memory_utils.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_private/freertos_debug.h>
#if CONFIG_IDF_TARGET_ESP32P4
#include "esp_wifi_remote.h"
#endif

#define TAG "SystemInfo"

namespace {
bool phase_monitor_active = false;
size_t phase_monitor_lifetime_min_internal = 0;
}  // namespace

size_t SystemInfo::GetFlashSize() {
    uint32_t flash_size;
    if (esp_flash_get_size(NULL, &flash_size) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get flash size");
        return 0;
    }
    return (size_t)flash_size;
}

size_t SystemInfo::GetMinimumFreeHeapSize() { return esp_get_minimum_free_heap_size(); }

size_t SystemInfo::GetFreeHeapSize() { return esp_get_free_heap_size(); }

std::string SystemInfo::GetMacAddress() {
    uint8_t mac[6];
#if CONFIG_IDF_TARGET_ESP32P4
    esp_wifi_get_mac(WIFI_IF_STA, mac);
#else
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
#endif
    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2],
             mac[3], mac[4], mac[5]);
    return std::string(mac_str);
}

std::string SystemInfo::GetChipModelName() { return std::string(CONFIG_IDF_TARGET); }

std::string SystemInfo::GetUserAgent() {
    auto app_desc = esp_app_get_description();
    auto user_agent = std::string("JSR-Algo-TBOT/") + app_desc->version;
    return user_agent;
}

esp_err_t SystemInfo::PrintTaskCpuUsage(TickType_t xTicksToWait) {
#define ARRAY_SIZE_OFFSET 5
    TaskStatus_t *start_array = NULL, *end_array = NULL;
    UBaseType_t start_array_size, end_array_size;
    configRUN_TIME_COUNTER_TYPE start_run_time, end_run_time;
    esp_err_t ret;
    uint32_t total_elapsed_time;

    // Allocate array to store current task states
    start_array_size = uxTaskGetNumberOfTasks() + ARRAY_SIZE_OFFSET;
    start_array = (TaskStatus_t*)malloc(sizeof(TaskStatus_t) * start_array_size);
    if (start_array == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto exit;
    }
    // Get current task states
    start_array_size = uxTaskGetSystemState(start_array, start_array_size, &start_run_time);
    if (start_array_size == 0) {
        ret = ESP_ERR_INVALID_SIZE;
        goto exit;
    }

    vTaskDelay(xTicksToWait);

    // Allocate array to store tasks states post delay
    end_array_size = uxTaskGetNumberOfTasks() + ARRAY_SIZE_OFFSET;
    end_array = (TaskStatus_t*)malloc(sizeof(TaskStatus_t) * end_array_size);
    if (end_array == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto exit;
    }
    // Get post delay task states
    end_array_size = uxTaskGetSystemState(end_array, end_array_size, &end_run_time);
    if (end_array_size == 0) {
        ret = ESP_ERR_INVALID_SIZE;
        goto exit;
    }

    // Calculate total_elapsed_time in units of run time stats clock period.
    total_elapsed_time = (end_run_time - start_run_time);
    if (total_elapsed_time == 0) {
        ret = ESP_ERR_INVALID_STATE;
        goto exit;
    }

    printf("| Task | Run Time | Percentage\n");
    // Match each task in start_array to those in the end_array
    for (int i = 0; i < start_array_size; i++) {
        int k = -1;
        for (int j = 0; j < end_array_size; j++) {
            if (start_array[i].xHandle == end_array[j].xHandle) {
                k = j;
                // Mark that task have been matched by overwriting their handles
                start_array[i].xHandle = NULL;
                end_array[j].xHandle = NULL;
                break;
            }
        }
        // Check if matching task found
        if (k >= 0) {
            uint32_t task_elapsed_time =
                end_array[k].ulRunTimeCounter - start_array[i].ulRunTimeCounter;
            uint32_t percentage_time = (task_elapsed_time * 100UL) /
                                       (total_elapsed_time * CONFIG_FREERTOS_NUMBER_OF_CORES);
            printf("| %-16s | %8lu | %4lu%%\n", start_array[i].pcTaskName, task_elapsed_time,
                   percentage_time);
        }
    }

    // Print unmatched tasks
    for (int i = 0; i < start_array_size; i++) {
        if (start_array[i].xHandle != NULL) {
            printf("| %s | Deleted\n", start_array[i].pcTaskName);
        }
    }
    for (int i = 0; i < end_array_size; i++) {
        if (end_array[i].xHandle != NULL) {
            printf("| %s | Created\n", end_array[i].pcTaskName);
        }
    }
    ret = ESP_OK;

exit:  // Common return path
    free(start_array);
    free(end_array);
    return ret;
}

void SystemInfo::PrintTaskList() {
    // Headroom for tasks created between the count and the snapshot
    UBaseType_t capacity = uxTaskGetNumberOfTasks() + 5;
    TaskStatus_t* tasks = (TaskStatus_t*)malloc(sizeof(TaskStatus_t) * capacity);
    if (tasks == NULL) {
        ESP_LOGE(TAG, "PrintTaskList: out of memory");
        return;
    }
    configRUN_TIME_COUNTER_TYPE total_run_time = 0;
    UBaseType_t count = uxTaskGetSystemState(tasks, capacity, &total_run_time);
    if (count == 0) {
        ESP_LOGE(TAG, "PrintTaskList: snapshot failed");
        free(tasks);
        return;
    }

    // Sort by priority (desc), then name, so the output is stable between prints
    std::sort(tasks, tasks + count, [](const TaskStatus_t& a, const TaskStatus_t& b) {
        if (a.uxCurrentPriority != b.uxCurrentPriority) {
            return a.uxCurrentPriority > b.uxCurrentPriority;
        }
        return strcmp(a.pcTaskName, b.pcTaskName) < 0;
    });

    ESP_LOGI(TAG, "Task list (%u tasks, uptime %lu ms):", (unsigned)count,
             (unsigned long)(esp_timer_get_time() / 1000));
    ESP_LOGI(TAG, "%-3s %-16s %-5s %4s %4s %4s %6s %6s %6s %6s %5s %-5s %12s %6s", "#", "Name",
             "State", "Prio", "Base", "Core", "Stack", "UsedNow", "Peak", "MinFree", "Peak%", "Mem",
             "RunTime(us)", "CPU%");
    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    uint32_t total_stack = 0;
    uint32_t low_stack_count = 0;
    for (UBaseType_t i = 0; i < count; i++) {
        const TaskStatus_t& t = tasks[i];
        const char* state;
        switch (t.eCurrentState) {
            case eRunning:   state = "Run"; break;
            case eReady:     state = "Ready"; break;
            case eBlocked:   state = "Block"; break;
            case eSuspended: state = "Susp"; break;
            case eDeleted:   state = "Del"; break;
            default:         state = "?"; break;
        }
        BaseType_t core = xTaskGetCoreID(t.xHandle);
        char core_str[12];
        if (core == tskNO_AFFINITY) {
            snprintf(core_str, sizeof(core_str), "any");
        } else {
            snprintf(core_str, sizeof(core_str), "%d", (int)core);
        }
        // ESP-IDF StackType_t is uint8_t, so the high water mark is already in bytes
        uint32_t stack_free = t.usStackHighWaterMark;
        const char* stack_mem = esp_ptr_external_ram(t.pxStackBase) ? "PSRAM" : "INT";

        // FreeRTOS has no public "stack size" getter, so read the stack bounds from the TCB.
        // pxEndOfStack is fixed at task creation. A task deleted after the snapshot above
        // could give garbage numbers for that one row (read-only, no crash).
        char size_str[12] = "?";
        char used_str[12] = "?";
        char peak_str[12] = "?";
        char pct_str[12] = "?";
        TaskSnapshot_t snap = {};
        if (vTaskGetSnapshot(t.xHandle, &snap) == pdTRUE && snap.pxEndOfStack != NULL) {
            uintptr_t base = (uintptr_t)t.pxStackBase;
            uintptr_t end = (uintptr_t)snap.pxEndOfStack + sizeof(StackType_t);
            if (end > base) {
                uint32_t size = end - base;
                uint32_t peak = size > stack_free ? size - stack_free : 0;
                total_stack += size;
                snprintf(size_str, sizeof(size_str), "%lu", (unsigned long)size);
                snprintf(peak_str, sizeof(peak_str), "%lu", (unsigned long)peak);
                snprintf(pct_str, sizeof(pct_str), "%lu%%", (unsigned long)(peak * 100 / size));
                // Saved stack pointer is only valid for tasks that are switched out.
                // For this task use the live SP; a task running on the other core is unknown.
                uintptr_t sp = 0;
                if (t.xHandle == self) {
                    sp = (uintptr_t)__builtin_frame_address(0);
                } else if (t.eCurrentState != eRunning) {
                    sp = (uintptr_t)snap.pxTopOfStack;
                }
                if (sp > base && sp <= end) {
                    snprintf(used_str, sizeof(used_str), "%lu", (unsigned long)(end - sp));
                } else {
                    snprintf(used_str, sizeof(used_str), "-");
                }
            }
        }
        // Cumulative CPU share since boot, summed across both cores
        float cpu_percent = total_run_time > 0
                                ? (float)t.ulRunTimeCounter * 100.0f /
                                      ((float)total_run_time * CONFIG_FREERTOS_NUMBER_OF_CORES)
                                : 0.0f;
        const char* warn = "";
        if (stack_free < 512) {
            warn = "  <-- LOW STACK";
            low_stack_count++;
        }
        ESP_LOGI(TAG, "%-3u %-16s %-5s %4u %4u %4s %6s %6s %6s %6lu %5s %-5s %12lu %5.1f%%%s",
                 (unsigned)t.xTaskNumber, t.pcTaskName, state, (unsigned)t.uxCurrentPriority,
                 (unsigned)t.uxBasePriority, core_str, size_str, used_str, peak_str,
                 (unsigned long)stack_free, pct_str, stack_mem, (unsigned long)t.ulRunTimeCounter,
                 cpu_percent, warn);
    }
    ESP_LOGI(TAG, "Total task stack allocated: %lu bytes", (unsigned long)total_stack);
    if (low_stack_count > 0) {
        ESP_LOGW(TAG, "%lu task(s) with < 512 bytes of stack left", (unsigned long)low_stack_count);
    }
    free(tasks);
}

void SystemInfo::PrintHeapStats() {
    int free_sram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    int min_free_sram = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    int largest_free_block = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    int free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "free SRAM: %u, min SRAM: %u largest_free_block: %u, free PSRAM: %u", free_sram,
             min_free_sram, largest_free_block, free_psram);
}

void SystemInfo::StartHeapPhaseMonitor() {
    if (phase_monitor_active) {
        ESP_LOGW(TAG, "heap phase monitor already active");
        return;
    }

    phase_monitor_lifetime_min_internal = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    const esp_err_t err = heap_caps_monitor_local_minimum_free_size_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "heap phase monitor start failed: %s", esp_err_to_name(err));
        return;
    }
    phase_monitor_active = true;
}

void SystemInfo::StopHeapPhaseMonitor() {
    if (!phase_monitor_active)
        return;

    const esp_err_t err = heap_caps_monitor_local_minimum_free_size_stop();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "heap phase monitor stop failed: %s", esp_err_to_name(err));
    }
    phase_monitor_active = false;
}

void SystemInfo::PrintHeapCheckpoint(const char* phase) {
    const size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t observed_min_internal = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    const size_t lifetime_min_internal =
        phase_monitor_active ? phase_monitor_lifetime_min_internal : observed_min_internal;
    const size_t phase_min_internal = phase_monitor_active ? observed_min_internal : internal_free;
    const size_t largest_internal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    const size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    ESP_LOGI(TAG,
             "heap_checkpoint phase=%s internal_free=%u lifetime_min_internal=%u "
             "phase_min_internal=%u largest_internal=%u psram_free=%u",
             phase == nullptr ? "unknown" : phase, (unsigned)internal_free,
             (unsigned)lifetime_min_internal, (unsigned)phase_min_internal,
             (unsigned)largest_internal, (unsigned)psram_free);
}

void SystemInfo::PrintPmLocks() { esp_pm_dump_locks(stdout); }
