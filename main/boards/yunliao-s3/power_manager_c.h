#ifndef YUNLIAO_POWER_MANAGER_C_H
#define YUNLIAO_POWER_MANAGER_C_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct yunliao_pm yunliao_pm_t;
typedef void (*yunliao_pm_cb_t)(bool status, void *user_data);

yunliao_pm_t *yunliao_pm_create(void);
void yunliao_pm_initialize(yunliao_pm_t *pm);
bool yunliao_pm_is_charging(yunliao_pm_t *pm);
bool yunliao_pm_is_discharging(yunliao_pm_t *pm);
int yunliao_pm_get_battery_level(yunliao_pm_t *pm);
void yunliao_pm_on_charging_changed(yunliao_pm_t *pm, yunliao_pm_cb_t cb, void *ud);
void yunliao_pm_on_discharging_changed(yunliao_pm_t *pm, yunliao_pm_cb_t cb, void *ud);
void yunliao_pm_on_bt_link_changed(yunliao_pm_t *pm, yunliao_pm_cb_t cb, void *ud);
void yunliao_pm_check_startup(yunliao_pm_t *pm);
void yunliao_pm_start_5v(yunliao_pm_t *pm);
void yunliao_pm_shutdown_5v(yunliao_pm_t *pm);
void yunliao_pm_start_4g(yunliao_pm_t *pm);
void yunliao_pm_shutdown_4g(yunliao_pm_t *pm);
void yunliao_pm_enable_4g(yunliao_pm_t *pm);
void yunliao_pm_disable_4g(yunliao_pm_t *pm);
void yunliao_pm_sleep(yunliao_pm_t *pm);
void yunliao_pm_init_bt_modul(yunliao_pm_t *pm);

#ifdef __cplusplus
}
#endif

#endif
