/*
 * llm_client —— 实现（模型说明见头文件）
 *
 * 结构就四块：一个常驻任务、一个 HTTP 事件回调（动态攒 body）、一组组/解 JSON
 * 的函数、以及把 HTTP 状态码翻成"人话"的映射。
 */
#include "llm_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"

static const char *TAG = "llm";

/* ==========================================================================
 * ⚠️⚠️ 临时的测试用 API Key —— **提交前删掉这一行的内容**（清成空串即可）
 *
 * 测试版刻意不做 NVS、也不做设置界面：Key 就写死在这里，方便快速联调。
 * **不要把真实 Key 提交进 git。**
 * 留空时本组件不发请求，直接回一条 err="未配置 API Key"，App 会把它显示出来。
 * ========================================================================== */
#define LLM_API_KEY   ""

/* DeepSeek 的 OpenAI 兼容接口。换厂商只要改这几行（协议是一样的）。
 *
 * ⚠️ 模型名按**当前**官方文档写：只有 `deepseek-flash`（V4.1-Flash，便宜快）和
 *    `deepseek-v4-pro`（V4-Pro，贵）两个。老教程里那个 `deepseek-chat`（V3 时代）
 *    在现在的官方模型表里**已经没有了**，填它会 400 —— 别照抄网上的旧例子。
 *    （下划线写法 `deepseek_chat` 从来没有过。） */
#define LLM_URL            "https://api.deepseek.com/chat/completions"
#define LLM_MODEL          "deepseek-flash"

/* 系统提示词。
 *
 * ⚠️ **别再说"回答要简短"** —— 那句话和做流式的目的直接冲突：流式的收益全部来自
 *    长回答（实测生成 ~235 字/s，2000 字能省 8~10 秒；几十字的回答只省 0.7 秒）。
 *    要求它"几句话以内"，等于把这个收益按死在地上。
 *
 * 三条约束都是冲着**这台设备**去的：
 *   1. 长度跟着问题走 —— 既不啰嗦也不该省的省，这才是聊天该有的样子；
 *   2. 给个 1000 字的软上限 —— 不是为了省字，是**避免常态性撞 `max_tokens`**：
 *      我们没有解析 `finish_reason`，撞线时回答会莫名其妙停在半句、没有任何提示
 *      （`max_tokens=2048` 约合 2000 汉字，留了余量）；顺带也把下行量控制在 ~180KB。
 *   3. **禁用 Markdown** —— 那个 label 是纯文本，`**`、`#`、反引号会原样显示成噪音。
 *      换行是会被正常渲染的，所以让它用换行 + "- " 分点。 */
#define LLM_SYSTEM_PROMPT                                                  \
    "你是一个运行在嵌入式设备上的助手，回答显示在一块小屏幕上。"            \
    "长度跟着问题走：简单问题一两句就够；需要展开的（解释原理、介绍、比较、"    \
    "操作步骤）就写详细，不要为了省字把内容砍掉。通常控制在 1000 字以内，"      \
    "除非用户明确要求更长。"                                              \
    "用纯文本和换行分段，不要用 Markdown 标记（星号、井号、反引号、表格竖线）"  \
    "——设备不做渲染，那些符号只会原样显示成噪音。"

/* 单次回复的 token 上限。
 * 512 太少了 —— 512 token ≈ 512 个汉字，稍微要展开讲的回答就会被截断（表现为
 * finish_reason=length）。2048 约合 2000 汉字，按实测 ~235 字/s 的生成速度
 * 最多约 8.7 秒。再往上加之前先想清楚：LVGL 那条 label 的重排代价是随长度涨的。 */
#define LLM_MAX_TOKENS     2048

/* 一次请求允许携带的**正文总量**上限（所有消息文本加起来，不含 JSON 壳子）。
 * 纯文本对话离它很远（几十条也就几 KB）。它守的是"别把堆吃光"，
 * 不是模型窗口 —— 那个是 1M，跟我们没关系。
 * ⚠️ 以后要发图片，body 会到 100KB 级，这条和 do_request 里的缓冲都要重新算。 */
