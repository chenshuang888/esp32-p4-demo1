# 拼音输入法**控件**：把键盘接成中文输入法（整句输入 + 简拼 + 用户学习）。
#
# 依赖：
#   pinyin_engine    转换引擎（纯 C，零依赖）—— REQUIRES 里写它
#   pinyin_keyboard  自研键盘（本组件只消费它的"按键语义"事件）
#   pinyin_learn     用户学习的持久化（用户选词时记一笔、App 退出时落盘）
#   lvgl             画候选栏（托管组件，**不要**写进 REQUIRES，
#                    在 idf_component.yml 里声明，由组件管理器注入）
#
# 不依赖 esp_lvgl_port：本组件不自己加锁，锁由调用方（App 的 enter）持有。
idf_component_register(
    SRCS
        "pinyin_input.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        pinyin_engine
        pinyin_keyboard
        pinyin_learn
)
