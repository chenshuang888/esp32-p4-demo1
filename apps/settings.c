/*
 * 设置 App
 *
 * 三个页面，全部住在**同一个 screen** 里，用三个容器 + HIDDEN 切换：
 *
 *      P1 设置列表 ──点 WiFi──▶ P2 WiFi 列表 ──点某个 SSID──▶ P3 密码输入
 *         ▲                          │                            │
 *         └────── "< Settings" ──────┘        "< WiFi" ───────────┘
 *
 * ⚠️ 为什么用 HIDDEN 切换，而不是切页面时重建：**点击回调里删对象是未定义行为**
 *    （回调可能正跑在被删对象的事件上 —— 项目在 desktop / photo 上已经踩过这个坑，
 *    见 README 的"坑"表）。三个页面在 enter 时一次性建好，之后只改可见性，
 *    整个 App 的点击回调里一个对象都不删。
 *
 * ⚠️ 页面内的"上一级"按钮和状态栏那个 "Back" **不是一回事**：
 *    状态栏的 Back 是 main 注入的、永远回桌面；这里的按钮只在 App 内部退一层。
 *    措辞上刻意写成 "< Settings" / "< WiFi"，就是为了不和状态栏那个混淆。
 *
 * ⚠️ 所以界面文案必须**全用 ASCII**：项目没有中文字体，中文会渲染成占位方块。
 *    唯一可能出方块的地方是 SSID 本身（外部数据），那部分不处理，见 wifi_service.h。
 */
#include "settings.h"

#include <stdint.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

#include "app_manager.h"
#include "kv_store.h"
#include "lcd_screen_display.h"
#include "ui_status_bar.h"
#include "wifi_service.h"

static const char *TAG = "settings";

/* ===================== 页面 ===================== */

typedef enum {
    PAGE_ROOT = 0,      /* 设置列表 */
    PAGE_WIFI,          /* WiFi 列表（扫描结果） */
    PAGE_PASS,          /* 密码输入 */
    PAGE_COUNT
} page_id_t;

/* P2 顶部那条（"< Settings" 按钮 + 状态文字）占的高度，列表从它下面开始 */
#define WIFI_PAGE_TOP_H  56

/* 页面内容区高度 = 屏高 - 状态栏 - 上下 padding(16*2) */
#define PAGE_CONTENT_H   (LCD_SCREEN_V_RES - UI_STATUS_BAR_HEIGHT - 32)

/* 连接结果轮询：每 500ms 看一眼，最多等 15 秒 */
#define CONN_POLL_MS     500
#define CONN_TIMEOUT_MS  15000

/* ===================== 模块状态 ===================== */

static lv_obj_t *s_scr = NULL;
static lv_obj_t *s_pages[PAGE_COUNT] = { NULL, NULL, NULL };

/* P2 */
static lv_obj_t *s_ap_list   = NULL;        /* lv_list：扫描结果填在这里 */
static lv_obj_t *s_ap_status = NULL;        /* 列表上方那行状态文字 */
static bool      s_scanning  = false;       /* 扫描重入保护（扫描是阻塞的） */
static wifi_service_ap_t s_aps[WIFI_SERVICE_SCAN_MAX];
static size_t    s_ap_count  = 0;

/* P3 */
static lv_obj_t *s_pass_ta   = NULL;
static lv_obj_t *s_pass_hint = NULL;

/* 选中的目标。**复制一份字符串**而不是只记下标 —— 这样在密码页停留期间
 * 就算列表被重新扫描覆盖了，手里的目标还是稳的。 */
static char s_pending_ssid[33];

/* 连接结果轮询 */
static lv_timer_t *s_conn_timer   = NULL;
static int         s_conn_waited  = 0;

/* ===================== 页面切换 ===================== */

/*
 * 切换到某一页：只改可见性，不删任何对象（见文件顶部）。
 * 顺便把状态栏标题也刷成这一页的。
 *
 * ⚠️ show_back 一直是 true —— 状态栏那个 Back 是"回桌面"，由 main 注入，
 *    和页面内的"上一级"按钮各管一段，不冲突。
 */
