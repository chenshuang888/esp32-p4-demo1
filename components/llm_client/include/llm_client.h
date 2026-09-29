/*
 * llm_client —— 大模型对话能力组件（HTTPS POST DeepSeek 的 OpenAI 兼容接口）
 *
 * 定位：**"问一句、拿一段回复"这件事的封装**（回复是**流式**来的）。
 * 它不认识 LVGL、不认识 wifi_service、也不认识 kv_store ——
 * 只对外给六个动作：初始化 / 发起一次请求 / 读增量 / 取结果 / 是否忙 / 释放结果。
 *
 * ============================ 为什么必须有自己的任务 ============================
 *
 * 和 weather_service 同一条硬约束：渲染界面的 LVGL 任务栈只有 **7168 字节**
 * （esp_lvgl_port 的 ESP_LVGL_PORT_INIT_CONFIG() 默认值，见 components/ui/ui.c），
 * TLS 握手 + 证书链校验的栈开销远超它 —— 放进去轻则冻界面，重则爆栈。
 * 所以本组件自带一个 **8192 栈**的常驻任务（与 IDF 官方 HTTPS 例程、
 * 以及本项目的 weather_service 同值），LVGL 侧只投请求 + 取结果，全程不碰网络。
 *
 * ============================ 任务模型（镜像 weather_service） ============================
 *
 *      App: llm_client_request(msgs, n) ──send──> [请求队列，深度 1]
 *                                                    ↓
 *                    llm_task: 收请求 -> 组 JSON -> HTTPS POST（SSE 流式）
 *                                                    ↓
 *                    SSE 逐片解析 -> s_text（累积正文，静态区，见 .c）
 *                                                    ↓
 *      App: llm_client_copy_text()  <──读──  流式过程中刷新屏幕上的那条消息
 *      App: llm_client_poll(&r)     <──recv──  [结果队列，深度 1]（结束时拿完整正文）
 *
 *   - 请求队列深度 1 + `忙` 令牌：上一次没结束就**拒绝**新的（返回 ESP_ERR_NOT_FINISHED），
 *     而不是排队 —— 聊天场景排队没有意义（用户要的是最新那句的回复）
 *   - 结果队列深度 1：天然"丢旧保新"，App 来不及取也不会堆积
 *   - 常驻不销毁：生命周期 = llm_client_init()，与 App 进出无关
 *   - 失败也会投一条结果（ok=false），否则 App 会永远停在"正在生成…"
 *
 * ============================ 流式输出 ============================
 *
 * 请求带 `stream: true`，服务端以 **SSE**（Server-Sent Events）的形式**每算出一个
 * token 就发一片**，而不是攒齐整段再回。所以"流式"其实是模型最自然的形态；
 * 非流式反而是服务端多了一层"先攒后发"的缓冲。
 *
 * 收益 = **省下整段生成时间**（实测约 235 字/秒，所以 2000 字能省 8~10 秒；
 * 200 字约省 0.7 秒）。收益的上限是固定的握手+排队+prefill —— 实测 0.45~0.8 秒，
 * 流式省不掉，所以**首字不会"瞬间出来"**。
 *
 * 两个必须知道的事实（都是实测/源码确认过的，别凭印象）：
 *   1. 流式片里增量在 `choices[0].delta.content`，非流式才是 `message.content`。
 *   2. **HTTP 的块边界和 SSE 的行边界没有关系** —— 一次回调可能只给半行，
 *      所以 .c 里必须跨回调攒行。这是改流式时唯一真正的坑。
 *   3. 下行字节数会放大 ~25~60 倍（每个 chunk 都重发一整份 `id`/`model` 信封）：
 *      2000 字约 440KB。PC 上无所谓，设备上要过 C6 那条 SDIO。
 *
 * ⚠️ **没有"取消/停止"**：ESP-IDF 5.4.3 里"事件回调返回 ESP_FAIL 就中断 perform()"
 *    是**行不通的**（`esp_http_client.c` 把 `http_dispatch_event()` 的返回值丢了），
 *    而这个版本也没有 abort / perform_async 之类的 API。真要中止只能从另一个任务
 *    去关连接，头文件没有线程安全承诺，有崩溃风险 —— 所以现阶段不做。
 *
 * ============================ 与 weather_service 的关键差别 ============================
 *
 *   1. **POST + 自定义头**（Content-Type / Authorization），请求体是 JSON
 *   2. 响应是**流式的**，正文边收边攒（weather 是一次收完再解析）
 *   3. 回复正文是**堆上的指针**，所有权随结果交给 App —— 取到之后**必须**
 *      `llm_client_reply_free()`，否则每次对话漏一块
 *
 * ⚠️ 回复正文的分配（llm_task 里 malloc）与释放（LVGL 任务里 free）跨了两个任务。
 *    IDF 的堆是线程安全的，这是允许的 —— 但必须成对，别漏。
 *
 * ============================ 多轮上下文 ============================
 *
 * 服务端**是无状态的**：它不存会话，所以每次请求都必须在 `messages` 里
 * **重发你想让它看见的整段历史**（这是协议要求，不是本组件的选择 —— Anthropic 官方
 * 文档对 Messages API 的原话就是 "you always send the full conversational history"）。
 *
 * 于是"历史存哪"就成了一个设计问题。本组件选了**调用方持有**：
 *   - 调用方（App）本来就要把每条消息画到屏幕上、本来就有这些文本，重绘天然成立；
 *   - 请求失败时回滚只是"别追加"，不用在组件里维护 pending 槽。
 * 组件这一侧保持"发一次请求"的无状态定位 —— 除了 `LLM_API_KEY` 那点静态配置，
 * 它不记任何会话状态。
 *
 * 系统提示词由组件自动加在 `messages` 最前面，调用方不用管。
 *
 * ⚠️ 一次请求的 body 大小 = 你发过去的那段历史的序列化，**不等于**模型的上下文窗口
 *    （那是上限，远得很）。纯文本聊天里它只有几 KB，不是约束。真正会咬人的是
 *    "body 要经 TLS 上行过 C6 那条 SDIO"（比如哪天要发图片，body 会到 100KB 级）。
 *
 * ============================ 测试版的 Key ============================
 *
 * 目前是测试版：**API Key 写死在 llm_client.c 顶部**（不做 NVS、不做输入界面），
 * 位置和删除方法见那个文件开头的 banner。以后要改成运行时配置时，
 * 只需在 init 前把 Key 填进来，本头文件的接口不用动。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>     /* size_t：llm_client_copy_text() */

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 失败原因短语的最大字节数（中文一个字 3 字节，约 30 个汉字） */
#define LLM_ERR_MAX 96