#define LLM_REQ_MAX        (64 * 1024)

/* HTTP 总超时。**改成流式之后这个值是"两次数据之间"的上限，不是总时长上限**
 * （`timeout_ms` 落在 socket 的 SO_RCVTIMEO 上）：流式下服务端会持续吐字，
 * 所以只有"卡住不发了"才会触发。给 120s 是为了兜住思考模式万一没关成的情况。 */
#define LLM_HTTP_TIMEOUT_MS 120000

/* 累积正文的上限见头文件的 LLM_TEXT_MAX（调用方也要按它开缓冲）。 */

/* SSE 单行上限。实测每个 chunk 的整行约 270~400 字节，2048 足够宽裕；
 * 真超了说明对端不是我们预期的格式，丢掉该行比让它吃掉内存好。 */
#define LLM_LINE_MAX       2048

/* 非 200 时留存的原始响应体上限 —— 只为了抠服务端给的 error.message。
 * 错误 JSON 只有两三百字节，1KB 绰绰有余。 */
#define LLM_ERR_BODY_MAX   1024

/* ===================== 任务参数 =====================
 * 栈 8192：与 IDF 官方 HTTPS 例程、本项目的 weather_service 同值（TLS 握手 +
 * 证书链校验的栈开销大）。优先级/核也一致：别抢 core 0 上 USB 驱动的核。 */
#define LLM_TASK_STACK 8192
#define LLM_TASK_PRIO  3
#define LLM_TASK_CORE  1

/* ===================== 状态 ===================== */

static QueueHandle_t     s_req_q  = NULL;   /* 深度 1：一次只装一条请求 */
static QueueHandle_t     s_reply_q = NULL;  /* 深度 1：丢旧保新 */
static SemaphoreHandle_t s_idle    = NULL;  /* "空闲"令牌：worker 收下时持有，投完结果交还 */
static bool              s_inited  = false;

/* 请求队列的元素。
 *
 * ⚠️ 它**不持有文本本体**，只带两把指针（items 指向一个 llm_msg_t 数组、pool 是
 *    所有正文拼在一起的大块）。这样才装得下"整段历史"这种变长内容 ——
 *    队列项本身只有 12 字节。
 *
 * ⚠️ 这两块是 request() 自己 malloc 的**拷贝**（不是调用方的历史数组），
 *    所有权在入队那一刻就转移给 worker，由 worker 在干完活后 free。
 *    所以 request() 一返回，调用方随便改/释放自己那份都安全。 */
typedef struct {
    int         n;
    llm_msg_t  *items;   /* malloc(n * sizeof(llm_msg_t))；每条的 text 指向 pool 内部 */
    char       *pool;    /* malloc(正文总字节数) */
} llm_req_t;

/* =====================================================================
 * 流式输出（SSE）
 *
 * 这一段全部跑在调用 esp_http_client_perform() 的任务里（也就是 llm_task）。
 *
 * 数据长这样（DeepSeek/OpenAI 兼容，Content-Type: text/event-stream）：
 *     data: {"choices":[{"delta":{"content":"你"},"finish_reason":null}]}
 *     data: {"choices":[{"delta":{"content":"好"},"finish_reason":null}]}
 *     data: [DONE]
 * 在 HTTP 层面它就是一块普通的响应体（chunked），**不是新协议、不是 WebSocket**。
 * 服务端每算出一个 token 就发一片，所以"流式"其实是它最自然的形态。
 *
 * ⚠️ 唯一的真坑：**HTTP 的块边界和 SSE 的行边界没有任何关系**。
 *    一次 HTTP_EVENT_ON_DATA 完全可能只给半行（`data: {"choices":[{"del`），
 *    剩下半行在下一片。所以必须跨回调攒行，见到 '\n' 才当作一整行解析。
 * ===================================================================== */

