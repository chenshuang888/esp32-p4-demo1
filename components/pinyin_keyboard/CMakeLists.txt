# 自研中文输入法键盘：键盘 + 候选栏 + 输入法逻辑，都在这个组件里。
#
# 键盘那半是用 lv_buttonmatrix 从零搭的（布局 / 分页 / 中英模式 / 样式全是自己的）。
# 不用 LVGL 自带的 lv_keyboard —— 它的布局表是文件内 static、ctrl_map 连读都读不到。
#
# 依赖：
#   pinyin_engine   拼音串 -> 整句候选（纯 C，零依赖）
#   pinyin_learn    用户学习记录的持久化（选词时记一笔、退出时落盘）
#   lvgl            画键盘与候选栏（托管组件，**不要**写进 REQUIRES，
#                   在 idf_component.yml 里声明，由组件管理器注入）
#
# 不依赖 esp_lvgl_port：本组件不自己加锁，锁由调用方（App 的 enter）持有。
idf_component_register(
    SRCS
        "pinyin_keyboard.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        pinyin_engine
        pinyin_learn
)