/**
 * @brief 一次请求最多能带多少条消息
 *
 * 纯文本对话远远用不到这么多（一条几百字节，64 条也就几十 KB）。
 * 它只是个"别无限增长"的闸门，不是从模型窗口推出来的数 —— 两者无关。
 */
#define LLM_MAX_MSGS 64

/**
 * @brief 累积正文的上限（流式片一片片攒在这里，也是 `llm_client_copy_text()` 的容量口径）
 *
 * 调用方如果自己声明一个缓冲来接收 copy_text()，就按这个大小开。
 * max_tokens=2048 时正文最多约 6~8KB，16KB 有裕量；超了会丢弃后续增量（日志告警），
 * 等于这次回复被截断。
 */
#define LLM_TEXT_MAX (16 * 1024)

/**
 * @brief 对话里的一条消息
 *
 * 角色只有两种，所以用 bool 而不是字符串：省得调用方拼错 "assistant"。
 */
typedef struct {
    const char *text;   /*!< UTF-8 正文；不能为 NULL，也不能是空串 */
    bool        is_ai;  /*!< false = 用户说的（role=user）；true = 模型说的（role=assistant） */
} llm_msg_t;

/**
 * @brief 一次请求的结果
 *
 * ⚠️ `text` 是**堆上的指针，所有权归取到它的人** —— 用完必须调
 *    `llm_client_reply_free()`。别只把它赋给别的变量就丢掉。
 */
typedef struct {
    bool  ok;               /*!< true = 拿到回复（text 非空）；false = 看 err */
    int   http_status;      /*!< HTTP 状态码；没走到 HTTP 就是 0 */
    char  err[LLM_ERR_MAX]; /*!< 失败原因（可直接上屏的短句）；ok=true 时是空串 */
    char *text;             /*!< 回复正文（堆，'\0' 结尾）。ok=true 时非 NULL，
                                 ok=false 时必为 NULL */
} llm_reply_t;

/**
 * @brief 初始化：建同步对象和常驻工作任务
 *
 * **非阻塞**（只建任务，不发请求），幂等。由 main 在启动时调一次。
 *
 * @return ESP_OK         就绪（或已经就绪过）
 *         ESP_ERR_NO_MEM 建同步对象 / 建任务失败
 */