/* 累积起来的正文 —— 就是这次回复的全部内容，边收边攒。
 * ⚠️ 单生产者单消费者：只有 worker 追加、只有取正文的那个任务（LVGL 任务）读。
 *    worker 只往后追加、从不改动已经写过的部分，所以读者把 s_text_len 读进局部量
 *    之后 [0, n) 就是稳定的 —— 不需要锁，也别在这里加花活。 */
static char   s_text[LLM_TEXT_MAX];
static size_t s_text_len = 0;
static bool   s_text_full_warned = false;

/* 追加一小片正文。超上限就丢（只告警一次）—— 等于这次回复被截断。 */
static void text_append(const char *s)
{
    const size_t n = strlen(s);
    if (s_text_len + n >= LLM_TEXT_MAX) {
        if (!s_text_full_warned) {
            ESP_LOGW(TAG, "正文已达 %d KB 上限，后续增量丢弃", LLM_TEXT_MAX / 1024);
            s_text_full_warned = true;
        }
        return;
    }
    memcpy(s_text + s_text_len, s, n);
    s_text_len += n;
    s_text[s_text_len] = '\0';
}

/* 事件回调的上下文。约 3KB。
 * ⚠️ **放静态区，不放栈上** —— llm_task 那 8192 的栈峰值被 TLS 握手占满
 *    （同 weather_service 的 s_report 那条约束）。同时只可能有一个请求在跑
 *    （空闲令牌守着），所以静态区不会打架。 */
typedef struct {
    char   line[LLM_LINE_MAX];          /* 还没拼完的当前行 */
    size_t line_len;
    bool   line_drop;                   /* 本行超长：丢到下一个换行为止 */

    char   err_body[LLM_ERR_BODY_MAX];  /* 非 200 时用它抠服务端给的原因 */
    size_t err_len;
} resp_ctx_t;

static resp_ctx_t s_resp;

static void sse_line(const char *line, size_t len);

/* 把新到的字节喂进"行装配器"。 */
static void sse_feed(resp_ctx_t *ctx, const char *data, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        const char c = data[i];

        if (c != '\n') {
            if (ctx->line_len < sizeof(ctx->line) - 1) {
                ctx->line[ctx->line_len++] = c;
            } else if (!ctx->line_drop) {
                ctx->line_drop = true;
                ESP_LOGW(TAG, "SSE 单行超过 %u 字节，本行丢弃", (unsigned)sizeof(ctx->line) - 1);
            }
            continue;
        }

        /* 攒到一整行了 */
        if (ctx->line_len > 0 && ctx->line[ctx->line_len - 1] == '\r') {
            ctx->line_len--;                /* 容忍 CRLF */
        }
        ctx->line[ctx->line_len] = '\0';
        if (!ctx->line_drop) {
            sse_line(ctx->line, ctx->line_len);
        }
        ctx->line_len  = 0;
        ctx->line_drop = false;
    }
}

