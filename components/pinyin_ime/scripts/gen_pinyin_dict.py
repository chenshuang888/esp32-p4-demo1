#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
生成拼音输入法的词典：

    dicts/lv_pinyin_dict.c      LVGL lv_ime_pinyin 用的 lv_pinyin_dict_t 数组
    source/char_freq.tsv        排序依据的审计文件（字 -> 频次）

用法（在工程根目录跑）：

    python components/pinyin_ime/scripts/gen_pinyin_dict.py

依赖两个 pip 包（脚本会自己检查，缺了会提示怎么装）：

    pypinyin    汉字 -> 拼音（含多音字的全部读音）
    jieba       提供 dict.txt（349046 个词 + 语料频次），用来算"字频"

──────────────────────── 为什么需要这个脚本 ────────────────────────

LVGL 9.5 自带拼音输入法 lv_ime_pinyin，但它内置的词典是**繁体**、且只有约 400 条
（见 managed_components/lvgl__lvgl/src/widgets/ime/lv_ime_pinyin.c 里的
lv_ime_pinyin_def_dict，形如 {"ai", "愛"}、{"bai", "百白敗"}）。

本工程要的是：简体 + 覆盖《通用规范汉字表》全表 + 候选按字频排序。
所以必须自带词典，并在 sdkconfig.defaults 里关掉 LV_IME_PINYIN_USE_DEFAULT_DICT
（留着它只是白占约 12KB 固件，而且万一 set_dict 失败会静默显示繁体）。

──────────────── 词典格式与四条铁律（全部从 LVGL 源码读出）────────────────

    typedef struct { const char * const py; const char * const py_mb; } lv_pinyin_dict_t;

    py      无声调全拼小写音节，如 "yi"
    py_mb   **该音节全部候选汉字的 UTF-8 拼接串，顺序即候选显示顺序**

1) py_mb 里每个字**必须正好 3 字节**。
   依据：lv_ime_pinyin.c 的 pinyin_input_proc() / pinyin_page_proc()：
       cand_num = lv_strlen(py_mb) / 3;              // 硬编码假定每字 3 字节
       lv_pinyin_cand_str[i][j] = cand_str[i*3 + j]; // 按 3 字节切片
   而且候选缓冲每格只有 4 字节（源码里的 lv_pinyin_cand_str[][4]），也只装得下一个汉字。
   ⇒ 本脚本**整字剔除 4 字节的字**（CJK 扩展 B/C/D/E，共 196 个，全在三级字表里）。
     不剔的后果不是"那个字显示不出来"，而是**该音节从它开始往后所有候选全部错位成乱码**。

2) 数组必须按 py **ASCII 升序**排，且同一首字母的条目**连续**。
   依据：init_pinyin_dict() 靠"首字母变化"来建 py_pos[26] / py_num[26]；
   检索（pinyin_search_matching）在该字母的条目区间里线性扫。
   排错 ⇒ 索引错乱、查不到字。某个首字母完全缺失是安全的（py_num=0，那个字母打不出字）。

3) 每条 py 必须以小写字母 a-z 开头。
   依据：pinyin_search_matching() 直接 offset = py_str[0] - 'a' 当数组下标。

4) 数组必须以 { NULL, NULL } 结尾。依据：init_pinyin_dict() 的循环靠它终止。

上面这些断言全部写在下面的 check_* 里，违反就报错退出，绝不会生成半个错误文件。

──────────────────── 字频从哪来、为什么这么算 ────────────────────

候选顺序 = 字频从高到低。频率来源是 jieba 的词表（jieba/dict.txt，349046 行，
格式 "词 频次 词性"）：把一个词的频次按**词内去重后的字**累加到每个字上 ——
例如 "哈哈 5" 只给「哈」记 5 分而不是 10，避免叠字词虚高。

不装 jieba 也能复核排序依据：脚本会同时输出 source/char_freq.tsv，那是排序的唯一输入。

