/*
 * 相册 App —— 看 SD 卡里拍过的照片
 *
 * ⚠️ 定位是**验证台**：只验证 "SD 读文件 -> JPEG 解码 -> 上屏" 这条链路。
 *    所以界面只有一列文件名 + 点开看一张，没有缩略图、翻页、删除。
 *    照片由相机 App 拍到 SD_CARD_PHOTO_DIR（见 sd_card.h）。
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 把相册注册给 app_manager（由 main 调用） */
void photo_register(void);

#ifdef __cplusplus
}
#endif