/* 处理一整行。要认的只有两种，其余（空行、以 ':' 开头的注释行、别的字段）全跳过。 */
static void sse_line(const char *line, size_t len)
{
    /* SSE 规范里 `data:` 后面那个空格是**可选**的，所以两种都认 —— 只认一种的话，
     * 万一对端改成 `data:{...}`，症状是"200 但一个字都没有"，很难查。 */
    const char *payload;
    if (len >= 6 && strncmp(line, "data: ", 6) == 0) {
        payload = line + 6;
    } else if (len >= 5 && strncmp(line, "data:", 5) == 0) {
        payload = line + 5;
    } else {
        return;
    }

    /* `data: [DONE]` = 服务端明确宣告结束。
     * ⚠️ 但**不靠它判断结束** —— perform() 返回就是结束，这条只当日志用。 */
    if (strcmp(payload, "[DONE]") == 0) {
        ESP_LOGD(TAG, "SSE: [DONE]");
        return;
    }

    cJSON *root = cJSON_Parse(payload);
    if (root == NULL) {
        return;                             /* 单条解析失败不影响后面的 */
    }
    const cJSON *choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
    const cJSON *first   = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : NULL;
    const cJSON *delta   = cJSON_IsObject(first)
                               ? cJSON_GetObjectItemCaseSensitive(first, "delta")
                               : NULL;
    /* ⚠️ 流式给的是 `delta`（这一小片），非流式才是 `message`（整段）—— 两者不一样，
     *    这是改流式时最容易拿错的地方。 */
    const cJSON *content = cJSON_IsObject(delta)
                               ? cJSON_GetObjectItemCaseSensitive(delta, "content")
                               : NULL;

    if (cJSON_IsString(content) && content->valuestring != NULL
        && content->valuestring[0] != '\0') {
        text_append(content->valuestring);
    }
    cJSON_Delete(root);
}

static esp_err_t http_event_cb(esp_http_client_event_t *evt)
{
    resp_ctx_t *ctx = (resp_ctx_t *)evt->user_data;

    if (evt->event_id != HTTP_EVENT_ON_DATA || ctx == NULL ||
        evt->data == NULL || evt->data_len <= 0) {
        return ESP_OK;
    }
    const char *p = (const char *)evt->data;
    const size_t n = (size_t)evt->data_len;

    /* 顺手留一份原始响应的开头。200 时它是 SSE 文本的前 1KB（用不上）；
     * 非 200 时它就是错误 JSON，靠它抠服务端给的原因。
     * 刻意**不在回调里判断状态码** —— 那要去查 client 的状态，多一处判据不划算。 */
    if (ctx->err_len < sizeof(ctx->err_body) - 1) {
        const size_t room = sizeof(ctx->err_body) - 1 - ctx->err_len;
        const size_t c = (n < room) ? n : room;
        memcpy(ctx->err_body + ctx->err_len, p, c);
        ctx->err_len += c;
        ctx->err_body[ctx->err_len] = '\0';
    }

    sse_feed(ctx, p, n);
    return ESP_OK;
}

/* =====================================================================
 * JSON
 * ===================================================================== */

/* 组请求体。返回 malloc 出来的字符串（调用方 free），失败返回 NULL。
 *
 * ⚠️ 必须用 cJSON 建、**不能拼字符串** —— 用户输入里可能有引号、反斜杠、
 *    换行、控制字符，手拼 JSON 转义是必错无疑的。
 *
 * 结构 = system 提示词 + 调用方给的整段历史（含本次提问），顺序原样保留。
 * 服务端无状态，所以每次都得整段重发 —— 见头文件"多轮上下文"。 */
