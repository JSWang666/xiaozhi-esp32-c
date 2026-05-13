#ifndef _SYSTEM_INFO_H_
#define _SYSTEM_INFO_H_

#include <stddef.h>
#include <esp_err.h>
#include <freertos/FreeRTOS.h>

#ifdef __cplusplus
extern "C" {
#endif

size_t system_info_get_flash_size(void);
size_t system_info_get_minimum_free_heap_size(void);
size_t system_info_get_free_heap_size(void);

/**
 * Write MAC address string to buf (at least 18 bytes).
 * Returns buf on success, NULL on failure.
 */
const char *system_info_get_mac_address(char *buf, size_t buf_size);

/** Returns a static string, do not free. */
const char *system_info_get_chip_model_name(void);

/**
 * Write user agent string to buf (e.g. "board-name/1.0.0").
 * Returns buf on success, NULL on failure.
 */
const char *system_info_get_user_agent(char *buf, size_t buf_size);

esp_err_t system_info_print_task_cpu_usage(TickType_t ticks_to_wait);
void system_info_print_task_list(void);
void system_info_print_heap_stats(void);
void system_info_print_pm_locks(void);

#ifdef __cplusplus
}
#endif

/* C++ compatibility wrapper */
#ifdef __cplusplus
#include <string>

class SystemInfo {
public:
    static size_t GetFlashSize() { return system_info_get_flash_size(); }
    static size_t GetMinimumFreeHeapSize() { return system_info_get_minimum_free_heap_size(); }
    static size_t GetFreeHeapSize() { return system_info_get_free_heap_size(); }
    static std::string GetMacAddress() {
        char buf[18];
        system_info_get_mac_address(buf, sizeof(buf));
        return std::string(buf);
    }
    static std::string GetChipModelName() {
        return std::string(system_info_get_chip_model_name());
    }
    static std::string GetUserAgent() {
        char buf[128];
        system_info_get_user_agent(buf, sizeof(buf));
        return std::string(buf);
    }
    static esp_err_t PrintTaskCpuUsage(TickType_t xTicksToWait) {
        return system_info_print_task_cpu_usage(xTicksToWait);
    }
    static void PrintTaskList() { system_info_print_task_list(); }
    static void PrintHeapStats() { system_info_print_heap_stats(); }
    static void PrintPmLocks() { system_info_print_pm_locks(); }
};
#endif

#endif /* _SYSTEM_INFO_H_ */
