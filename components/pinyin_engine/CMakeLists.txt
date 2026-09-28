# 拼音转换引擎：字母串 -> 若干个完整的汉字候选（整句转换）。
#
# **纯 C，零依赖**：只用 <string.h>/<stdint.h>，不碰 LVGL、不碰 FreeRTOS。
# 词典是编译进 flash 的 const 数据（dicts/ 下，由 scripts/gen_engine_dict.py 生成）。
#
# 所以这里不需要 REQUIRES，也不需要 idf_component.yml
# —— 形态和 components/avi_reader、components/avi_writer 一样。
# esp_log 只在 selftest.c 里用，它是 IDF 对所有组件自动注入的公共依赖，同样不用手写。
#
# ⚠️ dicts/ 要进 INCLUDE_DIRS：生成物 pe_dict.h 在那里，而尺寸常量必须和数组定义
#    在同一个文件里才能被编译期校验（写错长度会直接报错）。
idf_component_register(
    SRCS
        "pinyin_engine.c"
        "selftest.c"
        # 生成物，永不手改。改数据请重跑 scripts/gen_engine_dict.py。
        "dicts/pe_dict_words.c"
        "dicts/pe_dict_syllables.c"
        "dicts/pe_dict_abbrev.c"
    INCLUDE_DIRS
        "include"
        "dicts"
)