static char *build_body(const llm_msg_t *msgs, int n)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }

    cJSON_AddStringToObject(root, "model", LLM_MODEL);
    /* **流式**：服务端每算出一个 token 就发一片（SSE），而不是攒齐了整段再回。
     * 收益 = 省下整段生成时间（实测生成约 235 字/s，所以 2000 字能省 8~10 秒）；
     * 上限是固定的握手+排队+prefill（实测 0.45~0.8 秒），流式省不掉。 */
    cJSON_AddBoolToObject(root, "stream", true);
    cJSON_AddNumberToObject(root, "max_tokens", LLM_MAX_TOKENS);

    /* 关掉思考模式 —— **它默认是开的，而且 effort 默认 high**。
     * 开着的话服务端会先输出一整段思维链（走 `reasoning_content` 字段，和 content
     * 同级；本组件没解析它），后果两条：
     *   ① 非流式调用要干等更久（可能几十秒），屏幕上只有"正在生成…"；
     *   ② `max_tokens` 的预算可能被思维链吃掉，正文被截断甚至为空。
     * 我们只要最终答案，所以显式关掉。
     * ⚠️ 这是 **OpenAI 格式**下的开关（Anthropic 格式是 `reasoning.effort=none`，
     *    不是一回事，别记混）。万一服务端对多出来的这个字段报 400，删掉这段即可 ——
     *    后果只是变慢，不会功能失效。 */
    cJSON *think = cJSON_CreateObject();
    if (think != NULL) {
        cJSON_AddStringToObject(think, "type", "disabled");
        cJSON_AddItemToObject(root, "thinking", think);
    }

    cJSON *arr = cJSON_CreateArray();
    if (arr == NULL) {
        cJSON_Delete(root);
        return NULL;
    }
    cJSON_AddItemToObject(root, "messages", arr);

    /* 系统提示词：**由本组件加在最前面**，调用方不用管（也就不会有人忘了加）。 */
    cJSON *sys = cJSON_CreateObject();
    if (sys == NULL) {
        cJSON_Delete(root);                             /* 连同已挂进去的 arr 一起释放 */
        return NULL;
    }
    cJSON_AddStringToObject(sys, "role", "system");
    cJSON_AddStringToObject(sys, "content", LLM_SYSTEM_PROMPT);
    cJSON_AddItemToArray(arr, sys);

    /* 调用方给的整段历史，按顺序原样搬进 messages。
     * ⚠️ 失败时 cJSON_Delete(root) 会把已经挂进去的那几条一起释放，不用逐条回收。 */
    for (int i = 0; i < n; i++) {
        cJSON *m = cJSON_CreateObject();
        if (m == NULL) {
            cJSON_Delete(root);
            return NULL;
        }
        cJSON_AddStringToObject(m, "role", msgs[i].is_ai ? "assistant" : "user");
        cJSON_AddStringToObject(m, "content", msgs[i].text);
        cJSON_AddItemToArray(arr, m);
    }

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);                                 /* 深拷贝出来的 out 不受影响 */
    return out;
}

/* 从非 200 的响应里抠出服务端给的 error.message（能抠到就返回 true）。
 * 这是**为了联调期能看懂 400**（参数/额度问题服务端会给具体原因），
 * 不是必需品 —— 抠不到时就只用状态码翻出来的那句话。 */
static bool extract_error_message(const char *body, char *out, size_t out_size)
{
    if (body == NULL || out == NULL || out_size == 0) {
        return false;
    }
    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        return false;
    }

    bool ok = false;
    const cJSON *err = cJSON_GetObjectItemCaseSensitive(root, "error");
    const cJSON *msg = cJSON_IsObject(err)
                           ? cJSON_GetObjectItemCaseSensitive(err, "message")
                           : NULL;
    if (cJSON_IsString(msg) && msg->valuestring != NULL) {
        strncpy(out, msg->valuestring, out_size - 1);
        out[out_size - 1] = '\0';
        ok = true;
    }
    cJSON_Delete(root);
    return ok;
}

/* 把 HTTP 状态码翻成"人话"（App 会原样显示）。能取到服务端原因就附在后面。 */
static void map_http_error(int status, const char *body, llm_reply_t *r)
{
    switch (status) {
    case 400: snprintf(r->err, sizeof(r->err), "请求有误（400）"); break;
    case 401: snprintf(r->err, sizeof(r->err), "API Key 无效或未授权（401）"); break;
    case 402: snprintf(r->err, sizeof(r->err), "账户余额不足（402）"); break;
    case 429: snprintf(r->err, sizeof(r->err), "请求过于频繁，稍后再试（429）"); break;
    default:
        if (status >= 500) {
            snprintf(r->err, sizeof(r->err), "服务端错误（%d）", status);
        } else {
            snprintf(r->err, sizeof(r->err), "HTTP %d", status);
        }
        break;
    }

    char detail[96] = { 0 };
    if (extract_error_message(body, detail, sizeof(detail))) {
        const size_t used = strlen(r->err);
        if (used + 4 < sizeof(r->err)) {
            /* 用 %.80s 把长度**写死**：不然 -Wformat-truncation 会对着"已知 96 字节的
             * 源 + 剩下的 96-used 字节"报"可能截断"（IDF 那套警告标志下会变成 error）。 */
            snprintf(r->err + used, sizeof(r->err) - used, "：%.80s", detail);
        }
    }
}