──────────────────────── 拼音与字表来源 ────────────────────────

    拼音    pypinyin，Style.NORMAL（无声调）+ heteronym=True（多音字的全部读音都收，
            所以「行」既在 xing 下也在 hang 下）
    字表    ../font_cjk/chars/guifan_8105.txt
            **刻意复用 font_cjk 的字表，而不是自己另存一份** ——
            因为候选字必须是"字体能渲染的字"的子集，否则选出来就是占位方块；
            共用同一份字表，这个约束天然成立（字体那份就是拿它生成的）。
            该文件是**单行、无分隔符**的 8105 个汉字（24511 字节）。

    平局排序    字频相同时（含大量频次为 0 的生僻字）按字表内的先后顺序，
                即《通用规范汉字表》的 一级->二级->三级、同级按笔画。
                比按 Unicode 码位排更合理（一级 3500 字全部排在二级前面）。
"""

import pathlib
import re
import sys

# ─────────────────────────── 路径 ───────────────────────────

HERE = pathlib.Path(__file__).resolve().parent          # components/pinyin_ime/scripts
COMP = HERE.parent                                       # components/pinyin_ime
ROOT = COMP.parent.parent                                # 工程根

CHARS_FILE = ROOT / "components" / "font_cjk" / "chars" / "guifan_8105.txt"
FREQ_OUT   = COMP / "source" / "char_freq.tsv"
DICT_OUT   = COMP / "dicts" / "lv_pinyin_dict.c"

PY_RE = re.compile(r"^[a-z]+$")          # 铁律 3：只允许小写字母
MAX_CAND_PER_SYLLABLE = 0                # 0 = 不设上限（数据量很小，见 README）
WRAP_CHARS = 24                          # py_mb 每行的汉字数（纯排版，不影响数据）


def die(msg: str) -> None:
    print("❌ " + msg, file=sys.stderr)
    sys.exit(1)


# ─────────────────────── 1. 读字表 ───────────────────────

def load_chars() -> tuple[str, str]:
    """返回 (保留的 3 字节字, 被剔除的 4 字节字)。保持字表原始顺序（= 平局排序依据）。"""
    if not CHARS_FILE.is_file():
        die(f"找不到字表：{CHARS_FILE}")

    text = CHARS_FILE.read_text(encoding="utf-8")
    # 该文件是单行无分隔符的；仍按"去掉所有空白"处理，免得哪天格式变了
    text = "".join(text.split())
    if len(text) != 8105:
        die(f"字表应为 8105 字，实际 {len(text)} —— 先核对 font_cjk/chars/guifan_8105.txt")

    keep, drop, seen = [], [], set()
    for c in text:
        if c in seen:                      # 理论上无重复，防一手
            continue
        seen.add(c)
        if len(c.encode("utf-8")) == 3:
            keep.append(c)
        else:
            drop.append(c)                 # 铁律 1：4 字节字整字剔除
    return "".join(keep), "".join(drop)


# ─────────────────────── 2. 算字频 ───────────────────────

def load_char_freq(chars: str) -> dict[str, int]:
    """遍历 jieba 的 dict.txt 聚合字频。只关心 chars 里的字。"""
    try:
        import jieba
    except ImportError:
        die("缺 jieba：python -m pip install jieba")

    dict_txt = pathlib.Path(jieba.__file__).resolve().parent / "dict.txt"
    if not dict_txt.is_file():
        die(f"jieba 包里找不到词表：{dict_txt}")

    want = set(chars)
    freq: dict[str, int] = {}

    with dict_txt.open(encoding="utf-8") as f:
        for line in f:
            parts = line.split()
            if len(parts) < 2:
                continue                   # 格式为 "词 频次 [词性]"
            word = parts[0]
            try:
                n = int(parts[1])
            except ValueError:
                continue
            # 词内去重：叠字词不重复计分
            for c in set(word):
                if c in want:
                    freq[c] = freq.get(c, 0) + n

    missing = [c for c in chars if c not in freq]
    print(f"   字频覆盖：{len(chars) - len(missing)}/{len(chars)} 字在 jieba 词表里出现过"
          f"（{len(missing)} 字频次为 0，排在各音节末尾，按字表顺序）")
    return freq


# ─────────────────── 3. 建立 音节 -> 候选字 ───────────────────

def build_syllables(chars: str, freq: dict[str, int]) -> dict[str, list[str]]:
    try:
        from pypinyin import pinyin, Style
    except ImportError:
        die("缺 pypinyin：python -m pip install pypinyin")

    order = {c: i for i, c in enumerate(chars)}     # 平局排序依据
    syl: dict[str, list[tuple[int, str]]] = {}       # 音节 -> [(档位, 字)]
    bad_py: list[tuple[str, str]] = []               # 非法读音（跳过，不致命）
    no_py: list[str] = []                            # 一个有效读音都没有的字（致命）
    rare_readings = 0                                # 非主读音的(音节,字)对数量

    for c in chars:
        # heteronym=True：多音字的**全部**读音都要收，否则「行」只能打出一个读音。
        # heteronym=False：只要首选读音 —— 它就是"主读音"，用来分档（见下）。
        readings = pinyin(c, style=Style.NORMAL, heteronym=True)
        primary = pinyin(c, style=Style.NORMAL)[0][0]

        valid = 0
        for r in (readings[0] if readings else []):
            if not PY_RE.match(r):
                # 例如「欸」的一个读音是 'ê'（带声调符的拉丁字母）。
                # 键盘只能产生 a-z，这种读音本来就打不出来，跳过即可 ——
                # 该字在其它读音下照样能被选中。
                bad_py.append((c, r))
                continue

            # ⚠️ 分档是这个词典质量的关键：
            # pypinyin 的 heteronym 会带上**古音/异体等生僻读音**，而候选是按"字的
            # 总字频"排的（总字频由它的**常用**读音决定）。如果不分档，超高频字会靠
            # 生僻读音挤进别的音节的头几页 —— 实测「不、市、还、包」都因为生僻读音
            # 排在 "fu" 的第一页，而 "fu" 真正要的是 复/服/福/富/付。
            # 所以：主读音排 0 档（按字频排），其余读音整体降到 1 档（排到该音节末尾）。
            # 代价：多音字在"次要读音"下要靠后找（如 行 在 "hang" 下不是第一个），
            # 但它仍然能被打出来 —— 好过第一页全是错字。
            tier = 0 if r == primary else 1
            if tier == 1:
                rare_readings += 1
            syl.setdefault(r, []).append((tier, c))
            valid += 1

        if valid == 0:
            no_py.append(c)

    if bad_py:
        detail = "、".join(f"{c}→{r!r}" for c, r in bad_py[:5])
        print(f"   ⚠️ 跳过 {len(bad_py)} 个非纯字母读音（键盘打不出，不影响该字的其它读音）：{detail}")
    if no_py:
        die(f"{len(no_py)} 个字去掉非法读音后一个读音都没有（第一个：{no_py[0]}）—— 先查数据源")
    print(f"   主读音档 {sum(len(v) for v in syl.values()) - rare_readings} 对 / "
          f"多音字次要读音档 {rare_readings} 对（后者排在各音节末尾）")

    # 音节内：先按档位（0 主读音在前），再按字频降序，再按字表顺序
    out: dict[str, list[str]] = {}
    for s, items in syl.items():
        items.sort(key=lambda t: (t[0], -freq.get(t[1], 0), order[t[1]]))
        cand = [c for _, c in items]
        if MAX_CAND_PER_SYLLABLE:
            cand = cand[:MAX_CAND_PER_SYLLABLE]
        out[s] = cand

    # 铁律 2：音节本身按 ASCII 升序（dict 保持插入顺序，所以这里排完即最终顺序）
    return dict(sorted(out.items()))


# ─────────────────────── 4. 四条铁律自查 ───────────────────────

def check(syl: dict[str, list[str]]) -> None:
    names = list(syl.keys())

    # 铁律 2：升序 + 首字母连续
    if names != sorted(names):
        bad = next((names[i] for i in range(1, len(names)) if names[i] < names[i - 1]), "")
        die(f"音节未按 ASCII 升序排（第一个违例：{bad}）—— 违反铁律 2")
    if len(set(names)) != len(names):
        die("有重复音节 —— 违反铁律 2")

    letters = [s[0] for s in names]
    for i in range(1, len(letters)):
        if letters[i] != letters[i - 1] and letters[i] <= letters[i - 1]:
            die(f"首字母没有连续分组：{names[i]} —— 违反铁律 2")

    # 铁律 1 / 3
    for s, cs in syl.items():
        if not PY_RE.match(s):
            die(f"音节 {s!r} 不是纯小写字母 —— 违反铁律 3")
        for c in cs:
            if len(c.encode("utf-8")) != 3:
                die(f"音节 {s} 的候选 {c!r} 不是 3 字节 —— 违反铁律 1（会导致整串候选错位）")

    print(f"   ✅ 自查通过：{len(names)} 个音节、{sum(len(v) for v in syl.values())} 个(音节,字)对、"
          f"首字母 {len(set(letters))}/26 个有候选")


# ─────────────────────── 5. 输出 ───────────────────────

def emit_freq_tsv(chars: str, freq: dict[str, int]) -> None:
    FREQ_OUT.parent.mkdir(parents=True, exist_ok=True)
    with FREQ_OUT.open("w", encoding="utf-8", newline="\n") as f:
        f.write("# pinyin_ime 候选排序的唯一依据（字 <TAB> 频次）。\n")
        f.write("# 生成：python components/pinyin_ime/scripts/gen_pinyin_dict.py\n")
        f.write("# 来源：jieba 的 dict.txt（词 频次 词性），按词内去重后的字累加词频。\n")
        f.write("# 频次为 0 = 该字没在 jieba 词表里出现过（按字表顺序排在各音节末尾）。\n")
        f.write("# 字\t频次\n")
        for c in sorted(chars):            # 按码位排，让 diff 稳定
            f.write(f"{c}\t{freq.get(c, 0)}\n")
    print(f"   写出 {FREQ_OUT.relative_to(ROOT)}（{len(chars)} 行）")


def emit_dict_c(syl: dict[str, list[str]], dropped: str, chars: str) -> None:
    names = sorted(syl)
    pairs = sum(len(v) for v in syl.values())
    letters = sorted({s[0] for s in names})

    out: list[str] = []
    out.append("/*")
    out.append(" * 拼音词典 —— **生成物，永不手改**。")
    out.append(" *")
    out.append(" * 重新生成：")
    out.append(" *     python components/pinyin_ime/scripts/gen_pinyin_dict.py")
    out.append(" *")
    out.append(" * 格式、四条铁律、数据出处与排序算法，全部写在生成脚本顶部的注释里 ——")
    out.append(" * 改数据请改脚本重跑，不要动这个文件（它没有任何人工可维护的信息）。")
    out.append(" *")
    out.append(f" * 统计：{len(names)} 个音节 / {pairs} 个(音节,字)对 / 候选字 {len(chars)} 个")
    out.append(f" *       首字母有候选的：{len(letters)} 个（{' '.join(letters)}）")
    out.append(f" *       已剔除 4 字节字 {len(dropped)} 个（CJK 扩展 B/C/D/E，会破坏候选切片）")
    out.append(" */")
    out.append("")
    out.append('#include "lvgl.h"')
    out.append("")
    out.append("/* 护栏：lv_pinyin_dict_t 只在 LV_USE_IME_PINYIN 打开时才存在（lv_ime_pinyin.h 里被")
    out.append(" * #if LV_USE_IME_PINYIN 包着）。漏开配置时给一句能直接照做的提示，")
    out.append(" * 而不是一串 'unknown type name'。 */")
    out.append("#if !LV_USE_IME_PINYIN")
    out.append('#error "这个词典文件需要 CONFIG_LV_USE_IME_PINYIN=y。它已写在 sdkconfig.defaults'
               ' 里，但 defaults 只对 sdkconfig 中尚未出现的符号生效 —— 该符号已存在，'
               '所以必须删掉 sdkconfig 后重新 build。详见 README『中文输入（拼音输入法）』。"')
    out.append("#endif")
    out.append("")
    out.append("const lv_pinyin_dict_t pinyin_dict_data[] = {")

    last_letter = ""
    for s in names:
        if s[0] != last_letter:
            last_letter = s[0]
            out.append(f"    /* ---------- {last_letter} ---------- */")

        blob = "".join(syl[s])
        # 按每行 WRAP_CHARS 个字折断，用相邻字符串字面量拼接（纯排版，不影响内容）
        chunks = [blob[i:i + WRAP_CHARS] for i in range(0, len(blob), WRAP_CHARS)]
        prefix = f'    {{ "{s}", "'
        if len(chunks) == 1:
            out.append(f'{prefix}{chunks[0]}" }},')
        else:
            out.append(f'{prefix}{chunks[0]}"')
            indent = " " * len(prefix)     # 续行对齐到开引号右边
            for i, ch in enumerate(chunks[1:], 1):
                last = (i == len(chunks) - 1)
                out.append(f'{indent}"{ch}"' + (" }," if last else ","))

    out.append("")
    out.append("    /* 终止哨兵：init_pinyin_dict() 的循环靠它结束（铁律 4） */")
    out.append("    { NULL, NULL },")
    out.append("};")
    out.append("")

    DICT_OUT.parent.mkdir(parents=True, exist_ok=True)
    DICT_OUT.write_text("\n".join(out), encoding="utf-8", newline="\n")
    print(f"   写出 {DICT_OUT.relative_to(ROOT)}（{DICT_OUT.stat().st_size / 1024:.1f} KB 源码）")

    # 写回后自检：每个条目都必须以 "}," 收尾（含终止哨兵）。
    # 这条是补的 —— 第一版忘了给"只有一行"的短条目补右花括号，生成了 242 个语法错误的
    # 条目，而文本看着完全正常（短条目本来就少），肉眼复核很容易漏。
    text = DICT_OUT.read_text(encoding="utf-8")
    n_head = len(re.findall(r'^    \{ "[a-z]+",', text, re.M))
    n_tail = len(re.findall(r"\},\s*$", text, re.M))
    if n_head != len(names) or n_tail != len(names) + 1:   # +1 = 终止哨兵
        die(f"生成物自检失败：条目起始 {n_head}、以 \"}},\" 收尾 {n_tail}，"
            f"应为 {len(names)} 和 {len(names) + 1} —— 有条目缺右花括号")
    print(f"   ✅ 生成物自检通过（{n_head} 个条目 + 1 个终止哨兵，全部以 \"}},\" 收尾）")


# ─────────────────────── main ───────────────────────

def main() -> None:
    print("1/5 读字表 …")
    chars, dropped = load_chars()
    print(f"   字表 {len(chars) + len(dropped)} 字，保留 {len(chars)}，剔除 4 字节字 {len(dropped)}")

    print("2/5 聚合字频（jieba dict.txt，349046 个词）…")
    freq = load_char_freq(chars)

    print("3/5 取拼音、建 音节->候选字 …")
    syl = build_syllables(chars, freq)

    print("4/5 自查四条铁律 …")
    check(syl)

    print("5/5 输出 …")
    emit_freq_tsv(chars, freq)
    emit_dict_c(syl, dropped, chars)

    # 抽样报告：供人眼核对候选顺序是否合理（这是字频排序是否生效的最直接证据）。
    # 刻意挑了 fu / yu / hang / zhang —— 它们是最容易被"生僻读音"污染的音节。
    print("\n抽样（前 9 个候选 = 候选栏第一页）：")
    for s in ("yi", "shi", "fu", "ji", "yu", "zhong", "ni", "hao", "hang", "zhang", "he", "de"):
        if s in syl:
            print(f"   {s:<6} ({len(syl[s]):>3} 个)  {''.join(syl[s][:9])}")

    print("\n✅ 完成")


if __name__ == "__main__":
    main()
