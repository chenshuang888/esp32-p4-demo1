#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
生成 pinyin_engine 的词典数据：

    dicts/pe_dict.h              数组声明 + 维度常量（生成物里有尺寸，引擎拿它做断言）
    dicts/pe_dict_words.c        词表：音串 -> 候选词（带量化代价），按音串升序
    dicts/pe_dict_syllables.c    单字表：音节 -> 候选字（带量化代价），按音节升序
    dicts/pe_dict_abbrev.c       缩写表：声母序列 -> 词（简拼用），按打包键升序
    source/words.tsv             审计：音串 / 词 / 语料频次 / 量化代价
    source/syllables.tsv         审计：音节 / 字 / 字频 / 量化代价
    source/abbrev.tsv            审计：缩写键 / 组内词数 / 罚项 / 词下标 / 词 / 词代价

用法（在工程根目录跑）：

    python components/pinyin_engine/scripts/gen_engine_dict.py

依赖（两个 pip 包，都已经在用）：
    pypinyin    汉字/词 -> 拼音（**词级**注音，自带多音字消歧）
    jieba       提供 dict.txt（349046 个词 + 语料频次）

────────────────────────── 这个引擎在做什么 ──────────────────────────

把用户连打的一串字母（如 jintiantianqi）转成若干个**完整的**候选句子
（今天天气 / 今天天起 / …），用户点一个就整段上屏。这是手机输入法的模式。

做法是"在字母串上做最短路径"（DP）：
    节点 = 字母串的每个位置；边 = 一段子串，它要么命中词表、要么是一个合法音节
    代价 = 量化后的 (log(总数) - log(频次)) + 每边固定惩罚 GAMMA
    从位置 0 走到 n 的最小代价路径 = 最优转换结果

**音串精确匹配天然解决了分词**：用户打 xianzai，词表里就有 "xianzai" 这个 key，
不需要先判断它是 xian+zai 还是 xi+an+zai。多音字歧义也自动解决 ——
「银行」在 yinhang 和 yinxing 两个 key 下各有一条记录，用户打哪个都能命中。

────────────────────────── 数据从哪来 ──────────────────────────

    词表    jieba 的 dict.txt（词 + 语料频次），筛"纯汉字 + 长度 2~4 + 频次 >= FREQ_MIN"
    注音    pypinyin，对**整个词**注音（不是逐字）—— 它能按词消歧，
            所以「银行」得到 yinhang 而不是 yinxing，0 个词失败
    单字表  pypinyin 对 8105 字取全部读音（heteronym），每音节按字频取前 SYL_KEEP 个
    字频    jieba 词表按字聚合（词内去重后累加词频），与 components/pinyin_ime 同源

**字表复用 components/font_cjk/chars/guifan_8105.txt**，理由和旧的拼音组件一样：
候选字必须是**字体能渲染的字**的子集，否则选出来就是占位方块。
（本引擎不要求 3 字节 —— 候选缓冲是通用的；4 字节的扩展 B 字能显示，但都太生僻，
 排在各音节末尾，进不了前 SYL_KEEP 名。）

──────────────────── 量化代价：为什么不让引擎算 log ────────────────────

引擎里**一次浮点运算都没有**。代价在生成期就算好、量化成 1 个字节：

    cost_unit = round((log(TOTAL) - log(freq)) * COST_SCALE)      # COST_SCALE = 4
    每边再 + GAMMA_UNITS（= GAMMA * COST_SCALE = 16）

单位是 0.25 个自然对数。词频次最小 100、字频次最小 0，量化后最大约 74 —— 1 字节绰绰有余。
好处：引擎里没有 math.h、没有 logf、没有浮点，纯整数加法，又快又省栈。

量化有没有把精度量化掉？脚本最后会**用同一套量化值在 Python 里跑一遍 DP**，
打印 Top-1/Top-3 准确率 —— 和量化前对比就知道。实测无差异。

──────────────────────── 缩写表（简拼）：为什么要单独一张表 ────────────────────────

缩写键 = 每个字的**声母首字母**（zh/ch/sh 取 z/c/s；零声母取音节首字母），
所以「你好世界」-> "nhsj"、「中国」-> "zg"。

它**不重复存词串**，只存"词在 pe_dict_words 里的下标"（uint16）——
这是它只有 ~100KB 而不是 ~700KB 的原因。代价是词表不能超过 65535 条
（头文件里有静态断言挡着）。

键打包成 uint32（4 槽 × 5bit，a-z->1..26，不足补 0，高位在前）：这样
**整数大小关系与字符串字典序完全一致**，C 侧可以直接对 uint32 二分 ——
省掉 strcmp，也省掉"字符串池 + 偏移数组"那一整套。

**为什么需要 penalty**：缩写比全拼歧义得多。实测 5540 个缩写键里，
中位数组只有 1 个词，但最大的 "zz" 有 251 个词（长长/长征/长足…）。所以每组按组内词数罚：
penalty = round(log(组内词数) * COST_SCALE)。这样 "nhsj"（组很小）几乎不罚，
而打 "da" 时全拼的「大」能压过简拼的「答案」。

**一个必须知道的局限**：简拼候选池按**语料词频**排序，而语料词频 != 用户的使用习惯。
实测「你好」在 nh 组里排第 9、「手机」在 sj 组里排第 11、「谢谢」在 xx 组里排第 26 ——
光靠简拼都打不出来。这正是"用户学习"要解决的问题：打全拼选中几次，
该词的代价被压低，它在缩写组里就自动爬上来了。

──────────────────────── 常量：要和 pinyin_engine.c 保持一致 ────────────────────────

    PATH_KEEP = 8    每个位置保留几条最优路径（N-best）。**要比候选数大**：
                     不同路径经常产出**同一个文本**（词边「你好」和字边「你+好」都得到
                     "你好"），末尾去重后会变少。取 4 时实测只剩两三个不重复候选。
    EDGE_KEEP = 4    每个音串/音节取前几个候选作为边
    GAMMA     = 4.0  每增加一条边的固定惩罚（自然对数单位）