/* =====================================================================
 * 一次请求
 * ===================================================================== */

static void do_request(const llm_req_t *req, llm_reply_t *r)
{
    char *body = build_body(req->items, req->n);
    if (body == NULL) {
        snprintf(r->err, sizeof(r->err), "组请求失败（内存不足）");
        return;
    }

    /* 每轮开始先清空响应上下文（行装配器 + 错误体）。见 resp_ctx_t 那条"放静态区"的说明。 */
    memset(&s_resp, 0, sizeof(s_resp));

    const esp_http_client_config_t cfg = {
        .url               = LLM_URL,
        .method            = HTTP_METHOD_POST,
        .timeout_ms        = LLM_HTTP_TIMEOUT_MS,
        /* 用 IDF 自带的 Mozilla 根证书包做校验。**证书校验依赖正确的系统时间**
         * —— 所以这条链路的前提是先有 net_time(SNTP) 对过时（和 weather 一样）。 */
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler     = http_event_cb,
        .user_data         = &s_resp,
    };

    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (cli == NULL) {
        free(body);
        snprintf(r->err, sizeof(r->err), "HTTP 客户端初始化失败");
        return;
    }

    /* ⚠️ Bearer 头显式拼一个缓冲（不直接拼宏字面量）：将来 Key 改成从 NVS 读时，
     *    这里不用动。 */
    char auth[160];
    snprintf(auth, sizeof(auth), "Bearer %s", LLM_API_KEY);

    esp_http_client_set_header(cli, "Content-Type", "application/json");
    /* ⚠️ 刻意**不设** `Accept: text/event-stream`：是否流式由 body 里的 `stream` 决定，
     *    实测（curl 不带 Accept）SSE 就已经正常。多设一个头只会多一个被拒/被协商的可能。 */
    esp_http_client_set_header(cli, "Authorization", auth);
    esp_http_client_set_post_field(cli, body, (int)strlen(body));

    const esp_err_t err    = esp_http_client_perform(cli);
    const int       status = esp_http_client_get_status_code(cli);
    esp_http_client_cleanup(cli);

    r->http_status = status;

    if (err != ESP_OK) {
        /* 同 map_http_error：长度写死，免得 -Wformat-truncation 报"可能截断" */
        snprintf(r->err, sizeof(r->err), "网络错误: %.64s", esp_err_to_name(err));
        ESP_LOGW(TAG, "perform failed: %s", esp_err_to_name(err));
    } else if (status != 200) {
        /* 非 200 多半是普通 JSON 错误体（不是 SSE），http_event_cb 已经留了开头一段。 */
        map_http_error(status, s_resp.err_body, r);
    } else if (s_text_len == 0) {
        /* 200 但一片正文都没收到：SSE 格式变了，或者回复真的是空的。 */
        snprintf(r->err, sizeof(r->err), "没有收到正文（检查是否还支持 SSE）");
        ESP_LOGW(TAG, "200 but no content, sse_bytes=%u", (unsigned)s_resp.err_len);
    } else {
        /* **正文已经在 sse_line() 里一片片攒进 s_text 了** —— 这里只是把它拷一份
         * 交出去（历史、所有权都和以前一样：App 用完调 llm_client_reply_free）。 */
        r->text = strdup(s_text);
        if (r->text != NULL) {
            r->ok = true;
        } else {
            snprintf(r->err, sizeof(r->err), "内存不足，回复装不下");
        }
    }

    ESP_LOGI(TAG, "http=%d ok=%d reply=%u bytes err=\"%s\"",
             status, (int)r->ok, r->text != NULL ? (unsigned)strlen(r->text) : 0u, r->err);

    free(body);
}

