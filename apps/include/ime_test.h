/*
 * 拼音输入法验收台 App
 *
 * 它**不是**要给用户用的功能，是一个"验证台"（定位同 app_demo.c 之于
 * app_manager + kv_store）：只为了证明"屏上能打出中文"这一条链路是通的。
 * 联网那条轨道（DeepSeek）以后会自己带一套聊天界面，届时把这里的输入区搬过去即可。
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 把拼音输入法验收台注册给 app_manager */
void ime_test_register(void);

#ifdef __cplusplus
}
#endif