esp_err_t llm_client_init(void);

/**
 * @brief 上一次请求是否还没结束
 *
 * 供 App 在 UI 上决定"要不要禁用发送"。**不是**必须调 —— 直接调
 * llm_client_request() 也会在忙时被拒。
 *
 * @return true 忙（有一条请求正在跑，或结果还没被取走）
 */
bool llm_client_busy(void);

/**
 * @brief 发起一次对话（**非阻塞**）
 *
 * 把**整段对话**交给本组件 —— 服务端无状态，每次都得重发（见文件头"多轮上下文"）。
 * 系统提示词由本组件自动加在最前面。
 *
 * @param msgs 按时间顺序排列的消息数组，**最后一条必须是本次的用户提问**
 * @param n    条数（1..LLM_MAX_MSGS）
 *
 * ⚠️ 本函数会**把文本拷进自己的堆缓冲**再入队 —— 返回之后调用方随便改、随便释放
 *    自己那份都行。所以直接把 App 自己的历史数组传进来是安全的，不需要再拷一次。
 * ⚠️ 会**先清掉上一条没人取走的旧结果**（并释放它的正文）—— 否则 App 下次 poll
 *    会先拿到过期回复。
 * ⚠️ **不检查网络是否可用**（本组件不认识 wifi_service）。没联网时请求照样发，
 *    只是结果会是 ok=false、err="网络错误: ..."。
 * ⚠️ 调用方应保证 user/assistant **交替**（本项目的用法天然满足：失败时把用户那句
 *    回滚掉、成功后才追加模型那句）。服务端对相邻同角色消息是宽容的，但没必要去试探。
 *
 * @return ESP_OK                 已发起
 *         ESP_ERR_INVALID_ARG    msgs 为空 / 条数越界 / 某条正文为空
 *         ESP_ERR_INVALID_SIZE   正文总量过大（见 .c 里的 LLM_REQ_MAX）
 *         ESP_ERR_INVALID_STATE  init 没成功过
 *         ESP_ERR_NOT_FINISHED   上一次还在跑（用 llm_client_busy() 也能提前看出）
 *         ESP_ERR_NO_MEM         拷贝历史失败
 */
esp_err_t llm_client_request(const llm_msg_t *msgs, int n);

/**
 * @brief 把"当前已经收到的正文"整份拷出来（**非阻塞**）
 *
 * 流式过程中周期调用（配合 App 的 lv_timer），用来刷新屏幕上那条正在生成的回复。
 *
 * ⚠️ 每次给的都是**全量**而不是增量 —— 调用方不用记账，直接 `lv_label_set_text()`
 *    就行。代价是每调一次要拷一遍当前长度（最多 `LLM_TEXT_MAX`），几 KB 的 memcpy
 *    在 100ms 的节奏下完全可忽略，换来的是调用方零状态、不可能记错账。
 *
 * ⚠️ 它读的是**内部累积缓冲**，流式进行中读到的只是"到目前为止"的内容；等
 *    `llm_client_poll()` 取到结果时，那个 `text` 才是权威的完整正文（两者内容一致，
 *    但 poll 的那份是独立拷贝、可以直接交给历史）。
 *
 * @param[out] out 输出缓冲（至少 cap 字节）
 * @param[in]  cap 容量；正文比它长就截断（结尾一定有 '\0'）
 * @return 写入的字节数（不含结尾 '\0'）；0 = 还没有内容 / 参数不对
 */
size_t llm_client_copy_text(char *out, size_t cap);

/**
 * @brief 取一次结果（**非阻塞**，取走即清）
 *
 * 供 App 的 lv_timer 周期调用（跑在 LVGL 任务里，安全）。
 * 拿到 true 就说明这次流式结束了（成功或失败都算）—— 此后 `llm_client_copy_text()`
 * 给的就是完整正文。
 *
 * @param[out] out 结果。**只在返回 true 时被写入**；成功取到后调用方拥有
 *                 out->text，用完调 llm_client_reply_free()
 * @return true  这次拿到了一条新结果（可能是 ok=false 的失败结果）
 *         false 还没结束，继续等
 */
bool llm_client_poll(llm_reply_t *out);

/**
 * @brief 释放一条结果的正文
 *
 * 幂等，传 NULL 或 text 已经是 NULL 都安全。
 */
void llm_client_reply_free(llm_reply_t *r);

#ifdef __cplusplus
}
#endif