/* =====================================================================
 * 工作任务
 * ===================================================================== */

/* 投一条结果。先清掉没人取走的旧结果（连正文一起 free）——结果队列深度 1，
 * 不这样清的话旧结果会被 overwrite 直接覆盖，正文就永远漏了。 */
static void post_result(const llm_reply_t *r)
{
    if (s_reply_q == NULL) {
        return;
    }
    llm_reply_t stale;
    while (xQueueReceive(s_reply_q, &stale, 0) == pdTRUE) {
        free(stale.text);
    }
    xQueueSend(s_reply_q, r, 0);
}

static void llm_task(void *arg)
{
    (void)arg;

    llm_req_t req;
    for (;;) {
        if (xQueueReceive(s_req_q, &req, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        llm_reply_t r = { .ok = false, .http_status = 0, .err = { 0 }, .text = NULL };

        /* 没配 Key 就不发请求：直接给一句人话，省得用户对着一串 TLS 日志猜。
         * 这一步是为"测试版"服务的 —— 以后 Key 从 NVS 读，这里换成"读不到"的判断。 */
        if (LLM_API_KEY[0] == '\0') {
            snprintf(r.err, sizeof(r.err), "未配置 API Key（见 llm_client.c 顶部）");
            ESP_LOGW(TAG, "no api key configured");
        } else {
            do_request(&req, &r);
        }

        /* 请求历史的拷贝是本组件自己 malloc 的，不管成没成都得还回去。 */
        free(req.items);
        free(req.pool);

        post_result(&r);
        xSemaphoreGive(s_idle);     /* 交还"空闲"令牌，允许下一次请求 */
    }
}

/* =====================================================================
 * 对外接口
 * ===================================================================== */

esp_err_t llm_client_init(void)
{
    if (s_inited) {
        return ESP_OK;                  /* 幂等 */
    }

    s_req_q   = xQueueCreate(1, sizeof(llm_req_t));
    s_reply_q = xQueueCreate(1, sizeof(llm_reply_t));
    s_idle    = xSemaphoreCreateBinary();
    if (s_req_q == NULL || s_reply_q == NULL || s_idle == NULL) {
        ESP_LOGE(TAG, "create sync objects failed");
        return ESP_ERR_NO_MEM;
    }
    /* 二元信号量默认是"空"的；这里的语义是"空闲令牌"，所以先给出去一个。 */
    xSemaphoreGive(s_idle);

    if (xTaskCreatePinnedToCore(llm_task, "llm", LLM_TASK_STACK, NULL,
                                LLM_TASK_PRIO, NULL, LLM_TASK_CORE) != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        return ESP_ERR_NO_MEM;
    }

    s_inited = true;
    ESP_LOGI(TAG, "ready (worker: %d bytes, core %d, url: %s, key: %s)",
             LLM_TASK_STACK, LLM_TASK_CORE, LLM_URL,
             LLM_API_KEY[0] != '\0' ? "configured" : "MISSING");
    return ESP_OK;
}

bool llm_client_busy(void)
{
    if (s_idle == NULL) {
        return false;
    }
    /* 试着拿一下再立刻还 —— 拿到=空闲，拿不到=忙。刻意和 request() 用同一个判据，
     * 避免两处判断不一致。 */
    if (xSemaphoreTake(s_idle, 0) == pdTRUE) {
        xSemaphoreGive(s_idle);
        return false;
    }
    return true;
}

esp_err_t llm_client_request(const llm_msg_t *msgs, int n)
{
    if (s_req_q == NULL || s_reply_q == NULL || s_idle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (msgs == NULL || n <= 0 || n > LLM_MAX_MSGS) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 先把要拷多少算清楚：一次分配，一次拷贝，避免边拷边走一步才发现越界。 */
    size_t pool_bytes = 0;
    for (int i = 0; i < n; i++) {
        if (msgs[i].text == NULL || msgs[i].text[0] == '\0') {
            return ESP_ERR_INVALID_ARG;     /* 空消息不该出现在 messages 里 */
        }
        pool_bytes += strlen(msgs[i].text) + 1;     /* +1 是结尾的 '\0' */
    }
    if (pool_bytes > LLM_REQ_MAX) {
        ESP_LOGW(TAG, "请求历史过大：%u 字节（上限 %d）", (unsigned)pool_bytes, LLM_REQ_MAX);
        return ESP_ERR_INVALID_SIZE;
    }

    /* 忙就直接拒绝，不排队（队列深度 1，排了也会被丢掉）。 */
    if (xSemaphoreTake(s_idle, 0) != pdTRUE) {
        return ESP_ERR_NOT_FINISHED;
    }

    /* 拿住令牌了 = worker 一定没在跑（它只在工作期间写 s_text），所以现在清累积缓冲
     * 是安全的。**必须在入队之前清** —— 入队后 worker 随时可能开始往里追加。 */
    s_text_len          = 0;
    s_text[0]           = '\0';
    s_text_full_warned  = false;

    /* 丢掉上一条没人取走的旧结果，并释放它的正文 —— 这是本组件唯一容易漏的地方。 */
    llm_reply_t stale;
    while (xQueueReceive(s_reply_q, &stale, 0) == pdTRUE) {
        free(stale.text);
    }

    /* 拷一份自己的历史（见 llm_req_t 的说明：所有权随入队转移给 worker）。 */
    llm_req_t req = { .n = n, .items = NULL, .pool = NULL };
    req.items = malloc(sizeof(llm_msg_t) * (size_t)n);
    req.pool  = malloc(pool_bytes);
    if (req.items == NULL || req.pool == NULL) {
        free(req.items);                /* free(NULL) 是安全的，不用分情况 */
        free(req.pool);
        xSemaphoreGive(s_idle);
        return ESP_ERR_NO_MEM;
    }

    size_t off = 0;
    for (int i = 0; i < n; i++) {
        const size_t len = strlen(msgs[i].text) + 1;
        memcpy(req.pool + off, msgs[i].text, len);
        req.items[i].text  = req.pool + off;
        req.items[i].is_ai = msgs[i].is_ai;
        off += len;
    }

    if (xQueueSend(s_req_q, &req, 0) != pdTRUE) {
        free(req.items);                /* 没投出去就自己收尾，否则这两块永远漏了 */
        free(req.pool);
        xSemaphoreGive(s_idle);         /* 令牌也要还回去，否则永远"忙" */
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

bool llm_client_poll(llm_reply_t *out)
{
    /* 会被 App 的定时器无条件调用，所以队列可能是 NULL（init 失败）——
     * xQueueReceive(NULL, ...) 会直接崩，必须挡住。 */
    if (out == NULL || s_reply_q == NULL) {
        return false;
    }
    return xQueueReceive(s_reply_q, out, 0) == pdTRUE;
}

void llm_client_reply_free(llm_reply_t *r)
{
    if (r == NULL) {
        return;
    }
    free(r->text);
    r->text = NULL;
}

size_t llm_client_copy_text(char *out, size_t cap)
{
    if (out == NULL || cap == 0) {
        return 0;
    }
    out[0] = '\0';

    /* ⚠️ 把长度读进局部量再拷：worker 只会往 s_text_len 后面追加，从不改动已经写过的
     *    部分，所以 [0, n) 这段是稳定的（单生产者单消费者，不需要锁）。 */
    const size_t n = s_text_len;
    if (n == 0) {
        return 0;
    }
    const size_t c = (n < cap - 1) ? n : cap - 1;
    memcpy(out, s_text, c);
    out[c] = '\0';
    return c;
}