这三个是引擎的调参旋钮，**C 侧在 pinyin_engine.c 里**，这里只是为了跑参考实现而重复一份。
好消息：实测 GAMMA 从 2 扫到 12，Top-1 命中都是 27/30 —— 结果对这个常数不敏感。
"""

import collections
import math
import pathlib
import re
import sys

# ─────────────────────────── 路径 ───────────────────────────

HERE = pathlib.Path(__file__).resolve().parent          # components/pinyin_engine/scripts
COMP = HERE.parent                                       # components/pinyin_engine
ROOT = COMP.parent.parent                                # 工程根

CHARS_FILE = ROOT / "components" / "font_cjk" / "chars" / "guifan_8105.txt"
DICT_H     = COMP / "dicts" / "pe_dict.h"
WORDS_C    = COMP / "dicts" / "pe_dict_words.c"
SYLS_C     = COMP / "dicts" / "pe_dict_syllables.c"
ABBR_C     = COMP / "dicts" / "pe_dict_abbrev.c"
WORDS_TSV  = COMP / "source" / "words.tsv"
SYLS_TSV   = COMP / "source" / "syllables.tsv"
ABBR_TSV   = COMP / "source" / "abbrev.tsv"

# ─────────────────────────── 参数 ───────────────────────────

FREQ_MIN   = 100     # 词的语料频次下限（决定词表规模：实测 30808 词 / 数据约 800KB）
MIN_WORD   = 2       # 词长范围（单字走单字表，不进词表）
MAX_WORD   = 4
SYL_KEEP   = 20      # 每个音节保留前几个字（引擎只取前 EDGE_KEEP 个，留余量便于以后调）

COST_SCALE = 4       # 量化单位：每 1 个自然对数量化成 4 个单位
GAMMA      = 4.0     # 每边固定惩罚（自然对数单位）
GAMMA_U    = int(round(GAMMA * COST_SCALE))
PATH_KEEP  = 16      # 和 pinyin_engine.c 的 PE_PATH_KEEP 一致（要比候选数大，理由见文件头）
EDGE_KEEP  = 4       # 和 pinyin_engine.c 的 PE_EDGE_KEEP 一致
ABBREV_KEEP = 16     # 缩写边每个键取前几个词（和 pinyin_engine.c 的 PE_ABBREV_KEEP 一致）
                     # 单独一档是因为缩写组可以很大（实测最大 248 个词），而词边/字边很小

# 缩写键打包成几个 5 bit 槽。**必须和 pinyin_engine.c 的 PE_ABBR_SLOTS 一致**。
# 它固定等于词表最大词长（4），**不是**"数据里实际出现的最长键" —— 后者可能更短，
# 拿它当槽数会让两边的打包结果错位（简拼全部失灵，而且不报错）。
ABBR_SLOTS = MAX_WORD

PY_RE = re.compile(r"^[a-z]+$")

# 参考实现用的测试集：(输入, 期望的 Top-1)。和 selftest.c 的表保持一致。
TESTS = [
    ("nihao", "你好"), ("xiexie", "谢谢"), ("zaijian", "再见"), ("beijing", "北京"),
    ("yinhang", "银行"), ("xianzai", "现在"), ("jintiantianqi", "今天天气"),
    ("women", "我们"), ("womenshi", "我们是"), ("nizenmeyang", "你怎么样"),
    ("woaini", "我爱你"), ("qingwen", "请问"), ("zhongguo", "中国"),
    ("mingtian", "明天"), ("shenme", "什么"), ("zenme", "怎么"), ("keyi", "可以"),
    ("xuexi", "学习"), ("pengyou", "朋友"), ("shijian", "时间"), ("gongzuo", "工作"),
    ("shouji", "手机"), ("diannao", "电脑"), ("yinyue", "音乐"), ("tushuguan", "图书馆"),
    ("duoshaoqian", "多少钱"), ("shangban", "上班"), ("xiaban", "下班"),
    ("chifan", "吃饭"), ("zhongguoren", "中国人"),
]

# Top-1 门槛：低于这个数就视为回退（改了数据/算法之后拿它当护栏）
MIN_TOP1 = 27

# 简拼（首字母缩写）用例。**单独一张表**，不和上面的全拼混在一起 ——
# 全拼那 30 例是既有基线，混进简拼会看不出"缩写边有没有把全拼搞坏"。
TESTS_ABBR = [
    # 这些词的缩写组里有它、且它按词代价排在前列，简拼应当直接命中
    ("bj", "北京"), ("zg", "中国"), ("sm", "什么"), ("wm", "我们"),
    ("gz", "工作"), ("jt", "今天"), ("py", "朋友"), ("xz", "现在"),
    ("ky", "可以"),
    # 「世界」和「时间」的量化代价**都是 27**（打平），按 flat 的音串序
    # `shijian < shijie` 让「时间」排在前面。这是合法的平局，不是排序 bug。
    ("sj", "时间"),
    # 4 字母简拼：这两个词都在词表里、且缩写组里只有它一个
    ("bjdx", "北京大学"), ("qhdx", "清华大学"),
    # ⚠️ 刻意**不**放这里的例子，以及为什么：
    #   nh -> 你好（组内第 9，组里 32 个词）
    #   sj -> 手机（组内第 11，组里 165 个词）
    #   xx -> 谢谢（组内第 26，组里 110 个词）
    #     它们正是"用户学习"要解决的问题：全拼打几次选中之后，词代价被压低就能爬上来。
    #   nhsj -> 你好世界
    #     「你好世界」**根本不在词表里**（jieba 里没有这条短语）。引擎只能靠
    #     「nh」+「sj」两个缩写边组合出来，而那个组合会输给一个更便宜的搭配
    #     （"n" 是合法音节「嗯」+ "hsj" 是「红四军」这类罕见词），于是结果不是它。
    #     这不是算法问题，是"词表里没有这条短语"—— 同 README 已知限制里那条
    #     "词表是新闻语料频次，口语词偏少"。
]

# 简拼 Top-1 门槛（实测值定标；它衡量的是"当前词表 + 纯语料词频"下简拼能到哪 ——
# 这也是"用户学习"能带来多少改善的基线）
MIN_TOP1_ABBR = 12


def die(msg: str) -> None:
    print("❌ " + msg, file=sys.stderr)
    sys.exit(1)


# ─────────────────────── 1. 字表与字频 ───────────────────────

def load_chars() -> str:
    """返回字表里的 8105 个字，保持原始顺序（同频时的排序依据）。"""
    if not CHARS_FILE.is_file():
        die(f"找不到字表：{CHARS_FILE}")
    # 该文件是单行无分隔符的；仍按"去掉所有空白"处理，免得哪天格式变了
    text = "".join(CHARS_FILE.read_text(encoding="utf-8").split())
    if len(text) != 8105:
        die(f"字表应为 8105 字，实际 {len(text)}")
    return "".join(dict.fromkeys(text))     # 去重但保序


def load_char_freq(chars: str) -> dict:
    """遍历 jieba 的 dict.txt 聚合字频（词内去重，避免叠字词虚高）。"""
    try:
        import jieba
    except ImportError:
        die("缺 jieba：python -m pip install jieba")
    dict_txt = pathlib.Path(jieba.__file__).resolve().parent / "dict.txt"
    if not dict_txt.is_file():
        die(f"jieba 包里找不到词表：{dict_txt}")

    want = set(chars)
    freq: dict = {}
    with dict_txt.open(encoding="utf-8") as f:
        for line in f:
            parts = line.split()
            if len(parts) < 2:
                continue
            try:
                n = int(parts[1])
            except ValueError:
                continue
            for c in set(parts[0]):
                if c in want:
                    freq[c] = freq.get(c, 0) + n
    n_zero = sum(1 for c in chars if c not in freq)
    print(f"   字频：{len(chars) - n_zero}/{len(chars)} 字在 jieba 词表里出现过"
          f"（{n_zero} 字频次为 0，排在各音节末尾）")
    return freq


# ─────────────────────── 2. 词表 ───────────────────────

def load_words(chars: str) -> list:
    """返回按频次降序的 [(词, 频次)]。"""
    try:
        import jieba
    except ImportError:
        die("缺 jieba：python -m pip install jieba")
    dict_txt = pathlib.Path(jieba.__file__).resolve().parent / "dict.txt"

    cs = set(chars)
    out = []
    with dict_txt.open(encoding="utf-8") as f:
        for line in f:
            parts = line.split()
            if len(parts) < 2:
                continue
            try:
                freq = int(parts[1])
            except ValueError:
                continue
            w = parts[0]
            # 纯汉字（含 4 字节字也行）、长度在范围内、频次够
            if not (MIN_WORD <= len(w) <= MAX_WORD):
                continue
            if freq < FREQ_MIN:
                continue
            if not all(c in cs for c in w):
                continue
            out.append((w, freq))
    out.sort(key=lambda x: -x[1])
    return out


def build_word_index(words: list) -> dict:
    """音串 -> [(词, 频次)]，每组按频次降序。用 pypinyin 的**词级**注音。"""
    from pypinyin import pinyin, Style

    idx = collections.defaultdict(list)
    bad = 0
    for w, freq in words:
        syl = [x[0] for x in pinyin(w, style=Style.NORMAL)]
        if not all(PY_RE.match(s) for s in syl):
            bad += 1                      # 例如带 'ê' 的读音，键盘打不出来
            continue
        idx["".join(syl)].append((w, freq))

    if bad:
        print(f"   ⚠️ 跳过 {bad} 个词（注音里有非 a-z 读音）")
    for k in idx:
        idx[k].sort(key=lambda x: -x[1])
    print(f"   词表：{sum(len(v) for v in idx.values())} 词 / {len(idx)} 个音串"
          f"（最长音串 {max(len(k) for k in idx)} 字母）")
    return idx


# ─────────────────────── 3. 单字表 ───────────────────────

def build_syllable_table(chars: str, cfreq: dict) -> dict:
    """音节 -> [字]（按 主读音优先、再按字频降序）。

    ⚠️ "主读音优先"这一档是必须的：pypinyin 的 heteronym 会带上**古音/异体等生僻读音**，
    而排序用的是字的总字频（由它的**常用**读音决定）。不分档的话，超高频字会靠一个
    生僻读音挤进别的音节最前面 —— 实测「不、市、还、包」全排在 "fu" 的前几名，
    而这个音节真正要的是 复/服/福/富/付。（同 components/pinyin_ime 的做法与结论。）
    """
    from pypinyin import pinyin, Style

    order = {c: i for i, c in enumerate(chars)}
    syl = collections.defaultdict(list)     # 音节 -> [(档, 字)]
    bad = 0

    for c in chars:
        # ⚠️ 这里是 [0] 而不是 [x[0] for x in ...]：
        # pypinyin 对一个字返回 [[读音1, 读音2, ...]] —— 一整层是"这个字的全部读音"。
        # 写成 [x[0] for x in ...] 只会拿到**第一个**读音，多音字的次要读音全丢，
        # 结果是"谁"打不出 shei、"这"打不出 zhei、"得"打不出 dei（实测少了 13 个音节）。
        readings = pinyin(c, style=Style.NORMAL, heteronym=True)[0]
        primary = pinyin(c, style=Style.NORMAL)[0][0]
        valid = 0
        for r in readings:
            if not PY_RE.match(r):
                bad += 1
                continue
            syl[r].append((0 if r == primary else 1, c))
            valid += 1
        if valid == 0:
            die(f"字 {c} 去掉非法读音后一个读音都没有 —— 先查数据源")

    if bad:
        print(f"   ⚠️ 跳过 {bad} 个非纯字母读音（键盘打不出，不影响该字的其它读音）")

    out = {}
    for s, items in syl.items():
        items.sort(key=lambda t: (t[0], -cfreq.get(t[1], 0), order[t[1]]))
        out[s] = [c for _, c in items[:SYL_KEEP]]
    print(f"   单字表：{len(out)} 个音节，每音节最多 {SYL_KEEP} 字")
    return dict(sorted(out.items()))


# ─────────────────────── 4. 量化 ───────────────────────

def quantize_cost(total: float, freq: float) -> int:
    """(log(total) - log(freq)) * COST_SCALE，夹到 0..255。"""
    v = int(round((math.log(total) - math.log(freq)) * COST_SCALE))
    return max(0, min(255, v))


def build_tables():
    chars = load_chars()
    cfreq = load_char_freq(chars)
    words = load_words(chars)
    widx = build_word_index(words)
    syls = build_syllable_table(chars, cfreq)

    total_w = sum(f for v in widx.values() for _, f in v)
    total_c = float(sum(cfreq.values()))

    # 词表：按音串升序展开成扁平记录（引擎二分查找 + 同音串连续扫）
    flat = []
    for key in sorted(widx):
        for w, freq in widx[key]:
            flat.append((key, w, quantize_cost(total_w, freq), freq))
    # 单字表：每音节的字与代价
    syl_rows = []
    for s in sorted(syls):
        rows = [(c, quantize_cost(total_c, cfreq.get(c, 0) + 1), cfreq.get(c, 0))
                for c in syls[s]]
        syl_rows.append((s, rows))

    print(f"   量化：TOTAL_W={total_w:.3e}（log={math.log(total_w):.1f}） "
          f"TOTAL_C={total_c:.3e}（log={math.log(total_c):.1f}） GAMMA={GAMMA}->{GAMMA_U} 单位")
    cw = [r[2] for r in flat]
    cc = [r[1] for _, rows in syl_rows for r in rows]
    print(f"   代价范围：词 {min(cw)}..{max(cw)}，字 {min(cc)}..{max(cc)}（1 字节装得下）")
    return flat, syl_rows, widx, max(len(k) for k in widx), max(len(s) for s in syls)


# ─────────────────── 5. 缩写表（简拼 / 首字母缩写） ───────────────────

def pack_abbr(s: str) -> int:
    """把缩写串打包成 uint32：4 个 5-bit 槽，a-z -> 1..26，不足 4 位补 0，高位在前。

    这样打包出来的**整数大小关系与字符串字典序完全一致**（都是从最高位那个槽开始比），
    于是 C 侧可以直接对 uint32 二分 —— 省掉 strcmp，也省掉"字符串池 + 偏移数组"那套。

    ⚠️ 必须和 pinyin_engine.c 的 abbr_pack() 逐位一致。两边不一致不会报错，
    只是简拼全部失灵；C 侧 selftest 的简拼用例是守着这条的。
    """
    v = 0
    for i in range(ABBR_SLOTS):
        v <<= 5
        if i < len(s):
            v |= (ord(s[i]) - 96)       # 'a' -> 1（0 留给"没有这个槽"）
    return v


def word_initials(word: str, py: str) -> str:
    """一个词的缩写 = 每个字的**声母首字母**（zh/ch/sh 取 z/c/s）；零声母取音节首字母。

    ⚠️ 必须用**词级**注音算（`pinyin(词)` 会做多音字消歧），不能逐字算 ——
    否则「银行」按单字「行」的主读音 xing 会得到 "yx"，而它真实的音串是 yinhang（应为 "yh"）。
    顺便拿 NORMAL 的结果和已入库的音串对一遍：对不上就说明两次注音不一致，直接报错。

    @return 缩写串；返回 "" 表示这个词有打不出来的声母，调用方应整词跳过。
    """
    from pypinyin import pinyin, Style

    norms = [x[0] for x in pinyin(word, style=Style.NORMAL)]
    inis = [x[0] for x in pinyin(word, style=Style.INITIALS)]
    if "".join(norms) != py:
        die(f"词 {word} 的注音 {''.join(norms)!r} 与已入库的音串 {py!r} 不一致")

    out = []
    for ini, nrm in zip(inis, norms):
        ch = ini[0] if ini else (nrm[0] if nrm else "")
        if not ("a" <= ch <= "z"):
            return ""
        out.append(ch)
    return "".join(out)


def build_abbrev_index(flat: list) -> dict:
    """建缩写表（简拼索引）。

    输入是**已经展开、已按音串排好序**的词表 flat —— 因为 entries 里存的是
    "词在 pe_dict_words 里的下标"，那个下标只有 flat 定下来之后才存在。

    体积为什么能压到 ~106KB：entries 只存**词表下标**（uint16），不重复存词串。
    （如果像词表那样存一遍词串，这张表会涨到 ~700KB。）

    penalty 按**组内词数**算：歧义越大罚得越多。这样 "nhsj"（组很小）几乎不罚，
    而 "zz"（251 个词）罚得多 —— 保证打 "da" 时全拼的「大」压过简拼的「答案」。
    """
    groups = collections.defaultdict(list)
    skipped = 0
    for i, (py, w, cost, _f) in enumerate(flat):
        abbr = word_initials(w, py)
        if not abbr:
            skipped += 1                # 有打不出来的声母：整个词不进缩写表
            continue
        groups[abbr].append((i, w, cost))

    if skipped:
        print(f"   ⚠️ 跳过 {skipped} 个词（有打不出来的声母，进不了简拼）")

    rows = []                           # [(packed, abbr, [(词下标, 词, 代价)], penalty)]
    entries = []
    for abbr in sorted(groups, key=pack_abbr):
        # 组内按词代价升序（= 词频降序）；代价相同时靠 Python 的稳定排序保持 flat 顺序 → 幂等
        items = sorted(groups[abbr], key=lambda t: t[2])
        penalty = max(0, min(255, int(round(math.log(len(items)) * COST_SCALE))))
        rows.append((pack_abbr(abbr), abbr, items, penalty))
        entries.extend(idx for idx, _w, _c in items)

    min_len = min(len(r[1]) for r in rows)
    max_len = max(len(r[1]) for r in rows)
    biggest = max(len(r[2]) for r in rows)
    if max_len > ABBR_SLOTS:
        die(f"缩写键最长 {max_len} 字母，超过打包槽数 ABBR_SLOTS={ABBR_SLOTS} —— "
            f"要么调大 ABBR_SLOTS（并同步 pinyin_engine.c），要么限制词长")
    assert len(entries) == sum(len(r[2]) for r in rows)
    print(f"   缩写表：{len(rows)} 个缩写键 / {len(entries)} 条词记录"
          f"（键长 {min_len}..{max_len} 字母，最大组 {biggest} 个词）")
    return {"rows": rows, "entries": entries, "min_len": min_len, "max_len": max_len}


# ─────────────────── 6. 参考实现：用同一套量化跑 DP ───────────────────

def reference_convert(py: str, flat: list, syl_rows: list, abbr: dict,
                      max_key: int, max_syl: int, topn: int = 3) -> list:
    """和 pinyin_engine.c 的 DP 逐行对应（整数代价、PATH_KEEP/EDGE_KEEP/GAMMA_U 一致）。

    存在的意义：**证明量化没把精度量化掉**。跑出来的准确率就是 C 侧 selftest 的预期值。
    """
    # 预建查找表（C 侧是二分查找，这里用 dict 等价）
    by_key = collections.defaultdict(list)
    for key, w, cost, _f in flat:
        by_key[key].append((w, cost))
    by_syl = {s: rows for s, rows in syl_rows}
    # 缩写：打包键 -> (penalty, [(词下标, 词, 代价)])。C 侧是 uint32 整数二分。
    by_abbr = {r[0]: (r[3], r[2]) for r in abbr["rows"]}
    abbr_min, abbr_max = abbr["min_len"], abbr["max_len"]

    n = len(py)
    paths = [[] for _ in range(n + 1)]
    paths[0] = [(0, "")]                      # (代价, 文本)
    for i in range(n):
        if not paths[i]:
            continue
        for j in range(i + 1, min(i + max_key, n) + 1):
            seg = py[i:j]
            edges = []
            if seg in by_key:
                edges += [(c + GAMMA_U, w) for w, c in by_key[seg][:EDGE_KEEP]]
            if len(seg) <= max_syl and seg in by_syl:
                edges += [(c + GAMMA_U, ch) for ch, c, _f in by_syl[seg][:EDGE_KEEP]]
            if abbr_min <= len(seg) <= abbr_max:
                hit = by_abbr.get(pack_abbr(seg))
                if hit:
                    pen, items = hit
                    edges += [(c + pen + GAMMA_U, w) for _i, w, c in items[:ABBREV_KEEP]]
            for ecost, etxt in edges:
                for pcost, ptxt in paths[i][:PATH_KEEP]:
                    paths[j].append((pcost + ecost, ptxt + etxt))
            paths[j] = sorted(paths[j], key=lambda x: x[0])[:PATH_KEEP]
    # 末尾**去重**：不同路径产出同一文本是常态（词边「你好」vs 字边「你+好」），
    # 不去重候选栏会出现两个一模一样的候选。
    res, seen = [], set()
    for _c, t in sorted(paths[n], key=lambda x: x[0]):
        if t in seen:
            continue
        seen.add(t)
        res.append(t)
        if len(res) >= topn:
            break
    return res


def report_accuracy(flat, syl_rows, abbr, max_key, max_syl) -> None:
    print("\n参考实现（用同一套量化代价跑 DP）：")

    def run(cases, label):
        hit1 = hit3 = 0
        misses = []
        for inp, want in cases:
            r = reference_convert(inp, flat, syl_rows, abbr, max_key, max_syl)
            if r and r[0] == want:
                hit1 += 1
                hit3 += 1
            elif want in r:
                hit3 += 1
                misses.append((inp, want, r))
            else:
                misses.append((inp, want, r))
        print(f"   {label}：Top-1 {hit1}/{len(cases)}   Top-3 {hit3}/{len(cases)}")
        for inp, want, got in misses:
            print(f"      ⚠️ {inp}: 期望 {want}，实际 {'/'.join(got) or '(无候选)'}")
        return hit1

    hit1 = run(TESTS, "全拼")
    if hit1 < MIN_TOP1:
        die(f"全拼 Top-1 只有 {hit1}/{len(TESTS)}，低于门槛 {MIN_TOP1} —— "
            f"数据/量化出问题了（或新加的缩写边把全拼搞坏了）")

    hit1a = run(TESTS_ABBR, "简拼")
    if hit1a < MIN_TOP1_ABBR:
        die(f"简拼 Top-1 只有 {hit1a}/{len(TESTS_ABBR)}，低于门槛 {MIN_TOP1_ABBR}")


# ─────────────────────── 6. 输出 ───────────────────────

HEADER = """/*
 * {name} —— **生成物，永不手改**。
 *
 * 重新生成：
 *     python components/pinyin_engine/scripts/gen_engine_dict.py
 *
 * 数据来源、量化代价的算法、DP 的做法，全部写在生成脚本顶部的注释里。
 * 这份文件里没有任何人工可维护的信息。
 *
 * {stats}
 */
