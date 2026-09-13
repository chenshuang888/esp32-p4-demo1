/*
 * app_manager —— 多应用调度器（纯框架）实现
 *
 * 内部就 4 个静态状态，逻辑都是线性的，没有锁、没有查重、没有排序。
 * 因为"注册谁、什么顺序"完全由 app_main 控制，框架不需要防御自己的使用者。
 */
#include "app_manager.h"

#include "esp_log.h"

static const char *TAG = "app_manager";

/* 约定：第 0 个注册的 App 是主页（桌面） */
#define APP_MANAGER_HOME_INDEX  0

static const app_desc_t *s_apps[APP_MANAGER_MAX_APPS];  /* App 表（存指针） */
static int s_app_count = 0;                             /* 已注册数量 */
static int s_current   = -1;                            /* 当前 App；-1 = 还没进任何 App */

esp_err_t app_manager_register(const app_desc_t *desc)
{
    if (desc == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_app_count >= APP_MANAGER_MAX_APPS) {
        ESP_LOGE(TAG, "app table full (%d)", APP_MANAGER_MAX_APPS);
        return ESP_ERR_NO_MEM;
    }

    s_apps[s_app_count] = desc;
    ESP_LOGI(TAG, "register [%d] %s", s_app_count, desc->name);
    s_app_count++;
    return ESP_OK;
}

int app_manager_count(void)
{
    return s_app_count;
}

const app_desc_t *app_manager_get(int index)
{
    if (index < 0 || index >= s_app_count) {
        return NULL;
    }
    return s_apps[index];
}

esp_err_t app_manager_launch(int index)
{
    if (index < 0 || index >= s_app_count) {
        return ESP_ERR_INVALID_ARG;
    }
    if (index == s_current) {
        return ESP_OK;      /* 已经在这个 App 里了 */
    }

    int prev = s_current;
    s_current = index;

    ESP_LOGI(TAG, "launch [%d] %s", index, s_apps[index]->name);

    /* 先让新 App 上场：新 screen 就位后再拆旧的，避免出现"没有有效 screen"的空窗 */
    if (s_apps[index]->enter != NULL) {
        s_apps[index]->enter();
    }
    /* 再让旧 App 收尾 */
    if (prev >= 0 && s_apps[prev]->leave != NULL) {
        s_apps[prev]->leave();
    }

    return ESP_OK;
}

esp_err_t app_manager_go_home(void)
{
    return app_manager_launch(APP_MANAGER_HOME_INDEX);
}
