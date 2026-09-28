# 自研中文输入法软键盘：用 lv_buttonmatrix 从零搭出来，布局 / 分页 / 中英模式 /
# 按键语义**全是自己的**（不用 LVGL 自带的 lv_keyboard，因为它的布局表外部改不了）。
#
# 依赖：
#   lvgl   画键盘（托管组件，**不要**写进 REQUIRES，在 idf_component.yml 里声明）
#
# 它**不认识**拼音引擎、也不认识输入框 —— 只把"按了哪一类键"回调出去；
# 至于数字键是直选候选还是插字符，由调用方（pinyin_input）决定。
idf_component_register(
    SRCS
        "pinyin_keyboard.c"
    INCLUDE_DIRS
        "include"
)