#include "pe_dict.h"   /* 维度常量（PE_WORD_COUNT 等）与数组声明都在那里；它自己会带上 pinyin_engine.h */

"""


def emit_dict_h(max_key, max_syl, n_words, n_syl, abbr) -> None:
    DICT_H.parent.mkdir(parents=True, exist_ok=True)
    DICT_H.write_text(f"""/*
 * pinyin_engine 词典的**维度与声明** —— 生成物，永不手改。
 *
 * 重新生成：python components/pinyin_engine/scripts/gen_engine_dict.py
 *
 * 为什么要单独一个头：数组在别的 .c 里定义，而引擎需要"元素个数"来二分查找。
 * 在这里把长度写进数组类型（`pe_dict_words[N]`），引擎就能用
 * `sizeof(a)/sizeof(a[0])` 拿到个数，而且**改数据后忘了改这里的长度会直接编译报错**
 * （数组声明与定义尺寸不一致）—— 比运行时才发现好得多。
 */
#pragma once

#include "pinyin_engine.h"

#define PE_MAX_KEY_LEN       {max_key}   /* 最长音串的字母数：DP 里 j-i 的上限 */
#define PE_MAX_SYL_LEN       {max_syl}   /* 最长音节的字母数：只有不超过它才试"字边" */
#define PE_WORD_COUNT        {n_words}
#define PE_SYLLABLE_COUNT    {n_syl}
#define PE_MAX_CHARS_PER_SYL {SYL_KEEP}
#define PE_ABBREV_COUNT        {len(abbr["rows"])}      /* 缩写键（简拼组）个数 */
#define PE_ABBREV_ENTRY_COUNT  {len(abbr["entries"])}   /* 缩写表里词记录总条数 */
#define PE_MIN_ABBR_LEN      {abbr["min_len"]}   /* 缩写键长度下限：DP 里只有 >= 它才查 */
#define PE_MAX_ABBR_LEN      {abbr["max_len"]}   /* 缩写键长度上限：DP 里只有 <= 它才查 */