static void show_page(page_id_t p)
{
    static const char *const titles[PAGE_COUNT] = { "Settings", "WiFi", "Password" };

    if (p >= PAGE_COUNT) {
        return;
    }

    for (int i = 0; i < PAGE_COUNT; i++) {
        if (i == (int)p) {
            lv_obj_remove_flag(s_pages[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_pages[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    const ui_status_bar_cfg_t bar = {
        .title     = titles[p],
        .show_back = true,
    };
    ui_status_bar_apply(&bar);
}

/* 建一个页面容器。页面本身不要任何外观，也不吃点击 —— 它只是容器。 */
static void page_build(page_id_t id)
{
    lv_obj_t *p = lv_obj_create(s_scr);
    lv_obj_remove_style_all(p);
    lv_obj_set_size(p, LCD_SCREEN_H_RES, LCD_SCREEN_V_RES - UI_STATUS_BAR_HEIGHT);
    lv_obj_align(p, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_pad_all(p, 16, 0);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_CLICKABLE);

    s_pages[id] = p;
}

/* ===================== 把断开原因翻成人话 ===================== */

/*
 * 只列常见的几种，其余给个通用说法 —— 总比界面只说"失败"有用。
 * 完整清单见 esp_wifi_types_generic.h 里的 wifi_err_reason_t。
 */
static const char *reason_text(uint8_t reason)
{
    switch (reason) {
    case 0:   return "timeout (no response)";   /* 本次尝试压根没断开过 */
    case 15:  return "wrong password";          /* 4WAY_HANDSHAKE_TIMEOUT */
    case 201: return "AP not found";            /* NO_AP_FOUND */
    case 202: return "authentication failed";   /* AUTH_FAIL */
    case 203: return "association failed";      /* ASSOC_FAIL */
    case 204: return "handshake timeout";       /* HANDSHAKE_TIMEOUT */
    case 205: return "connection failed";       /* CONNECTION_FAIL */
    default:  return "failed (see log)";
    }
}

/* ===================== 连接结果的轮询 ===================== */

static void stop_conn_poll(void)
{
    if (s_conn_timer != NULL) {
        lv_timer_delete(s_conn_timer);
        s_conn_timer = NULL;
    }
}

/*
 * 跑在 LVGL 任务里（lv_timer 驱动）。
 * connect 是非阻塞的，所以"成功了没有"只能这样等出来。
 */
static void on_conn_poll(lv_timer_t *timer)
{
    (void)timer;

    if (wifi_service_is_connected()) {
        char ip[16] = { 0 };
        wifi_service_get_ip(ip, sizeof(ip));
        lv_label_set_text_fmt(s_ap_status, "Connected: %s", ip);
        stop_conn_poll();
        return;
    }

    s_conn_waited += CONN_POLL_MS;
    if (s_conn_waited >= CONN_TIMEOUT_MS) {
        lv_label_set_text_fmt(s_ap_status, "Connect failed: %s",
                              reason_text(wifi_service_last_disconnect_reason()));
        stop_conn_poll();
    }
}

/* ===================== 连 接 ===================== */

/*
 * 存 NVS + 喂组件 + 发起连接。
 *
 * ⚠️ "存下来"和"用起来"是两件事，必须成对做：wifi_service 不认识 NVS，
 *    kv_store 不认识 WiFi，把它们凑在一起是**调用方**的责任（这里一份，main 一份）。
 *    键名都用 wifi_service.h 里的 WIFI_CFG_*，不会各写各的。
 */
static void connect_now(const char *password)
{
    const esp_err_t e_ssid = kv_write_str(WIFI_CFG_NS, WIFI_CFG_KEY_SSID, s_pending_ssid);
    const esp_err_t e_pass = kv_write_str(WIFI_CFG_NS, WIFI_CFG_KEY_PASS, password);
    if (e_ssid != ESP_OK || e_pass != ESP_OK) {
        /* 存不下不该拦着连接：这一次还是能连上，只是下次开机不会自动连。
         * 最常见的失败原因是 partition 写满。 */
        ESP_LOGW(TAG, "凭据存 NVS 失败（ssid=%s pass=%s），本次连接继续",
                 esp_err_to_name(e_ssid), esp_err_to_name(e_pass));
    }

    if (wifi_service_set_credentials(s_pending_ssid, password) != ESP_OK ||
        wifi_service_connect() != ESP_OK) {
        /* ⚠️ 先切回列表页再写提示 —— 失败可能发生在密码页（键盘的 OK 键），
         *    而 s_ap_status 属于列表页，留在密码页的话用户什么都看不到。 */
        show_page(PAGE_WIFI);
        lv_label_set_text(s_ap_status, "Cannot start connection (see log)");
        return;
    }

    /* 回列表页等结果。connect 是非阻塞的，所以要靠 on_conn_poll 等出来。 */
    show_page(PAGE_WIFI);
    lv_label_set_text(s_ap_status, "Connecting...");
    s_conn_waited = 0;
    if (s_conn_timer == NULL) {
        s_conn_timer = lv_timer_create(on_conn_poll, CONN_POLL_MS, NULL);
    } else {
        lv_timer_reset(s_conn_timer);       /* 上一次的还没停就又连了一次 */
    }
}

/* ===================== P2：WiFi 列表 ===================== */

static void on_up_to_root(lv_event_t *e)
{
    (void)e;
    show_page(PAGE_ROOT);
}

/*
 * 点某一行之后的动作。
 * user_data 里存的是**下标值**（不是地址）—— 指向 s_aps[] 里那一条。
 */
static void on_ap_clicked(lv_event_t *e)
{
    const size_t idx = (size_t)(intptr_t)lv_event_get_user_data(e);
    if (idx >= s_ap_count) {
        return;                             /* 理论上到不了这儿 */
    }

    snprintf(s_pending_ssid, sizeof(s_pending_ssid), "%s", s_aps[idx].ssid);

    if (!s_aps[idx].secure) {
        /* 开放网络：没有可输的东西，直接连（空密码在驱动那边会推出 WIFI_AUTH_OPEN） */
        ESP_LOGI(TAG, "选中开放网络: %s", s_pending_ssid);
        connect_now("");
        return;
    }

    /* 加密网络：去密码页。提示里带上 SSID，让用户确认自己没点错行 */
    lv_label_set_text_fmt(s_pass_hint, "Password for: %s", s_pending_ssid);
    lv_textarea_set_text(s_pass_ta, "");
    show_page(PAGE_PASS);
}

/*
 * 扫描并填列表。**阻塞 2~4 秒**（这是刻意的取舍：不引入任务/异步回调，
 * 代价是这段时间界面冻住。见 wifi_service_scan() 的说明）。
 */
static void wifi_start_scan(void)
{
    if (s_scanning) {
        return;                             /* 重入保护 */
    }
    s_scanning = true;

    lv_obj_clean(s_ap_list);                /* 清掉上一次的结果 */
    lv_label_set_text(s_ap_status, "Scanning...");
    /* ⚠️ 必须立刻刷一次：下面一阻塞 2~4 秒，不刷的话连 "Scanning..." 都看不到。
     *    （正常渲染要等本次事件处理完，而那要等扫描结束。） */
    lv_refr_now(NULL);

    s_ap_count = 0;
    const esp_err_t err = wifi_service_scan(s_aps, WIFI_SERVICE_SCAN_MAX, &s_ap_count);

    if (err != ESP_OK) {
        lv_label_set_text(s_ap_status, "Scan failed (see log)");
    } else if (s_ap_count == 0) {
        lv_label_set_text(s_ap_status, "No network found");
    } else {
        lv_label_set_text_fmt(s_ap_status, "%u networks found", (unsigned)s_ap_count);

        char row[96];
        for (size_t i = 0; i < s_ap_count; i++) {
            /* ⚠️ SSID 是外部数据，最长 32 字节，所以用 "%.32s" 限死宽度。
             *    加密方式也**必须带精度**（"%.4s"）—— 传个条件表达式给 %s 时
             *    GCC 推不出上界，会以 -Werror=format-truncation 报错
             *    （photo.c 里踩过同一个坑）。
             *    中文 SSID 在这里会变成一串占位方块（没有中文字体），是已知限制。 */
            snprintf(row, sizeof(row), "%.32s    %.4s    %d dBm",
                     s_aps[i].ssid,
                     s_aps[i].secure ? "WPA2" : "OPEN",
                     (int)s_aps[i].rssi);

            /* icon 传 NULL：列表行只用文字，不为图标再引一套资源 */
            lv_obj_t *btn = lv_list_add_button(s_ap_list, NULL, row);
            lv_obj_add_event_cb(btn, on_ap_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        }
    }

    /* ⚠️ 已知的粗糙面：扫描期间屏幕是冻的，用户若在那 2~4 秒里又点了一下，
     *    这一次触摸会排在输入队列里，解冻后被当成一次点击处理 —— 万一正好落在
     *    刚生成的某一行上，就会直接跳进密码页。概率很低，先不做输入屏蔽；
     *    真觉得烦，就在上面循环前记一个 lv_tick_get()，然后在 on_ap_clicked 里
     *    忽略"距上一次填表不到 300ms"的点击。 */
    s_scanning = false;
}

/* 点 P1 的 "WiFi" 那一行：切到列表页并立刻扫描 */
static void on_wifi_row_clicked(lv_event_t *e)
{
    (void)e;
    show_page(PAGE_WIFI);
    wifi_start_scan();
}

/* ===================== P3：密码输入 ===================== */

static void on_up_to_wifi(lv_event_t *e)
{
    (void)e;
    show_page(PAGE_WIFI);
}

/*
 * 键盘自带 OK / Close 两个键，分别发 LV_EVENT_READY / LV_EVENT_CANCEL ——
 * 直接拿它们当"连接"和"取消"，不再另做两个按钮。
 */
static void on_kb_event(lv_event_t *e)
{
    const lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_READY) {
        connect_now(lv_textarea_get_text(s_pass_ta));
    } else if (code == LV_EVENT_CANCEL) {
        show_page(PAGE_WIFI);
    }
}

/* ===================== 建三个页面 ===================== */

static void build_root_page(void)
{
    page_build(PAGE_ROOT);

    /* 宽度给死、高度由内容撑（lv_list 内部是 flex 列，所以 LV_SIZE_CONTENT 可用）。
     * 列表跟着内容长，以后加设置项不用改这里。 */
    lv_obj_t *list = lv_list_create(s_pages[PAGE_ROOT]);
    lv_obj_set_size(list, 420, LV_SIZE_CONTENT);
    lv_obj_align(list, LV_ALIGN_TOP_LEFT, 0, 0);

    /* 目前只有一项 —— 点进去是 WiFi 列表。以后加设置项就往这个列表里继续
     * add_button，页面本身和页面切换都不用动。 */
    lv_obj_t *wifi_row = lv_list_add_button(list, NULL, "WiFi");
    lv_obj_add_event_cb(wifi_row, on_wifi_row_clicked, LV_EVENT_CLICKED, NULL);
}

static void build_wifi_page(void)
{
    page_build(PAGE_WIFI);

    /* 页面内的"上一级"。和状态栏那个 "Back"（回桌面）不是一回事，措辞上区分开。 */
    lv_obj_t *up = lv_button_create(s_pages[PAGE_WIFI]);
    lv_obj_set_size(up, 160, 40);
    lv_obj_align(up, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_add_event_cb(up, on_up_to_root, LV_EVENT_CLICKED, NULL);
    lv_obj_t *up_lb = lv_label_create(up);
    lv_label_set_text(up_lb, "< Settings");
    lv_obj_center(up_lb);

    s_ap_status = lv_label_create(s_pages[PAGE_WIFI]);
    lv_label_set_text(s_ap_status, "Not scanned yet");
    lv_obj_align(s_ap_status, LV_ALIGN_TOP_LEFT, 176, 12);

    s_ap_list = lv_list_create(s_pages[PAGE_WIFI]);
    lv_obj_set_size(s_ap_list, LCD_SCREEN_H_RES - 32,
                    PAGE_CONTENT_H - WIFI_PAGE_TOP_H);
    lv_obj_align(s_ap_list, LV_ALIGN_TOP_LEFT, 0, WIFI_PAGE_TOP_H);
}

static void build_pass_page(void)
{
    page_build(PAGE_PASS);

    lv_obj_t *back = lv_button_create(s_pages[PAGE_PASS]);
    lv_obj_set_size(back, 140, 40);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_add_event_cb(back, on_up_to_wifi, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_lb = lv_label_create(back);
    lv_label_set_text(back_lb, "< WiFi");
    lv_obj_center(back_lb);

    s_pass_hint = lv_label_create(s_pages[PAGE_PASS]);
    lv_label_set_text(s_pass_hint, "Password");
    lv_obj_align(s_pass_hint, LV_ALIGN_TOP_LEFT, 156, 12);

    s_pass_ta = lv_textarea_create(s_pages[PAGE_PASS]);
    lv_textarea_set_one_line(s_pass_ta, true);
    /* 刻意**不掩码**：WiFi 密码输错太常见，看得见比藏起来实用。
     * 想改成掩码就打开下面这行（但那样用户没法自己核对，不建议）。
     *   lv_textarea_set_password_mode(s_pass_ta, true); */
    lv_obj_set_size(s_pass_ta, LCD_SCREEN_H_RES - 32, 48);
    lv_obj_align(s_pass_ta, LV_ALIGN_TOP_LEFT, 0, WIFI_PAGE_TOP_H);

    lv_obj_t *kb = lv_keyboard_create(s_pages[PAGE_PASS]);
    lv_obj_set_size(kb, LCD_SCREEN_H_RES - 32, 260);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    /* 默认就是全小写 + 数字/符号可切页。WiFi 密码大小写敏感，
     * 需要大写时按键盘上的 shift 键即可 —— 不做自动判断。 */
    lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(kb, s_pass_ta);
    lv_obj_add_event_cb(kb, on_kb_event, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(kb, on_kb_event, LV_EVENT_CANCEL, NULL);
}

/* ===================== App 生命周期 ===================== */

static void settings_enter(void)
{
    ESP_LOGI(TAG, "enter");

    lvgl_port_lock(0);

    s_scr = lv_obj_create(NULL);
    s_ap_count = 0;             /* 数组本身不用清：条数归零就够了 */

    /* 三个页面一次性建好，之后只改可见性（见文件顶部） */
    build_root_page();
    build_wifi_page();
    build_pass_page();

    show_page(PAGE_ROOT);       /* 顺便把状态栏标题刷成 "Settings" */

    lv_screen_load(s_scr);

    lvgl_port_unlock();
}

static void settings_leave(void)
{
    ESP_LOGI(TAG, "leave");

    lvgl_port_lock(0);

    /* ⚠️ 定时器不属于 s_scr，删 screen 不会带走它 —— enter 申请的游离资源
     *    在 leave 里一个都不能漏（clock.c 的同一条纪律）。 */
    stop_conn_poll();

    lv_obj_delete_async(s_scr);     /* 本回调可能正跑在 s_scr 里某个对象的事件上 */
    s_scr = NULL;
    for (int i = 0; i < PAGE_COUNT; i++) {
        s_pages[i] = NULL;
    }
    s_ap_list   = NULL;
    s_ap_status = NULL;
    s_pass_ta   = NULL;
    s_pass_hint = NULL;
    s_ap_count  = 0;
    s_scanning  = false;

    lvgl_port_unlock();
}

static const app_desc_t s_desc = {
    .name  = "Settings",
    /* 暂时没有图标资源：picture_icon() 查不到会返回 NULL，桌面就只显示名字
     * （加图标的流程见 README 的"图像资源"一节）。 */
    .icon  = "",
    .enter = settings_enter,
    .leave = settings_leave,
};

void settings_register(void)
{
    app_manager_register(&s_desc);
}
