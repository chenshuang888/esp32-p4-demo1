/*
 * AI Chat —— 和大模型对话的 App
 *
 * 链路：屏上输入（自研拼音键盘打中文）-> llm_client 发 HTTPS POST 到 DeepSeek
 *       -> 回复**流式**出现在聊天区（边生成边刷，不是等整段回来才显示）。
 *
 * ⚠️ 这是**测试版**，只做"能对话"这一条闭环，含两件：
 *    1. **多轮上下文**：整段历史由本 App 持有，每次请求全量重发（服务端无状态）。
 *    2. **流式上屏**：在 lv_timer 里按 100ms 的节奏把新收到的正文刷上去。
 *
 * ⚠️ 刻意不做的东西（都砍了）：**"停止生成"**（生成中发送/新对话一律忽略 ——
 *    见 llm_client.h 里"没有取消"那段说明）、Markdown/代码块渲染、
 *    对话历史落盘（**退出 App 即结束本次对话**）、消息时间戳/头像/气泡、
 *    多会话管理、重新生成/编辑。
 *
 * ⚠️ 中文输入法只有一套：components/pinyin_keyboard（键盘 + 候选栏 + 输入法逻辑）。
 *    它的字号**一处都不能设** —— 中文只有 18px 一档，显式设别的字号会变成占位方块
 *    （同 ime_test.c）。
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 把 AI Chat 注册给 app_manager */
void ai_chat_register(void);

#ifdef __cplusplus
}
#endif