extern const pe_word_t     pe_dict_words[PE_WORD_COUNT];
extern const pe_syllable_t pe_dict_syllables[PE_SYLLABLE_COUNT];
extern const pe_abbr_t     pe_dict_abbrev[PE_ABBREV_COUNT];
/* 缩写表只存"词在 pe_dict_words 里的下标"，不重复存词串 —— 这是它只有 ~100KB
 * 而不是 ~700KB 的原因。代价是下标用 uint16，所以词表不能超过 65535 条： */
#if defined(__cplusplus)
static_assert(PE_WORD_COUNT <= 65535, "缩写表用 uint16 存词下标，词表超过 65535 条会静默截断");
#else
_Static_assert(PE_WORD_COUNT <= 65535, "缩写表用 uint16 存词下标，词表超过 65535 条会静默截断");
#endif
extern const uint16_t      pe_dict_abbrev_entries[PE_ABBREV_ENTRY_COUNT];
""", encoding="utf-8", newline="\n")
    print(f"   写出 {DICT_H.relative_to(ROOT)}")


def emit_words_c(flat, max_key) -> None:
    lines = [HEADER.format(
        name="词表：音串 -> 候选词",
        stats=(f"统计：{len(flat)} 条记录（音串升序，同音串内按词频降序）；"
               f"最长音串 {max_key} 字母"),
    )]
    lines.append("const pe_word_t pe_dict_words[PE_WORD_COUNT] = {")
    last = None
    for key, w, cost, _f in flat:
        if key != last:
            lines.append(f"    /* ---- {key} ---- */")
            last = key
        lines.append(f'    {{ "{key}", "{w}", {cost} }},')
    lines.append("};")
    lines.append("")
    WORDS_C.parent.mkdir(parents=True, exist_ok=True)
    WORDS_C.write_text("\n".join(lines), encoding="utf-8", newline="\n")
    print(f"   写出 {WORDS_C.relative_to(ROOT)}"
          f"（{WORDS_C.stat().st_size / 1024:.0f} KB 源码）")


def emit_syllables_c(syl_rows) -> None:
    out = [HEADER.format(
        name="单字表：音节 -> 候选字",
        stats=f"统计：{len(syl_rows)} 个音节，每音节最多 {SYL_KEEP} 字（按主读音、字频降序）",
    )]
    for s, rows in syl_rows:
        blob = "".join(c for c, _cost, _f in rows)
        costs = ", ".join(str(cost) for _c, cost, _f in rows)
        out.append(f'static const uint8_t S_COST_{s}[] = {{ {costs} }};')
        out.append(f'static const char    S_CHARS_{s}[] = "{blob}";')
    out.append("")
    out.append("const pe_syllable_t pe_dict_syllables[PE_SYLLABLE_COUNT] = {")
    for s, rows in syl_rows:
        out.append(f'    {{ "{s}", S_CHARS_{s}, S_COST_{s}, {len(rows)} }},')
    out.append("};")
    out.append("")
    SYLS_C.parent.mkdir(parents=True, exist_ok=True)
    SYLS_C.write_text("\n".join(out), encoding="utf-8", newline="\n")
    print(f"   写出 {SYLS_C.relative_to(ROOT)}"
          f"（{SYLS_C.stat().st_size / 1024:.0f} KB 源码）")


def emit_abbrev_c(abbr) -> None:
    rows, entries = abbr["rows"], abbr["entries"]
    out = [HEADER.format(
        name="缩写表：声母序列 -> 词（简拼 / 首字母缩写）",
        stats=(f"统计：{len(rows)} 个缩写键 / {len(entries)} 条词记录"
               f"（键升序，组内按词代价升序）；键长 {abbr['min_len']}..{abbr['max_len']} 字母"),
    )]
    # entries：所有组的词下标顺序拼接。组与组的边界由 pe_dict_abbrev[].first/.count 描述。
    out.append("const uint16_t pe_dict_abbrev_entries[PE_ABBREV_ENTRY_COUNT] = {")
    for i in range(0, len(entries), 20):
        out.append("    " + ", ".join(str(v) for v in entries[i:i + 20]) + ",")
    out.append("};")
    out.append("")
    out.append("const pe_abbr_t pe_dict_abbrev[PE_ABBREV_COUNT] = {")
    first = 0
    for packed, s, items, penalty in rows:
        out.append(f"    {{ 0x{packed:08X}u, {first}, {len(items)}, {penalty} }},"
                   f"  /* {s} */")
        first += len(items)
    if first != len(entries):
        die(f"缩写表内部不一致：组内词数合计 {first}，entries 实有 {len(entries)}")
    out.append("};")
    out.append("")
    ABBR_C.parent.mkdir(parents=True, exist_ok=True)
    ABBR_C.write_text("\n".join(out), encoding="utf-8", newline="\n")
    print(f"   写出 {ABBR_C.relative_to(ROOT)}"
          f"（{ABBR_C.stat().st_size / 1024:.0f} KB 源码）")


def emit_audit(flat, syl_rows, abbr) -> None:
    WORDS_TSV.parent.mkdir(parents=True, exist_ok=True)
    with WORDS_TSV.open("w", encoding="utf-8", newline="\n") as f:
        f.write("# 音串\t词\t语料频次\t量化代价\n")
        f.write("# 生成：python components/pinyin_engine/scripts/gen_engine_dict.py\n")
        f.write("# 来源：jieba dict.txt 频次 + pypinyin 词级注音\n")
        for key, w, cost, freq in flat:
            f.write(f"{key}\t{w}\t{freq}\t{cost}\n")
    with SYLS_TSV.open("w", encoding="utf-8", newline="\n") as f:
        f.write("# 音节\t字\t字频\t量化代价\n")
        f.write("# 字频 = jieba 词表按字聚合（词内去重）；排序 = 主读音优先、再按字频降序\n")
        for s, rows in syl_rows:
            for c, cost, freq in rows:
                f.write(f"{s}\t{c}\t{freq}\t{cost}\n")
    with ABBR_TSV.open("w", encoding="utf-8", newline="\n") as f:
        f.write("# 缩写键\t组内词数\t罚项\t词表下标\t词\t词代价\n")
        f.write("# 缩写 = 每字声母首字母（zh/ch/sh 取 z/c/s；零声母取音节首字母）\n")
        f.write("# 罚项 = round(log(组内词数) * COST_SCALE)，越大越歧义；组内按词代价升序\n")
        for _packed, s, items, penalty in abbr["rows"]:
            for idx, w, cost in items:
                f.write(f"{s}\t{len(items)}\t{penalty}\t{idx}\t{w}\t{cost}\n")
    print(f"   写出 {WORDS_TSV.relative_to(ROOT)} / {SYLS_TSV.relative_to(ROOT)} /"
          f" {ABBR_TSV.relative_to(ROOT)}（审计用）")


# ─────────────────────── 7. 四条铁律自查 ───────────────────────

def check(flat, syl_rows, abbr, max_key, max_syl) -> None:
    # 词表：音串必须严格升序（二分查找的前提），且同一音串连续
    keys = [k for k, _w, _c, _f in flat]
    if keys != sorted(keys):
        bad = next((keys[i] for i in range(1, len(keys)) if keys[i] < keys[i - 1]), "")
        die(f"词表音串未按 ASCII 升序（第一个违例：{bad}）—— 二分查找会失效")
    if len(set(keys)) != len(set(keys)) or not keys:
        die("词表为空")
    for k in keys:
        if not PY_RE.match(k):
            die(f"音串 {k!r} 不是纯小写字母")

    # 单字表：音节升序、每字 3/4 字节、cost 个数与字数一致
    names = [s for s, _ in syl_rows]
    if names != sorted(names):
        die("单字表音节未按 ASCII 升序")
    for s, rows in syl_rows:
        if not rows:
            die(f"音节 {s} 一个候选字都没有")
        if len(rows) > SYL_KEEP:
            die(f"音节 {s} 有 {len(rows)} 个字，超过 SYL_KEEP={SYL_KEEP}")
        for c, cost, _f in rows:
            if len(c.encode("utf-8")) not in (3, 4):
                die(f"音节 {s} 的候选 {c!r} 不是 3/4 字节 UTF-8")
            if not (0 <= cost <= 255):
                die(f"音节 {s} 的字 {c} 代价 {cost} 超出一字节")
    for _k, _w, cost, _f in flat:
        if not (0 <= cost <= 255):
            die(f"词代价 {cost} 超出一字节")

    # 缩写表（简拼）：打包后的键必须**严格升序** —— C 侧是对 uint32 做整数二分，
    # 顺序错了不会报错，只是简拼全部失灵。
    akeys = [r[0] for r in abbr["rows"]]
    if len(set(akeys)) != len(akeys):
        die("缩写表键有重复")
    if akeys != sorted(akeys):
        die("缩写表键未严格升序（打包后的 uint32 顺序）—— 二分查找会失效")
    if akeys and akeys[0] == 0:
        die("缩写表出现 key=0 —— a-z 应映射到 1..26（0 是『这个槽没有字符』的标记）")

    n_words = len(flat)
    off = 0
    for packed, s, items, penalty in abbr["rows"]:
        if not items:
            die(f"缩写键 {s!r} 一个词都没有")
        if len(items) > 255:
            die(f"缩写键 {s!r} 有 {len(items)} 个词，count 字段（uint8）装不下")
        if not (1 <= len(s) <= 4) or not PY_RE.match(s):
            die(f"缩写键 {s!r} 不是 1~4 个纯小写字母")
        if pack_abbr(s) != packed:
            die(f"缩写键 {s!r} 的打包值 {packed} 与 pack_abbr 结果不符")
        expect = max(0, min(255, int(round(math.log(len(items)) * COST_SCALE))))
        if penalty != expect:
            die(f"缩写键 {s!r} 的罚项 {penalty} 与公式算出的 {expect} 不符")
        for idx, w, _c in items:
            if not (0 <= idx < n_words):
                die(f"缩写键 {s!r} 的词下标 {idx} 越界（词表只有 {n_words} 条）")
            if len(w) != len(s):
                die(f"缩写键 {s!r}（{len(s)} 字母）配上了 {len(w)} 个字的词 {w!r}")
        off += len(items)
    if off != len(abbr["entries"]):
        die(f"缩写表 entries 条数不一致：组内合计 {off}，entries {len(abbr['entries'])}")

    print(f"   ✅ 自查通过：词表 {len(flat)} 条 / 单字表 {len(syl_rows)} 音节 /"
          f" 缩写表 {len(abbr['rows'])} 键 {len(abbr['entries'])} 条 /"
          f" 最长音串 {max_key} 字母 / 最长音节 {max_syl} 字母")


def check_emitted(n_words: int, n_syl: int, abbr) -> None:
    """写回后自检：文件里的记录数必须和头文件里声明的维度一致。

    这条是补的 —— 生成器和生成物是两套代码，写错一行（漏个小括号、循环少一轮）
    文本看着完全正常，要到编译或运行时才炸。这里数一遍最省事。

    ⚠️ 也检查 include：生成物用的是 pe_dict.h 里的维度常量，**只引 pinyin_engine.h 会
    编译报 "PE_WORD_COUNT undeclared"**。这条是实际踩过的 —— 第一版自检只数了记录数，
    没查 include，于是漏过了这个错误直到真机构建。
    """
    h = DICT_H.read_text(encoding="utf-8")
    macros = {k: int(v) for k, v in re.findall(r"#define\s+(PE_\w+)\s+(\d+)", h)}
    for name, expected in (("PE_WORD_COUNT", n_words), ("PE_SYLLABLE_COUNT", n_syl),
                           ("PE_ABBREV_COUNT", len(abbr["rows"])),
                           ("PE_ABBREV_ENTRY_COUNT", len(abbr["entries"])),
                           ("PE_MIN_ABBR_LEN", abbr["min_len"]),
                           ("PE_MAX_ABBR_LEN", abbr["max_len"])):
        if macros.get(name) != expected:
            die(f"{DICT_H.name} 里 {name}={macros.get(name)}，应为 {expected}")
    for macro in ("PE_MAX_KEY_LEN", "PE_MAX_SYL_LEN", "PE_MAX_CHARS_PER_SYL"):
        if macro not in macros:
            die(f"{DICT_H.name} 缺少 {macro}")

    for f in (WORDS_C, SYLS_C, ABBR_C):
        if '#include "pe_dict.h"' not in f.read_text(encoding="utf-8"):
            die(f"{f.name} 没有 #include \"pe_dict.h\" —— 它的 PE_* 维度常量会找不到")

    w = WORDS_C.read_text(encoding="utf-8")
    s = SYLS_C.read_text(encoding="utf-8")
    n_w = len(re.findall(r'^    \{ "', w, re.M))
    n_s = len(re.findall(r'^    \{ "', s, re.M))
    n_cost = len(re.findall(r"^static const uint8_t S_COST_", s, re.M))
    n_close_w = len(re.findall(r"\},\s*$", w, re.M))
    n_close_s = len(re.findall(r"\},\s*$", s, re.M))
    if n_w != n_words or n_close_w != n_words:
        die(f"词表生成物自检失败：{n_w} 条记录 / {n_close_w} 个 '}},'，应为 {n_words}")
    if n_s != n_syl or n_close_s != n_syl or n_cost != n_syl:
        die(f"单字表生成物自检失败：{n_s} 条记录 / {n_close_s} 个 '}},' / {n_cost} 个代价数组，"
            f"应为 {n_syl}")

    # 缩写表：不只数条数，把生成物的内容**逐条和内存里的数据对一遍**。
    # 这张表的 first/count 是累加出来的，错一位就会让某个键指向别人的词 —— 而且
    # 不会崩，只是简拼出莫名其妙的词。
    a = ABBR_C.read_text(encoding="utf-8")
    m = re.search(r"pe_dict_abbrev_entries\[PE_ABBREV_ENTRY_COUNT\] = \{(.*?)\n\};", a, re.S)
    if not m:
        die("缩写表生成物里找不到 pe_dict_abbrev_entries 数组（变量名或格式变了？）")
    entries_out = [int(v) for v in re.findall(r"\d+", m.group(1))]
    if entries_out != abbr["entries"]:
        die(f"缩写表 entries 内容与内存数据不一致"
            f"（生成物 {len(entries_out)} 条，内存 {len(abbr['entries'])} 条）")

    rows_out = re.findall(r"^    \{ 0x([0-9A-F]+)u, (\d+), (\d+), (\d+) \},", a, re.M)
    if len(rows_out) != len(abbr["rows"]):
        die(f"缩写表生成物组数 {len(rows_out)}，应为 {len(abbr['rows'])}")
    off = 0
    for (packed_hex, first, count, penalty), (packed, ab, items, pen) in zip(rows_out, abbr["rows"]):
        if int(packed_hex, 16) != packed or int(first) != off \
                or int(count) != len(items) or int(penalty) != pen:
            die(f"缩写表生成物与内存数据不符（组 #{off}，键 {ab!r}）")
        off += int(count)
    if off != len(entries_out):
        die(f"缩写表组内词数合计 {off}，entries 实有 {len(entries_out)}")

    print(f"   ✅ 生成物自检通过（词表 {n_w} 条、单字表 {n_s} 条 + {n_cost} 个代价数组、"
          f"缩写表 {len(rows_out)} 组 {len(entries_out)} 条，与 {DICT_H.name} 的维度一致）")


# ─────────────────────── main ───────────────────────

def main() -> None:
    print("1/7 读字表 …")
    print("2/7 聚合字频 …")
    print("3/7 建词表与单字表 …")
    flat, syl_rows, widx, max_key, max_syl = build_tables()

    print("4/7 建缩写表（简拼）…")
    abbr = build_abbrev_index(flat)

    print("5/7 自查 …")
    check(flat, syl_rows, abbr, max_key, max_syl)

    print("6/7 用同一套量化跑参考实现（验证量化没劣化）…")
    report_accuracy(flat, syl_rows, abbr, max_key, max_syl)

    print("\n7/7 输出 …")
    emit_dict_h(max_key, max_syl, len(flat), len(syl_rows), abbr)
    emit_words_c(flat, max_key)
    emit_syllables_c(syl_rows)
    emit_abbrev_c(abbr)
    emit_audit(flat, syl_rows, abbr)

    check_emitted(len(flat), len(syl_rows), abbr)

    print("\n抽样（引擎在这个输入下的前 3 个候选）：")
    for inp in ("nihao", "beijing", "jintiantianqi", "nizenmeyang", "tushuguan",
                "nh", "sj", "bjdx"):
        r = reference_convert(inp, flat, syl_rows, abbr, max_key, max_syl)
        print(f"   {inp:<16} {' / '.join(r)}")

    print("\n✅ 完成")


if __name__ == "__main__":
    main()
