/*
 * kv_store —— 实现（后端：NVS）
 *
 * 四个读写函数长得几乎一样，是**刻意重复**的：每个函数从头读到尾就是一次
 * 完整的事务（open -> 操作 -> commit -> close），不用跳到别处看"公共部分
 * 到底做了什么"。抽公共函数要么引入函数指针、要么引入类型标记，比这几行
 * 重复更难看懂 —— 所以不抽。
 */
#include "kv_store.h"

#include "esp_check.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "kv_store";

esp_err_t kv_init(void)
{
    esp_err_t err = nvs_flash_init();

    /* 分区写满 / 分区里的数据格式和当前 IDF 不匹配 —— 这两种情况唯一的恢复
     * 手段是擦掉重来。只在这个分支里擦，正常路径一个字节都不碰。
     * （nvs_flash_init() 本身是幂等的，重复调用返回 ESP_OK。） */
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "nvs unusable (%s), erasing", esp_err_to_name(err));
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "erase failed");
        err = nvs_flash_init();
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "init failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t kv_read_i32(const char *ns, const char *key, int32_t *out)
{
    if (ns == NULL || key == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 用只读模式打开：namespace 不存在时 nvs_open() 直接返回 ESP_ERR_NVS_NOT_FOUND，
     * 和"key 不存在"是同一个码 —— 调用方统一按"没存过"处理，这里不用额外判断。
     * 另外只读打开不会创建 namespace，所以读操作没有任何副作用。 */
    nvs_handle_t h;
    esp_err_t err = nvs_open(ns, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;                 /* h 未建立，不需要 close */
    }

    err = nvs_get_i32(h, key, out);
    nvs_close(h);
    return err;
}

esp_err_t kv_write_i32(const char *ns, const char *key, int32_t value)
{
    if (ns == NULL || key == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(ns, NVS_READWRITE, &h);    /* 不存在会自动创建 */
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_i32(h, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(h);        /* 立即落盘 —— 本组件的契约，见头文件 */
    }
    nvs_close(h);
    return err;
}

esp_err_t kv_read_str(const char *ns, const char *key, char *out, size_t size)
{
    if (ns == NULL || key == NULL || out == NULL || size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 先把输出置空：这样无论后面从哪个分支返回，out 都是一条合法的 C 字符串，
     * 而不是调用方栈上的残留数据。（这条约定写在头文件里。） */
    out[0] = '\0';

    nvs_handle_t h;
    esp_err_t err = nvs_open(ns, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }

    /* nvs_get_str() 的 size 是 in/out 参数：传入缓冲大小，回来时被改成实际
     * 长度（含结尾符）。存进去的串比 size 长则返回 ESP_ERR_NVS_INVALID_LENGTH。 */
    size_t len = size;
    err = nvs_get_str(h, key, out, &len);
    nvs_close(h);

    if (err != ESP_OK) {
        out[0] = '\0';              /* 可能已被写入半截，抹掉 */
    }
    return err;
}

esp_err_t kv_write_str(const char *ns, const char *key, const char *value)
{
    if (ns == NULL || key == NULL || value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(ns, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    /* nvs_set_str() 内部自己算 strlen + 1，结尾符由它负责 —— 所以调用方
     * 不需要像"统一走 blob"的方案那样记得把 '\0' 算进长度。 */
    err = nvs_set_str(h, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}
