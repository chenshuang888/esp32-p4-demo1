/*
 * 单字表：音节 -> 候选字 —— **生成物，永不手改**。
 *
 * 重新生成：
 *     python components/pinyin_engine/scripts/gen_engine_dict.py
 *
 * 数据来源、量化代价的算法、DP 的做法，全部写在生成脚本顶部的注释里。
 * 这份文件里没有任何人工可维护的信息。
 *
 * 统计：415 个音节，每音节最多 20 字（按主读音、字频降序）
 */
#include "pe_dict.h"   /* 维度常量（PE_WORD_COUNT 等）与数组声明都在那里；它自己会带上 pinyin_engine.h */


static const uint8_t S_COST_a[] = { 32, 34, 55, 57, 41, 47, 59 };
static const char    S_CHARS_a[] = "阿啊嗄锕呵腌吖";
static const uint8_t S_COST_ai[] = { 31, 36, 39, 40, 41, 41, 41, 42, 42, 43, 47, 49, 49, 52, 52, 54, 54, 56, 57, 57 };
static const char    S_CHARS_ai[] = "爱埃碍癌艾哀挨矮唉哎隘嗳蔼暧霭瑷叆皑嗌毐";
static const uint8_t S_COST_an[] = { 26, 31, 32, 34, 34, 40, 42, 43, 44, 44, 47, 50, 51, 55, 55, 58, 58, 60, 63, 74 };
static const char    S_CHARS_an[] = "安案按暗岸氨俺庵鞍胺黯谙铵鹌桉垵犴唵盦埯";
static const uint8_t S_COST_ang[] = { 41, 48, 50, 59, 38, 47 };
static const char    S_CHARS_ang[] = "昂盎肮卬仰腌";
static const uint8_t S_COST_ao[] = { 35, 38, 43, 43, 45, 45, 45, 47, 47, 48, 50, 52, 52, 52, 53, 54, 55, 56, 56, 57 };
static const char    S_CHARS_ao[] = "奥澳傲熬凹嶅鳌拗袄懊敖坳嗷翱螯鏖遨鏊骜璈";
static const uint8_t S_COST_ba[] = { 27, 30, 31, 33, 35, 35, 40, 41, 41, 43, 44, 45, 45, 46, 46, 46, 47, 48, 48, 50 };
static const char    S_CHARS_ba[] = "把八巴吧罢拔爸坝霸粑跋扒灞靶叭芭耙疤胈笆";
static const uint8_t S_COST_bai[] = { 28, 29, 34, 36, 36, 40, 50, 56, 57, 59, 60, 31, 32, 34, 43, 45, 51, 57, 57, 69 };
static const char    S_CHARS_bai[] = "白百败拜摆柏掰稗擘捭佰派排伯啡扒呗薜鞴鞁";
static const uint8_t S_COST_ban[] = { 30, 30, 31, 33, 33, 33, 37, 38, 40, 40, 41, 43, 45, 45, 45, 48, 49, 52, 52, 58 };
static const char    S_CHARS_ban[] = "半办般板班版伴颁搬扮斑拌瓣湴扳绊阪瘢坂钣";
static const uint8_t S_COST_bang[] = { 33, 33, 38, 39, 40, 41, 42, 43, 44, 46, 46, 50, 51, 60, 65, 74, 74, 74, 74, 35 };
static const char    S_CHARS_bang[] = "邦帮浜棒榜镑膀绑傍磅蚌梆谤棓蒡𠳐玤塝搒旁";
static const uint8_t S_COST_bao[] = { 28, 28, 30, 31, 36, 36, 36, 36, 38, 38, 40, 43, 45, 46, 49, 49, 49, 51, 51, 53 };
static const char    S_CHARS_bao[] = "报保包宝抱爆暴胞堡薄饱豹鲍孢苞煲褒雹龅鸨";
static const uint8_t S_COST_bei[] = { 25, 27, 29, 33, 36, 37, 37, 37, 38, 39, 40, 44, 47, 47, 47, 49, 51, 51, 52, 57 };
static const char    S_CHARS_bei[] = "北被备背杯辈贝倍悲碑卑陂狈悖惫钡焙呗孛鞴";
static const uint8_t S_COST_ben[] = { 24, 36, 36, 42, 45, 53, 58, 62, 64, 68, 68, 68, 25, 48 };
static const char    S_CHARS_ben[] = "本奔锛苯笨贲畚坌栟坋倴犇体夯";
static const uint8_t S_COST_beng[] = { 42, 45, 47, 47, 48, 48, 55, 61, 64, 74, 74, 26, 35, 40, 44, 46, 47, 49, 55, 67 };
static const char    S_CHARS_beng[] = "崩泵绷蹦迸甭嘣甏琫镚祊平旁榜傍蚌俸抨堋唪";
static const uint8_t S_COST_bi[] = { 27, 29, 34, 34, 35, 35, 35, 36, 38, 38, 38, 39, 41, 42, 43, 43, 43, 43, 44, 44 };
static const char    S_CHARS_bi[] = "比必笔毕币闭避壁臂鼻逼彼碧陛弊蔽鄙璧婢毙";
static const uint8_t S_COST_bian[] = { 28, 28, 29, 31, 35, 39, 40, 40, 41, 42, 44, 45, 46, 47, 50, 51, 52, 54, 54, 58 };
static const char    S_CHARS_bian[] = "变便边编遍辩辨鞭扁匾贬卞辫汴蝙鳊弁砭苄笾";
static const uint8_t S_COST_biao[] = { 25, 31, 42, 43, 48, 48, 51, 53, 53, 54, 54, 55, 61, 61, 64, 64, 66, 67, 74, 74 };
static const char    S_CHARS_biao[] = "表标彪镖婊鳔飙裱杓骠镳膘摽飑俵藨儦幖骉脿";
static const uint8_t S_COST_bie[] = { 28, 46, 48, 52, 56, 69, 34, 35, 43, 45, 45, 57, 62, 69 };
static const char    S_CHARS_bie[] = "别憋鳖瘪蹩咇秘拔蔽扒撇捌苾癿";
static const uint8_t S_COST_bin[] = { 38, 41, 43, 46, 46, 47, 48, 49, 51, 53, 57, 57, 58, 59, 60, 62, 63, 33, 38, 63 };
static const char    S_CHARS_bin[] = "宾滨斌濒彬殡鬓槟缤摈膑镔髌傧豳玢邠份浜攽";
static const uint8_t S_COST_bing[] = { 27, 29, 31, 36, 40, 40, 41, 41, 44, 44, 52, 61, 64, 66, 26, 40, 40, 49, 54, 58 };
static const char    S_CHARS_bing[] = "并兵病冰秉柄禀饼丙炳摒昺邴蛃平屏拼槟枋燹";
static const uint8_t S_COST_bo[] = { 33, 34, 34, 35, 39, 40, 40, 41, 42, 42, 43, 43, 44, 44, 46, 48, 48, 48, 49, 50 };
static const char    S_CHARS_bo[] = "波伯博播拨剥玻卜勃脖搏舶驳膊渤箔钵帛菠簸";
static const uint8_t S_COST_bu[] = { 19, 24, 29, 29, 34, 37, 42, 45, 45, 47, 54, 54, 56, 56, 57, 60, 62, 66, 74, 74 };
static const char    S_CHARS_bu[] = "不部布步补捕怖哺簿埠卟钚埗逋瓿醭晡蔀𬷕𫐓";
static const uint8_t S_COST_ca[] = { 40, 50, 39, 41 };
static const char    S_CHARS_ca[] = "擦嚓拆蔡";
static const uint8_t S_COST_cai[] = { 28, 31, 32, 33, 34, 34, 37, 40, 41, 45, 47, 65 };
static const char    S_CHARS_cai[] = "才采财材菜彩裁猜蔡踩睬偲";
static const uint8_t S_COST_can[] = { 30, 37, 38, 39, 43, 44, 46, 48, 54, 54, 56, 62, 68, 74 };
static const char    S_CHARS_can[] = "参残餐惨蚕灿惭掺粲璨孱骖黪䅟";
static const uint8_t S_COST_cang[] = { 33, 40, 41, 41, 44, 59, 74, 51 };
static const char    S_CHARS_cang[] = "藏仓舱苍沧伧鸧臧";
static const uint8_t S_COST_cao[] = { 33, 35, 37, 41, 45, 48, 49, 60, 63, 74, 30, 47, 68 };
static const char    S_CHARS_cao[] = "草操曹槽漕糙嘈艚螬𥕢造澡慥";
static const uint8_t S_COST_ce[] = { 32, 33, 34, 38, 45, 50, 43, 46 };
static const char    S_CHARS_ce[] = "策测侧册厕恻赦栅";
static const uint8_t S_COST_cen[] = { 41, 49, 65, 30 };
static const char    S_CHARS_cen[] = "涔岑梣参";
static const uint8_t S_COST_ceng[] = { 31, 32, 48, 57, 63, 29, 37, 64 };
static const char    S_CHARS_ceng[] = "曾层蹭噌嶒增僧鄫";
static const uint8_t S_COST_cha[] = { 31, 32, 33, 34, 38, 41, 44, 45, 45, 47, 47, 48, 49, 53, 54, 55, 58, 61, 62, 66 };
static const char    S_CHARS_cha[] = "察查差茶插叉诧杈岔姹茬槎汊搽碴衩锸檫馇镲";
static const uint8_t S_COST_chai[] = { 39, 39, 48, 52, 57, 58, 66, 74, 32, 33, 47, 69 };
static const char    S_CHARS_chai[] = "柴拆钗豺侪虿茝瘥查差搓茈";
static const uint8_t S_COST_chan[] = { 25, 41, 41, 41, 41, 42, 44, 46, 47, 47, 48, 50, 51, 52, 52, 54, 55, 55, 58, 59 };
static const char    S_CHARS_chan[] = "产缠颤澶阐禅铲蝉蟾搀馋谄忏浐谗婵潺梴廛儳";
static const uint8_t S_COST_chang[] = { 27, 27, 33, 34, 37, 38, 39, 40, 40, 41, 45, 49, 49, 50, 51, 53, 54, 56, 56, 57 };
static const char    S_CHARS_chang[] = "常场昌厂唱肠偿畅尝倡敞猖娼怅嫦阊苌氅徜菖";
static const uint8_t S_COST_chao[] = { 30, 32, 36, 38, 42, 42, 43, 45, 45, 50, 50, 65, 69, 69, 43, 47, 65 };
static const char    S_CHARS_chao[] = "朝超潮炒抄吵巢嘲钞晁焯耖怊弨剿绰槱";
static const uint8_t S_COST_che[] = { 29, 36, 37, 40, 47, 47, 56, 62, 74, 37, 38, 39, 41, 41 };
static const char    S_CHARS_che[] = "车撤彻扯澈掣砗坼㬚池尺拆宅斥";
static const uint8_t S_COST_chen[] = { 32, 34, 34, 39, 40, 41, 41, 41, 44, 45, 47, 48, 49, 50, 54, 55, 56, 57, 58, 60 };
static const char    S_CHARS_chen[] = "陈沉臣尘晨宸趁辰衬琛嗔谌忱谶郴龀碜抻榇瞋";
static const uint8_t S_COST_cheng[] = { 23, 26, 28, 28, 32, 36, 36, 38, 41, 41, 42, 44, 46, 46, 48, 48, 50, 50, 52, 54 };
static const char    S_CHARS_cheng[] = "成城称程承乘呈诚惩撑丞澄橙逞秤铖骋晟瞠蛏";
static const uint8_t S_COST_chi[] = { 30, 30, 37, 38, 38, 38, 39, 40, 41, 41, 42, 43, 45, 46, 46, 47, 48, 48, 48, 49 };
static const char    S_CHARS_chi[] = "持吃池赤尺迟齿驰斥翅耻痴嗤敕侈弛炽叱哧螭";
static const uint8_t S_COST_chong[] = { 32, 33, 36, 36, 42, 50, 51, 51, 51, 60, 61, 64, 64, 74, 74, 74, 25, 25, 38, 42 };
static const char    S_CHARS_chong[] = "冲充崇虫宠舂憧铳忡艟珫茺翀㳘埫摏种重涌烛";
static const uint8_t S_COST_chou[] = { 37, 38, 39, 40, 41, 41, 41, 42, 43, 44, 46, 48, 53, 54, 56, 59, 66, 67, 68, 74 };
static const char    S_CHARS_chou[] = "抽仇筹愁臭丑酬畴绸瞅稠踌惆雠俦椆杻瘳犨侴";
static const uint8_t S_COST_chu[] = { 22, 28, 31, 31, 34, 34, 36, 37, 40, 43, 47, 47, 47, 48, 48, 48, 49, 50, 50, 51 };
static const char    S_CHARS_chu[] = "出处除初楚础触储畜厨矗褚橱躇雏锄杵黜绌搐";
static const uint8_t S_COST_chua[] = { 64, 46 };
static const char    S_CHARS_chua[] = "欻撮";
static const uint8_t S_COST_chuai[] = { 46, 51, 52, 56, 68, 52 };
static const char    S_CHARS_chuai[] = "揣踹啜嘬搋啐";
static const uint8_t S_COST_chuan[] = { 29, 33, 34, 35, 41, 42, 53, 54, 54, 58, 58, 68, 51, 51, 55 };
static const char    S_CHARS_chuan[] = "传船穿川串喘椽钏舛氚遄圌踹惴掾";
static const uint8_t S_COST_chuang[] = { 31, 35, 37, 37, 46, 47, 53, 68, 44, 50, 52, 60, 67 };
static const char    S_CHARS_chuang[] = "创床闯窗疮幢怆噇葱舂囱橦漴";
static const uint8_t S_COST_chui[] = { 38, 38, 44, 44, 47, 47, 48, 49, 49, 67, 66, 68, 74 };
static const char    S_CHARS_chui[] = "吹垂椎锤捶炊棰槌陲倕惙圌魋";
static const uint8_t S_COST_chun[] = { 33, 36, 41, 41, 43, 45, 48, 51, 52, 59, 74, 74, 74, 74, 51, 57, 59, 60 };
static const char    S_CHARS_chun[] = "春纯唇醇淳蠢椿莼鹑蝽堾瑃𬭚䲠沌朐肫楯";
static const uint8_t S_COST_chuo[] = { 44, 47, 48, 53, 55, 60, 66, 68, 36, 45, 46, 48, 48, 49, 50, 50, 52, 52, 58, 64 };
static const char    S_CHARS_chuo[] = "戳绰辍龊踔婼惙逴促簇醛躇踱淖荃焯斫啜趵蔟";
static const uint8_t S_COST_ci[] = { 26, 27, 33, 34, 37, 37, 38, 40, 41, 42, 43, 44, 45, 47, 49, 52, 57, 57, 57, 69 };
static const char    S_CHARS_ci[] = "此次词刺磁辞慈赐瓷雌茨祠伺糍佽疵泚呲鹚茈";
static const uint8_t S_COST_cong[] = { 26, 39, 41, 42, 44, 45, 49, 50, 52, 53, 54, 54, 58, 61, 37, 57 };
static const char    S_CHARS_cong[] = "从丛聪匆葱璁淙熜囱骢苁琮枞悰窗偬";
static const uint8_t S_COST_cou[] = { 42, 50, 59, 28, 36, 37, 45, 51, 64 };
static const char    S_CHARS_cou[] = "凑辏腠族奏趣簇揍蔟";
static const uint8_t S_COST_cu[] = { 36, 38, 45, 45, 49, 50, 53, 54, 63, 63, 64, 31, 37, 39, 41, 54, 59 };
static const char    S_CHARS_cu[] = "促粗醋簇蹙猝蹴酢徂殂蔟且趣卒戚槭捽";
static const uint8_t S_COST_cuan[] = { 44, 48, 49, 53, 58, 62, 66, 44, 46, 74 };
static const char    S_CHARS_cuan[] = "窜篡蹿撺爨汆镩蹲攒僔";
static const uint8_t S_COST_cui[] = { 40, 41, 41, 44, 44, 44, 48, 49, 51, 52, 52, 53, 60, 64, 69, 74, 25, 31, 39, 39 };
static const char    S_CHARS_cui[] = "催翠脆粹摧崔萃悴淬啐璀瘁榱毳漼缞体察衰卒";
static const uint8_t S_COST_cun[] = { 31, 31, 40, 49, 55, 62, 44, 47 };
static const char    S_CHARS_cun[] = "存村寸忖皴邨蹲浚";
static const uint8_t S_COST_cuo[] = { 33, 36, 43, 46, 46, 47, 50, 53, 55, 57, 57, 58, 59, 60, 65, 65, 68, 69, 26, 33 };
static const char    S_CHARS_cuo[] = "错措挫撮磋搓棤锉厝嵯蹉痤鹾矬酂脞瑳莝最差";
static const uint8_t S_COST_da[] = { 19, 28, 28, 33, 39, 44, 48, 49, 49, 50, 50, 53, 54, 57, 57, 57, 58, 61, 63, 65 };
static const char    S_CHARS_da[] = "大打达答搭鞑瘩沓嗒靼哒耷褡怛垯跶笪妲炟阘";
static const uint8_t S_COST_dai[] = { 25, 29, 32, 38, 38, 38, 38, 43, 44, 44, 45, 46, 47, 49, 49, 55, 55, 60, 60, 62 };
static const char    S_CHARS_dai[] = "代带待戴袋贷呆逮岱歹怠傣黛绐殆迨玳呔埭骀";
static const uint8_t S_COST_dan[] = { 26, 30, 32, 33, 35, 36, 37, 37, 37, 41, 44, 45, 47, 47, 50, 52, 54, 54, 54, 55 };
static const char    S_CHARS_dan[] = "但单弹担丹蛋胆淡旦诞耽氮惮澹郸掸疸眈殚儋";
static const uint8_t S_COST_dang[] = { 25, 29, 38, 39, 39, 44, 49, 49, 52, 54, 54, 55, 55, 55, 56, 74, 74 };
static const char    S_CHARS_dang[] = "当党荡档挡垱珰宕裆铛筜谠菪砀凼𬍡𣗋";
static const uint8_t S_COST_dao[] = { 22, 23, 28, 32, 33, 33, 38, 41, 42, 43, 46, 46, 47, 49, 55, 56, 67, 68, 74, 74 };
static const char    S_CHARS_dao[] = "到道导倒岛刀盗稻蹈捣悼祷叨焘纛氘鱽捯忉舠";
static const uint8_t S_COST_de[] = { 22, 23, 29, 56, 64, 21, 31, 34, 56 };
static const char    S_CHARS_de[] = "的得德锝嘚地底登陟";
static const uint8_t S_COST_dei[] = { 23, 64 };
static const char    S_CHARS_dei[] = "得嘚";
static const uint8_t S_COST_den[] = { 74 };
static const char    S_CHARS_den[] = "扽";
static const uint8_t S_COST_deng[] = { 24, 34, 35, 38, 42, 45, 48, 49, 52, 53, 62, 66, 74, 44, 46, 74 };
static const char    S_CHARS_deng[] = "等登灯邓瞪凳蹬磴镫噔戥嶝璒澄橙憕";
static const uint8_t S_COST_di[] = { 21, 26, 30, 31, 31, 32, 33, 36, 38, 39, 40, 40, 41, 42, 43, 44, 44, 47, 47, 48 };
static const char    S_CHARS_di[] = "地第帝低底弟敌抵递滴堤迪棣蒂缔笛狄砥邸涤";
static const uint8_t S_COST_dia[] = { 55 };
static const char    S_CHARS_dia[] = "嗲";
static const uint8_t S_COST_dian[] = { 27, 27, 34, 34, 35, 42, 42, 43, 43, 44, 45, 46, 47, 48, 49, 49, 49, 50, 54, 55 };
static const char    S_CHARS_dian[] = "点电典殿店颠奠甸垫淀滇佃碘靛癫巅惦掂玷钿";
static const uint8_t S_COST_diao[] = { 30, 35, 37, 40, 44, 48, 48, 50, 50, 52, 56, 62, 67, 68, 68, 33, 35, 37, 37, 39 };
static const char    S_CHARS_diao[] = "调掉雕吊钓貂刁凋碉叼鲷铫铞汈窎刀跳挑鸟敦";
static const uint8_t S_COST_die[] = { 38, 39, 41, 44, 45, 45, 47, 47, 53, 55, 57, 58, 61, 63, 64, 68, 74, 74, 74, 74 };
static const char    S_CHARS_die[] = "爹跌叠迭蝶碟牒谍喋嗲堞鲽耋蹀垤瓞昳绖䏲𫶇";
static const uint8_t S_COST_ding[] = { 25, 34, 35, 36, 41, 41, 42, 44, 46, 51, 53, 54, 55, 56, 57, 58, 59, 65, 68, 35 };
static const char    S_CHARS_ding[] = "定顶丁订鼎钉盯叮锭啶仃铤玎碇酊疔腚耵萣灯";
static const uint8_t S_COST_diu[] = { 40, 65 };
static const char    S_CHARS_diu[] = "丢铥";
static const uint8_t S_COST_dong[] = { 24, 26, 34, 37, 37, 38, 41, 45, 46, 47, 51, 54, 55, 56, 58, 58, 59, 61, 74, 74 };
static const char    S_CHARS_dong[] = "动东洞董冬懂冻栋侗咚峒恫硐氡胴胨鸫岽垌𬟽";
static const uint8_t S_COST_dou[] = { 24, 31, 37, 41, 43, 44, 45, 45, 50, 51, 61, 63, 30, 33, 43, 45, 56, 63 };
static const char    S_CHARS_dou[] = "都斗豆抖陡兜窦逗蚪痘篼蔸投读剅逾窬钭";
static const uint8_t S_COST_du[] = { 26, 32, 33, 34, 34, 37, 38, 38, 41, 42, 44, 46, 46, 48, 48, 49, 51, 51, 52, 53 };
static const char    S_CHARS_du[] = "度独读毒督渡杜肚赌堵睹笃妒嘟渎镀牍犊蠹椟";
static const uint8_t S_COST_duan[] = { 30, 31, 34, 34, 43, 46, 54, 56, 68, 74, 74, 51 };
static const char    S_CHARS_duan[] = "段断端短锻缎椴煅簖塅瑖踹";
static const uint8_t S_COST_dui[] = { 23, 28, 38, 41, 53, 54, 58, 69, 34, 39, 49, 74 };
static const char    S_CHARS_dui[] = "对队堆兑怼镦碓祋追敦槌𬭚";
static const uint8_t S_COST_dun[] = { 35, 36, 38, 39, 41, 44, 47, 47, 48, 51, 51, 52, 52, 55, 60, 60, 63, 63, 39, 44 };
static const char    S_CHARS_dun[] = "顿吨盾敦墩蹲炖钝遁沌囤盹趸惇楯砘蹾礅俊豚";
static const uint8_t S_COST_duo[] = { 24, 35, 39, 39, 43, 45, 45, 47, 47, 48, 48, 48, 48, 49, 50, 51, 54, 58, 65, 66 };
static const char    S_CHARS_duo[] = "多夺躲朵舵堕跺剁垛惰哆踱掇亸咄铎裰哚埵剟";
static const uint8_t S_COST_e[] = { 33, 34, 34, 35, 41, 41, 43, 44, 44, 45, 45, 46, 46, 46, 47, 48, 49, 50, 51, 51 };
static const char    S_CHARS_e[] = "额鄂俄恶饿鹅娥峨厄遏萼愕鳄扼蛾讹噩垩颚鹗";
static const uint8_t S_COST_ei[] = { 66 };
static const char    S_CHARS_ei[] = "欸";
static const uint8_t S_COST_en[] = { 34, 50, 55 };
static const char    S_CHARS_en[] = "恩蒽摁";
static const uint8_t S_COST_er[] = { 24, 27, 28, 30, 35, 46, 49, 50, 52, 58, 58, 61, 63, 74, 74, 74, 74, 47, 58, 74 };
static const char    S_CHARS_er[] = "而二儿尔耳饵迩贰洱珥鸸鲕铒佴陑咡峏濡臑耏";
static const uint8_t S_COST_fa[] = { 23, 24, 38, 38, 39, 40, 47, 53, 53, 59, 35, 36, 74 };
static const char    S_CHARS_fa[] = "发法罚乏伐阀筏砝珐垡拔泛酦";
static const uint8_t S_COST_fan[] = { 28, 32, 34, 35, 35, 35, 36, 36, 37, 38, 39, 42, 43, 43, 44, 45, 47, 48, 49, 50 };
static const char    S_CHARS_fan[] = "反范饭犯翻繁凡泛番烦返贩樊帆藩蕃梵矾钒幡";
static const uint8_t S_COST_fang[] = { 24, 28, 31, 31, 36, 37, 38, 39, 40, 42, 44, 54, 54, 55, 55, 58, 61, 65, 74, 51 };
static const char    S_CHARS_fang[] = "方放防房访纺仿芳坊妨肪鲂枋舫昉钫邡牥蚄彷";
static const uint8_t S_COST_fei[] = { 29, 31, 31, 36, 37, 39, 39, 41, 43, 43, 43, 45, 48, 49, 49, 49, 50, 50, 51, 51 };
static const char    S_CHARS_fei[] = "非飞费废肥菲妃肺匪啡沸斐扉吠腓绯芾翡霏诽";
static const uint8_t S_COST_fen[] = { 24, 33, 36, 37, 37, 39, 41, 41, 41, 42, 43, 45, 45, 47, 48, 48, 51, 59, 59, 61 };
static const char    S_CHARS_fen[] = "分份粉奋纷愤氛芬吩坟焚粪酚瀵忿汾翂鲼棻偾";
static const uint8_t S_COST_feng[] = { 28, 32, 34, 35, 36, 37, 37, 40, 40, 40, 40, 41, 44, 47, 48, 50, 52, 53, 55, 57 };
static const char    S_CHARS_feng[] = "风封丰峰奉锋凤缝蜂疯冯逢讽俸枫烽葑沣酆砜";
static const uint8_t S_COST_fo[] = { 35 };
static const char    S_CHARS_fo[] = "佛";
static const uint8_t S_COST_fou[] = { 34, 55, 19, 67, 69, 74 };
static const char    S_CHARS_fou[] = "否缶不垺芣衃";
static const uint8_t S_COST_fu[] = { 28, 30, 30, 31, 31, 31, 32, 32, 33, 34, 34, 35, 35, 36, 37, 37, 37, 38, 38, 38 };
static const char    S_CHARS_fu[] = "府复夫服副富负父福妇附佛付幅伏符腹浮腐辅";
static const uint8_t S_COST_ga[] = { 44, 45, 46, 47, 52, 55, 57, 63, 74, 39, 43, 44, 46, 50 };
static const char    S_CHARS_ga[] = "尬伽噶嘎呷尕旮钆尜夹咖胳轧戛";
static const uint8_t S_COST_gai[] = { 28, 30, 35, 36, 42, 43, 45, 53, 55, 65, 68, 74, 74, 33, 35, 42, 46, 46, 74, 74 };
static const char    S_CHARS_gai[] = "改该概盖丐钙溉垓赅陔戤荄晐核汽咳骸芥𬮿胲";
static const uint8_t S_COST_gan[] = { 29, 30, 33, 34, 37, 39, 39, 44, 44, 46, 47, 48, 48, 49, 53, 53, 53, 54, 55, 56 };
static const char    S_CHARS_gan[] = "干感敢赶甘杆肝赣尴柑竿苷橄秆酐绀擀淦矸旰";
static const uint8_t S_COST_gang[] = { 33, 33, 35, 36, 38, 38, 44, 46, 49, 51, 53, 60, 65, 69, 74, 74, 74, 32, 46, 46 };
static const char    S_CHARS_gang[] = "刚港岗钢冈纲缸杠肛罡冮戆筻矼㭎鿍堽抗亢扛";
static const uint8_t S_COST_gao[] = { 24, 30, 36, 40, 40, 42, 45, 46, 48, 49, 50, 52, 53, 53, 55, 56, 56, 58, 60, 60 };
static const char    S_CHARS_gao[] = "高告搞稿糕膏皋诰镐睾羔篙缟锆槁杲筶藁槔郜";
static const uint8_t S_COST_ge[] = { 21, 26, 30, 30, 33, 35, 37, 37, 39, 41, 41, 44, 44, 45, 47, 47, 47, 48, 48, 48 };
static const char    S_CHARS_ge[] = "个各革格哥歌阁隔割葛戈搁胳鸽咯铬骼疙滆圪";
static const uint8_t S_COST_gei[] = { 28 };
static const char    S_CHARS_gei[] = "给";
static const uint8_t S_COST_gen[] = { 30, 31, 50, 51, 58, 59, 42 };
static const char    S_CHARS_gen[] = "根跟亘艮茛哏痕";
static const uint8_t S_COST_geng[] = { 28, 39, 45, 45, 46, 47, 48, 50, 51, 57, 58, 61, 74, 74, 74, 36, 40, 44, 46, 50 };
static const char    S_CHARS_geng[] = "更耕梗羹耿庚哽绠埂鲠暅赓浭𬒔鹒硬颈邢亢亘";
static const uint8_t S_COST_gong[] = { 24, 24, 28, 30, 31, 32, 32, 37, 39, 40, 42, 42, 43, 46, 47, 48, 52, 54, 56, 74 };
static const char    S_CHARS_gong[] = "工公共功宫供攻贡拱恭弓巩躬汞龚蚣觥肱珙䢼";
static const uint8_t S_COST_gou[] = { 30, 33, 35, 37, 37, 40, 42, 46, 48, 51, 52, 52, 53, 53, 53, 53, 54, 55, 58, 61 };
static const char    S_CHARS_gou[] = "构够购沟狗勾钩苟垢枸媾佝彀诟篝岣姤笱鞲觏";
static const uint8_t S_COST_gu[] = { 29, 31, 32, 33, 34, 34, 35, 35, 36, 38, 38, 42, 44, 44, 44, 44, 46, 47, 48, 49 };
static const char    S_CHARS_gu[] = "古股故顾固姑骨谷鼓孤估雇辜咕菇沽鸪箍钴鹘";
static const uint8_t S_COST_gua[] = { 36, 38, 39, 42, 43, 46, 50, 54, 55, 55, 56, 62, 74, 33, 40, 51, 52 };
static const char    S_CHARS_gua[] = "挂寡瓜刮卦褂剐聒诖胍鸹栝坬括舌惴呱";
static const uint8_t S_COST_guai[] = { 34, 41, 44, 60, 69 };
static const char    S_CHARS_guai[] = "怪拐乖掴夬";
static const uint8_t S_COST_guan[] = { 26, 28, 29, 30, 34, 37, 37, 37, 39, 40, 41, 49, 53, 54, 55, 56, 57, 57, 58, 59 };
static const char    S_CHARS_guan[] = "关管官观馆冠贯惯灌棺罐莞倌掼鹳盥涫鳏筦琯";
static const uint8_t S_COST_guang[] = { 28, 29, 45, 49, 53, 53, 57, 57, 65, 74, 74, 35, 44, 51 };
static const char    S_CHARS_guang[] = "光广逛胱犷咣垙洸桄珖𨐈横恍潢";
static const uint8_t S_COST_gui[] = { 28, 33, 34, 36, 37, 39, 40, 42, 42, 42, 43, 43, 44, 46, 48, 49, 50, 51, 51, 51 };
static const char    S_CHARS_gui[] = "规贵归鬼桂跪轨柜硅龟圭瑰诡闺傀癸珪鳜皈桧";
static const uint8_t S_COST_gun[] = { 39, 41, 45, 53, 54, 56, 59, 35, 35, 59 };
static const char    S_CHARS_gun[] = "滚棍衮绲鲧辊磙卷混琯";
static const uint8_t S_COST_guo[] = { 20, 24, 28, 36, 38, 42, 47, 54, 54, 57, 60, 61, 62, 62, 64, 65, 66, 74, 74, 28 };
static const char    S_CHARS_guo[] = "国过果郭锅裹椁虢帼埚蝈崞呙腘蜾馘馃粿𬇹活";
static const uint8_t S_COST_ha[] = { 35, 48, 58, 39, 41, 42, 55 };
static const char    S_CHARS_ha[] = "哈蛤铪吓呵虾獬";
static const uint8_t S_COST_hai[] = { 25, 26, 32, 33, 42, 44, 46, 48, 49, 50, 61, 68, 74, 42, 58, 66 };
static const char    S_CHARS_hai[] = "还海害孩亥骇骸嗐氦嗨醢咍胲咳咴浬";
static const uint8_t S_COST_han[] = { 27, 34, 36, 36, 37, 37, 40, 41, 41, 41, 42, 44, 45, 46, 47, 47, 48, 49, 49, 50 };
static const char    S_CHARS_han[] = "汉含韩寒汗喊函旱罕翰涵憾悍撼焊酣捍憨邯鼾";
static const uint8_t S_COST_hang[] = { 32, 38, 48, 48, 50, 61, 62, 23, 39, 40, 41, 44, 46, 50, 51, 53 };
static const char    S_CHARS_hang[] = "航杭夯绗珩沆颃行狼狠巷炕吭肮桁酐";
static const uint8_t S_COST_hao[] = { 25, 29, 34, 38, 39, 40, 43, 44, 45, 47, 48, 49, 50, 53, 54, 55, 56, 57, 59, 60 };
static const char    S_CHARS_hao[] = "好号毫豪耗浩灏郝蒿壕嚎皓濠昊嗥貉蚝颢薅鄗";
static const uint8_t S_COST_he[] = { 20, 26, 28, 29, 33, 34, 37, 39, 39, 40, 41, 41, 43, 43, 47, 47, 47, 49, 49, 50 };
static const char    S_CHARS_he[] = "和合河何核喝荷赫贺鹤盒呵褐禾劾颌龢菏壑阖";
static const uint8_t S_COST_hei[] = { 31, 45, 74, 50 };
static const char    S_CHARS_hei[] = "黑嘿𬭶嗨";
static const uint8_t S_COST_hen[] = { 28, 38, 40, 42, 74, 42, 51, 59 };
static const char    S_CHARS_hen[] = "很恨狠痕𬣳掀艮哏";
static const uint8_t S_COST_heng[] = { 35, 37, 39, 41, 43, 51, 56, 57, 63, 69, 23, 50, 55 };
static const char    S_CHARS_heng[] = "横衡恒哼亨桁蘅姮鸻堼行珩訇";
static const uint8_t S_COST_hng[] = { 41 };
static const char    S_CHARS_hng[] = "哼";
static const uint8_t S_COST_hong[] = { 30, 34, 37, 37, 39, 42, 42, 44, 44, 47, 52, 54, 55, 59, 59, 61, 62, 63, 66, 68 };
static const char    S_CHARS_hong[] = "红洪宏鸿轰哄虹烘弘讧闳泓訇竑薨纮蕻吽黉硔";
static const uint8_t S_COST_hou[] = { 23, 30, 35, 39, 40, 42, 44, 53, 54, 58, 59, 59, 62, 62, 65, 65, 74, 74, 74, 63 };
static const char    S_CHARS_hou[] = "后候厚侯猴喉吼逅鲎骺垕篌堠糇瘊齁郈𬭤鲘吽";
static const uint8_t S_COST_hu[] = { 27, 31, 32, 33, 33, 33, 34, 34, 36, 36, 39, 42, 42, 44, 45, 45, 46, 48, 48, 49 };
static const char    S_CHARS_hu[] = "湖护户乎胡呼互忽狐虎糊沪壶弧瑚葫唬蝴扈惚";
static const uint8_t S_COST_hua[] = { 26, 28, 28, 29, 30, 33, 37, 44, 47, 49, 51, 56, 62, 74, 32, 33, 44, 45, 48, 53 };
static const char    S_CHARS_hua[] = "化话华花划画滑哗桦猾骅铧婳觟找敌哇豁叱稞";
static const uint8_t S_COST_huai[] = { 34, 35, 39, 45, 47, 51, 30, 40, 50, 55 };
static const char    S_CHARS_huai[] = "怀坏淮槐徊踝划圳坯喟";
static const uint8_t S_COST_huan[] = { 31, 33, 33, 36, 37, 40, 41, 42, 43, 43, 44, 45, 47, 48, 49, 50, 51, 51, 52, 53 };
static const char    S_CHARS_huan[] = "环欢换缓患唤幻浣焕寰桓宦獾痪鬟涣鲩豢圜洹";
static const uint8_t S_COST_huang[] = { 29, 29, 38, 39, 40, 41, 43, 44, 44, 44, 47, 48, 48, 49, 51, 52, 52, 52, 53, 54 };
static const char    S_CHARS_huang[] = "黄皇荒慌晃煌凰惶恍谎簧幌蝗鳇潢徨湟璜隍肓";
static const uint8_t S_COST_hui[] = { 22, 27, 33, 36, 36, 37, 37, 38, 39, 39, 39, 40, 41, 44, 45, 45, 46, 46, 46, 47 };
static const char    S_CHARS_hui[] = "会回挥汇灰毁恢绘辉惠徽慧悔贿讳晖诲晦烩秽";
static const uint8_t S_COST_hun[] = { 35, 35, 38, 40, 40, 46, 52, 53, 58, 60, 61, 38, 41, 45, 54, 61, 74, 74 };
static const char    S_CHARS_hun[] = "婚混魂昏浑荤馄诨阍惛溷昆棍捆珲湣婫碈";
static const uint8_t S_COST_huo[] = { 27, 28, 30, 32, 34, 37, 40, 40, 41, 42, 45, 50, 54, 54, 57, 59, 59, 61, 61, 63 };
static const char    S_CHARS_huo[] = "或活火获货伙霍祸惑佸豁夥镬蠖藿砉嚯锪攉濩";
static const uint8_t S_COST_ji[] = { 25, 27, 27, 28, 28, 28, 28, 29, 29, 29, 29, 29, 30, 30, 30, 31, 31, 32, 33, 33 };
static const char    S_CHARS_ji[] = "机级及己基计几济技记集即际积极纪击急继辑";
static const uint8_t S_COST_jia[] = { 23, 26, 30, 33, 34, 34, 36, 37, 38, 39, 39, 40, 44, 44, 45, 45, 46, 48, 48, 50 };
static const char    S_CHARS_jia[] = "家加价架假甲嘉佳驾夹贾嫁颊钾迦稼茄珈枷浃";
static const uint8_t S_COST_jian[] = { 26, 26, 26, 29, 32, 32, 33, 33, 33, 34, 35, 35, 35, 35, 37, 37, 38, 38, 38, 39 };
static const char    S_CHARS_jian[] = "建间见件检监剑简坚减渐健兼舰尖箭键肩践鉴";
static const uint8_t S_COST_jiang[] = { 26, 27, 32, 33, 35, 38, 39, 39, 41, 42, 42, 43, 46, 48, 49, 53, 57, 59, 60, 61 };
static const char    S_CHARS_jiang[] = "将江降讲奖蒋疆匠浆姜僵酱桨缰绛犟弶豇糨鳉";
static const uint8_t S_COST_jiao[] = { 27, 28, 29, 30, 33, 33, 37, 39, 40, 40, 40, 43, 43, 43, 43, 44, 45, 45, 45, 45 };
static const char    S_CHARS_jiao[] = "教交叫较角脚焦胶郊椒轿娇剿缴搅骄礁浇绞窖";
static const uint8_t S_COST_jie[] = { 28, 28, 28, 29, 31, 32, 33, 34, 34, 35, 36, 37, 37, 38, 38, 39, 39, 40, 41, 42 };
static const char    S_CHARS_jie[] = "解结接界节阶街介届借姐截皆戒杰揭捷洁劫竭";
static const uint8_t S_COST_jin[] = { 25, 27, 28, 29, 32, 32, 32, 34, 36, 37, 37, 37, 38, 40, 41, 42, 42, 44, 49, 49 };
static const char    S_CHARS_jin[] = "进金今近尽仅紧禁津斤劲晋锦筋谨巾浸襟瑾矜";
static const uint8_t S_COST_jing[] = { 24, 28, 30, 31, 32, 32, 33, 33, 34, 34, 35, 36, 36, 36, 37, 37, 37, 38, 39, 40 };
static const char    S_CHARS_jing[] = "经京精境景竟惊荆静警径竞睛敬镜晶井净靖颈";
static const uint8_t S_COST_jiong[] = { 47, 47, 48, 59, 61, 61, 63, 74, 74, 74, 74, 51, 65 };
static const char    S_CHARS_jiong[] = "炯迥窘炅冏颎扃坰泂𬳶䌹坷垧";
static const uint8_t S_COST_jiu[] = { 23, 30, 32, 32, 33, 34, 34, 40, 44, 44, 46, 47, 47, 48, 48, 49, 49, 50, 52, 52 };
static const char    S_CHARS_jiu[] = "就究九酒久救旧纠舅揪鸠灸柩阄咎疚臼桕韭鹫";
static const uint8_t S_COST_ju[] = { 29, 29, 29, 31, 31, 33, 33, 35, 36, 36, 39, 39, 40, 41, 43, 44, 44, 44, 46, 46 };
static const char    S_CHARS_ju[] = "据举具局居句剧巨距聚拒俱惧矩炬拘菊咀驹锯";
static const uint8_t S_COST_juan[] = { 35, 36, 41, 44, 44, 44, 45, 46, 51, 52, 54, 57, 58, 64, 68, 74, 27, 37, 47, 47 };
static const char    S_CHARS_juan[] = "卷涓捐鹃眷倦娟绢隽镌鄄蠲锩狷桊焆身圈蕊眩";
static const uint8_t S_COST_jue[] = { 28, 31, 32, 40, 41, 44, 44, 45, 46, 48, 49, 50, 50, 51, 51, 53, 54, 54, 56, 56 };
static const char    S_CHARS_jue[] = "决觉绝掘爵诀厥嚼崛抉倔獗蕨撅攫蹶傕谲噘镢";
static const uint8_t S_COST_jun[] = { 25, 31, 35, 36, 38, 39, 42, 42, 47, 47, 48, 59, 59, 63, 65, 66, 67, 68, 69, 74 };
static const char    S_CHARS_jun[] = "军均君郡菌俊钧峻浚竣骏麇皲珺捃畯焌鲪莙晙";
static const uint8_t S_COST_ka[] = { 35, 41, 43, 50, 59, 47 };
static const char    S_CHARS_ka[] = "卡喀咖咔胩咯";
static const uint8_t S_COST_kai[] = { 25, 39, 42, 46, 48, 50, 51, 51, 55, 60, 61, 61, 74, 74, 74, 33, 34, 39, 42, 47 };
static const char    S_CHARS_kai[] = "开凯慨楷揩恺铠忾闿锴剀锎炌垲蒈核喝岂渴劾";
static const uint8_t S_COST_kan[] = { 26, 38, 40, 40, 43, 44, 46, 47, 49, 50, 55, 59, 64, 65, 66, 67, 74, 37, 44, 60 };
static const char    S_CHARS_kan[] = "看刊堪砍勘坎槛瞰侃龛嵁戡崁莰磡衎墈喊嵌靬";
static const uint8_t S_COST_kang[] = { 32, 34, 44, 45, 46, 46, 49, 55, 56, 57, 74, 74, 37, 38, 38, 41, 61 };
static const char    S_CHARS_kang[] = "抗康炕慷亢扛糠钪闶伉𡐓𩾌奋荒杭坑沆";
static const uint8_t S_COST_kao[] = { 29, 34, 39, 48, 50, 52, 54, 56, 68, 74, 36, 55, 68, 68 };
static const char    S_CHARS_kao[] = "考靠烤拷铐犒栲尻洘㸆搞槁訄熇";
static const uint8_t S_COST_ke[] = { 23, 27, 30, 31, 32, 35, 39, 39, 42, 42, 42, 43, 43, 46, 47, 48, 49, 51, 51, 52 };
static const char    S_CHARS_ke[] = "可科克客刻课颗壳柯渴咳磕棵珂苛恪蝌坷缂瞌";
static const uint8_t S_COST_kei[] = { 32, 64 };
static const char    S_CHARS_kei[] = "刻剋";
static const uint8_t S_COST_ken[] = { 35, 45, 46, 47, 51, 57, 40, 55, 74, 74 };
static const char    S_CHARS_ken[] = "肯恳垦啃龈裉狠垠珢硍";
static const uint8_t S_COST_keng[] = { 41, 46, 49, 63, 52, 54, 74 };
static const char    S_CHARS_keng[] = "坑吭铿硁忐硎硍";
static const uint8_t S_COST_kong[] = { 29, 34, 36, 36, 51, 57, 59, 68, 69, 39, 50, 69 };
static const char    S_CHARS_kong[] = "空控孔恐崆倥箜硿埪腔穹矼";
static const uint8_t S_COST_kou[] = { 26, 40, 42, 44, 50, 56, 56, 64, 65, 74, 47, 52, 53, 58, 69 };
static const char    S_CHARS_kou[] = "口扣叩寇抠芤蔻眍筘𫸩挎佝彀刳竘";
static const uint8_t S_COST_ku[] = { 33, 35, 36, 41, 41, 42, 42, 50, 51, 56, 58, 66, 74, 29, 35, 38, 40, 47, 65, 66 };
static const char    S_CHARS_ku[] = "苦库哭枯酷窟裤绔骷喾刳矻圐古圣跨掘挎齁朏";
static const uint8_t S_COST_kua[] = { 38, 41, 44, 47, 49, 58, 61, 54 };
static const char    S_CHARS_kua[] = "跨夸垮挎胯姱侉髁";
static const uint8_t S_COST_kuai[] = { 30, 33, 45, 46, 54, 55, 58, 60, 61, 69, 74, 22, 44, 48, 61 };
static const char    S_CHARS_kuai[] = "快块筷蒯脍狯侩哙郐鲙㧟会魁傀浍";
static const uint8_t S_COST_kuan[] = { 35, 35, 56, 29, 43 };
static const char    S_CHARS_kuan[] = "宽款髋完棵";
static const uint8_t S_COST_kuang[] = { 31, 34, 38, 43, 44, 46, 47, 49, 52, 53, 53, 56, 60, 64, 65, 66, 69, 74, 34, 36 };
static const char    S_CHARS_kuang[] = "况矿狂旷框眶匡筐诳诓哐邝圹纩贶夼洭𫛭兄呈";
static const uint8_t S_COST_kui[] = { 39, 42, 42, 44, 45, 45, 45, 46, 46, 46, 47, 49, 50, 51, 53, 54, 55, 56, 56, 56 };
static const char    S_CHARS_kui[] = "亏溃愧魁逵盔窥奎葵馈夔戣匮隗睽喹喟馗揆聩";
static const uint8_t S_COST_kun[] = { 35, 38, 42, 45, 50, 53, 54, 58, 59, 60, 61, 61, 62, 63, 63, 69, 74, 35, 39 };
static const char    S_CHARS_kun[] = "困昆坤捆醌锟髡堃鲲阃悃壸焜琨裈鹍婫混卵";
static const uint8_t S_COST_kuo[] = { 33, 34, 38, 46, 56, 32, 62, 66 };
static const char    S_CHARS_kuo[] = "括扩阔廓蛞适栝漷";
static const uint8_t S_COST_la[] = { 30, 37, 39, 40, 42, 43, 44, 46, 56, 57, 61, 61, 66, 67, 74, 74, 31, 37 };
static const char    S_CHARS_la[] = "拉啦腊辣喇蜡垃剌邋旯瘌砬镴蝲鞡𬶟落蓝";
static const uint8_t S_COST_lai[] = { 21, 38, 39, 47, 51, 52, 52, 53, 53, 55, 57, 57, 60, 68, 57, 60 };
static const char    S_CHARS_lai[] = "来莱赖睐涞籁癞梾崃徕濑赉铼俫黧釐";
static const uint8_t S_COST_lan[] = { 33, 37, 37, 39, 39, 40, 40, 43, 43, 44, 45, 45, 47, 47, 49, 49, 51, 53, 57, 58 };
static const char    S_CHARS_lan[] = "兰览蓝拦烂篮栏滥懒揽澜缆岚榄婪斓阑褴襕镧";
static const uint8_t S_COST_lang[] = { 37, 37, 37, 39, 39, 47, 47, 48, 52, 52, 55, 55, 56, 59, 60, 61, 68, 74, 74, 74 };
static const char    S_CHARS_lang[] = "朗郎浪廊狼琅埌啷螂榔阆莨锒烺蒗稂桹崀㫰蓢";
static const uint8_t S_COST_lao[] = { 27, 32, 40, 43, 45, 46, 47, 48, 50, 51, 51, 52, 54, 56, 59, 59, 64, 64, 65, 65 };
static const char    S_CHARS_lao[] = "老劳牢捞姥佬涝烙酪唠潦崂痨醪嫪铑栳耢荖铹";
static const uint8_t S_COST_le[] = { 19, 31, 47, 57, 63, 64, 65, 74, 74, 36, 57 };
static const char    S_CHARS_le[] = "了乐肋叻鳓泐饹仂簕勒嘞";
static const uint8_t S_COST_lei[] = { 29, 35, 36, 36, 37, 44, 46, 47, 48, 50, 51, 54, 56, 56, 56, 57, 57, 59, 62, 62 };
static const char    S_CHARS_lei[] = "类雷累勒泪垒蕾擂儡磊嫘镭诔缧耒羸嘞礌酹罍";
static const uint8_t S_COST_len[] = { 54 };
static const char    S_CHARS_len[] = "啉";
static const uint8_t S_COST_leng[] = { 34, 43, 44, 46, 51, 58, 68 };
static const char    S_CHARS_leng[] = "冷愣棱楞崚塄堎";
static const uint8_t S_COST_li[] = { 24, 25, 25, 27, 27, 29, 29, 31, 32, 33, 35, 37, 38, 38, 38, 39, 39, 39, 40, 40 };
static const char    S_CHARS_li[] = "里理力立利历李离例礼丽黎隶厉粒励吏厘璃哩";
static const uint8_t S_COST_lia[] = { 40 };
static const char    S_CHARS_lia[] = "俩";
static const uint8_t S_COST_lian[] = { 29, 30, 32, 33, 36, 38, 40, 40, 41, 41, 43, 44, 45, 47, 49, 50, 51, 51, 51, 53 };
static const char    S_CHARS_lian[] = "联连脸练莲炼链恋怜廉帘敛濂鲢涟镰楝琏殓奁";
static const uint8_t S_COST_liang[] = { 25, 27, 33, 34, 35, 36, 38, 38, 42, 47, 49, 49, 50, 56, 66, 69, 74, 74, 74, 74 };
static const char    S_CHARS_liang[] = "两量良亮粮梁辆凉谅粱踉椋晾魉俍墚𬜯悢𫟅辌";
static const uint8_t S_COST_liao[] = { 30, 35, 37, 42, 42, 45, 46, 47, 49, 49, 50, 50, 51, 52, 53, 54, 54, 57, 59, 59 };
static const char    S_CHARS_liao[] = "料疗辽聊僚寮廖撩缭寥撂燎瞭镣獠嘹蓼钌髎尥";
static const uint8_t S_COST_lie[] = { 31, 34, 37, 40, 41, 46, 53, 54, 54, 56, 60, 60, 64, 74, 74, 74, 32, 36, 43, 44 };
static const char    S_CHARS_lie[] = "列烈裂猎劣咧冽洌鬣趔埒捩躐䴕脟𫚭例累栗膊";
static const uint8_t S_COST_lin[] = { 29, 33, 38, 41, 41, 42, 42, 43, 45, 46, 48, 49, 49, 50, 50, 53, 54, 54, 54, 55 };
static const char    S_CHARS_lin[] = "林临邻磷霖淋鳞琳凛麟拎赁躏璘吝嶙遴廪啉蔺";
static const uint8_t S_COST_ling[] = { 28, 30, 32, 34, 34, 36, 37, 37, 39, 42, 43, 44, 47, 47, 48, 49, 50, 51, 52, 53 };
static const char    S_CHARS_ling[] = "领令另陵灵岭零龄凌菱铃玲伶羚棂苓绫聆翎呤";
static const uint8_t S_COST_liu[] = { 28, 31, 32, 32, 38, 41, 41, 42, 43, 44, 44, 48, 49, 53, 54, 55, 55, 58, 58, 58 };
static const char    S_CHARS_liu[] = "流六留刘柳瘤溜硫琉浏榴馏镏绺遛鎏镠鹨旒熘";
static const uint8_t S_COST_lo[] = { 47 };
static const char    S_CHARS_lo[] = "咯";
static const uint8_t S_COST_long[] = { 31, 36, 40, 43, 44, 47, 47, 47, 48, 48, 49, 53, 54, 55, 57, 60, 61, 74, 74, 74 };
static const char    S_CHARS_long[] = "龙隆笼垄拢聋陇胧珑咙窿砻茏眬栊泷癃昽哢漋";
static const uint8_t S_COST_lou[] = { 32, 40, 43, 44, 45, 48, 48, 50, 50, 51, 52, 55, 55, 59, 61, 61, 65, 74, 74, 34 };
static const char    S_CHARS_lou[] = "楼漏剅搂陋镂喽娄篓髅偻瘘蝼嵝耧蒌溇䁖𪣻露";
static const uint8_t S_COST_lu[] = { 27, 32, 34, 34, 35, 37, 39, 39, 41, 43, 43, 43, 43, 43, 44, 45, 47, 47, 47, 47 };
static const char    S_CHARS_lu[] = "路陆录露鲁鹿卢炉芦禄虏麓庐卤胪碌颅赂戮璐";
static const uint8_t S_COST_luan[] = { 33, 39, 49, 49, 50, 51, 51, 53, 53, 56, 61, 74 };
static const char    S_CHARS_luan[] = "乱卵鸾挛峦滦銮娈孪栾脔脟";
static const uint8_t S_COST_lun[] = { 29, 34, 36, 43, 43, 49, 50, 55, 74, 74 };
static const char    S_CHARS_lun[] = "论轮伦仑沦纶抡囵𫭢𬬭";
static const uint8_t S_COST_luo[] = { 31, 31, 35, 36, 40, 42, 42, 44, 44, 44, 45, 45, 50, 51, 52, 52, 52, 54, 56, 59 };
static const char    S_CHARS_luo[] = "罗落络洛逻螺珞萝裸锣骡骆椤啰箩雒摞漯荦瘰";
static const uint8_t S_COST_lv[] = { 31, 31, 32, 35, 36, 40, 41, 43, 43, 43, 43, 45, 46, 46, 48, 50, 51, 54, 54, 57 };
static const char    S_CHARS_lv[] = "律率旅绿虑吕履氯屡铝驴滤侣缕捋榈闾膂褛垏";
static const uint8_t S_COST_lve[] = { 32, 41, 57, 74, 74, 31 };
static const char    S_CHARS_lve[] = "略掠锊䂮圙率";
static const uint8_t S_COST_m[] = { 67, 49, 57 };
static const char    S_CHARS_m[] = "呣唔呒";
static const uint8_t S_COST_ma[] = { 28, 34, 35, 35, 36, 37, 40, 40, 41, 47, 48, 51, 59, 66, 66, 74, 25, 37, 41, 46 };
static const char    S_CHARS_ma[] = "马吗麻妈码骂嘛玛杩蚂蟆嬷犸孖祃唛么摩抹靡";
static const uint8_t S_COST_mai[] = { 34, 34, 36, 37, 39, 41, 52, 60, 66, 74, 31, 40, 48, 74 };
static const char    S_CHARS_mai[] = "买卖脉麦埋迈霾劢荬鿏派哩咪唛";
static const uint8_t S_COST_man[] = { 31, 36, 38, 38, 41, 42, 46, 47, 50, 50, 50, 51, 57, 58, 59, 62, 64, 64, 67, 74 };
static const char    S_CHARS_man[] = "满慢曼漫蛮瞒蔓馒幔谩螨鳗缦鞔嫚颟鬘墁镘𬜬";
static const uint8_t S_COST_mang[] = { 34, 41, 42, 42, 45, 47, 49, 57, 62, 62, 63, 63, 66, 69, 47, 58, 66 };
static const char    S_CHARS_mang[] = "忙芒盲茫莽氓蟒邙牻杧硭尨牤漭朦厖瞢";
static const uint8_t S_COST_mao[] = { 32, 36, 37, 37, 38, 40, 41, 41, 41, 47, 48, 48, 51, 51, 51, 53, 54, 54, 55, 57 };
static const char    S_CHARS_mao[] = "毛贸冒貌矛茅帽猫茂卯髦锚瑁峁牦铆懋耄袤楙";
static const uint8_t S_COST_me[] = { 25, 26, 35, 49 };
static const char    S_CHARS_me[] = "么没末麽";
static const uint8_t S_COST_mei[] = { 26, 27, 29, 36, 37, 37, 38, 38, 39, 42, 43, 44, 45, 45, 45, 45, 46, 47, 50, 52 };
static const char    S_CHARS_mei[] = "没美每梅妹媒煤眉枚霉酶媚魅嵋玫昧浼镁寐楣";
static const uint8_t S_COST_men[] = { 23, 24, 41, 45, 53, 54, 62, 74, 48, 57, 58, 60, 74 };
static const char    S_CHARS_men[] = "们门闷焖懑扪钔𫞩汶亹鞔惛呇";
static const uint8_t S_COST_meng[] = { 34, 36, 36, 37, 40, 45, 45, 47, 49, 51, 51, 55, 56, 59, 59, 60, 63, 63, 63, 66 };
static const char    S_CHARS_meng[] = "蒙盟猛梦孟锰萌朦檬懵虻蠓勐艋艨蜢獴甍礞瞢";
static const uint8_t S_COST_mi[] = { 28, 32, 34, 37, 40, 42, 42, 43, 44, 44, 46, 47, 47, 48, 48, 48, 51, 52, 53, 53 };
static const char    S_CHARS_mi[] = "米密秘迷弥泌蜜麋觅谜靡眯醚縻咪糜猕幂谧宓";
static const uint8_t S_COST_mian[] = { 24, 33, 37, 41, 41, 42, 43, 44, 47, 50, 52, 53, 57, 61, 64, 65, 74, 74, 74, 46 };
static const char    S_CHARS_mian[] = "面免棉眠绵沔勉缅冕娩腼湎渑勔眄偭丏愐𩾃冥";
static const uint8_t S_COST_miao[] = { 35, 37, 38, 38, 39, 44, 48, 48, 50, 52, 53, 55, 60, 60, 63, 41, 42, 50, 57 };
static const char    S_CHARS_miao[] = "庙苗妙描秒瞄渺喵缈藐淼邈眇鹋杪猫吵缪蜱";
static const uint8_t S_COST_mie[] = { 36, 47, 54, 56, 57, 48 };
static const char    S_CHARS_mie[] = "灭蔑篾乜咩咪";
static const uint8_t S_COST_min[] = { 23, 37, 44, 48, 49, 49, 50, 50, 53, 55, 56, 57, 58, 61, 64, 64, 66, 74, 74, 41 };
static const char    S_CHARS_min[] = "民敏闽闵悯抿皿岷泯黾缗愍旻湣珉忞鳘苠碈眠";
static const uint8_t S_COST_ming[] = { 26, 26, 28, 39, 41, 46, 49, 51, 52, 54, 57, 59, 60, 69, 36, 45, 50 };
static const char    S_CHARS_ming[] = "明名命鸣铭冥洺茗瞑螟溟酩暝蓂盟萌皿";
static const uint8_t S_COST_miu[] = { 46, 50 };
static const char    S_CHARS_miu[] = "谬缪";
static const uint8_t S_COST_mo[] = { 32, 35, 35, 37, 37, 37, 38, 38, 38, 39, 39, 41, 44, 45, 46, 46, 47, 47, 48, 49 };
static const char    S_CHARS_mo[] = "模末莫摩默墨摸磨魔膜漠抹沫陌摹蓦寞蘑谟馍";
static const uint8_t S_COST_mou[] = { 34, 36, 46, 50, 50, 53, 57, 58, 62, 29, 47, 52, 55, 67 };
static const char    S_CHARS_mou[] = "某谋牟缪眸鍪蛑侔哞件毋婺袤呣";
static const uint8_t S_COST_mu[] = { 28, 31, 32, 35, 37, 38, 38, 38, 39, 39, 43, 44, 44, 44, 45, 46, 50, 56, 57, 67 };
static const char    S_CHARS_mu[] = "目木母墓亩幕牧姆慕穆募暮沐牡拇睦钼苜仫毪";
static const uint8_t S_COST_n[] = { 43, 48, 49, 59, 60 };
static const char    S_CHARS_n[] = "嗯哽唔哏唵";
static const uint8_t S_COST_na[] = { 25, 32, 33, 34, 40, 43, 44, 49, 49, 59, 60, 66, 74, 26, 26, 46, 55, 59 };
static const char    S_CHARS_na[] = "那拿哪纳娜钠呐捺衲肭镎乸𦰡南内絮呶箬";
static const uint8_t S_COST_nai[] = { 37, 38, 39, 40, 48, 51, 52, 53, 63, 64, 74, 24, 25, 33, 74 };
static const char    S_CHARS_nai[] = "乃奶耐奈氖鼐艿萘柰迺耏能那哪佴";
static const uint8_t S_COST_nan[] = { 26, 29, 33, 45, 45, 57, 59, 60, 65, 69, 74, 48, 59, 61 };
static const char    S_CHARS_nan[] = "南难男楠喃赧腩囡蝻婻萳冉罱弇";
static const uint8_t S_COST_nang[] = { 40, 54, 54, 61, 61, 69, 63, 63 };
static const char    S_CHARS_nang[] = "囊囔馕攮曩齉瀼蘘";
static const uint8_t S_COST_nao[] = { 34, 37, 40, 44, 44, 49, 55, 56, 56, 56, 57, 58, 59, 64, 67 };
static const char    S_CHARS_nao[] = "脑闹恼挠瑙淖呶孬铙蛲猱臑硇峱垴";
static const uint8_t S_COST_ne[] = { 33, 50, 25, 33, 44, 58 };
static const char    S_CHARS_ne[] = "呢讷那哪呐疔";
static const uint8_t S_COST_nei[] = { 26, 52, 25, 33 };
static const char    S_CHARS_nei[] = "内馁那哪";
static const uint8_t S_COST_nen[] = { 40, 50, 58, 59, 74 };
static const char    S_CHARS_nen[] = "嫩恁臑枘媆";
static const uint8_t S_COST_neng[] = { 24, 24, 39 };
static const char    S_CHARS_neng[] = "能而耐";
static const uint8_t S_COST_ng[] = { 43, 48, 49, 60 };
static const char    S_CHARS_ng[] = "嗯哽唔唵";
static const uint8_t S_COST_ni[] = { 24, 33, 36, 38, 40, 44, 45, 46, 47, 48, 49, 50, 51, 52, 54, 54, 55, 56, 57, 61 };
static const char    S_CHARS_ni[] = "你尼泥拟逆腻倪妮匿溺霓鲵昵睨怩铌旎猊伲坭";
static const uint8_t S_COST_nian[] = { 21, 32, 45, 46, 47, 47, 48, 48, 49, 49, 52, 53, 63, 40, 41 };
static const char    S_CHARS_nian[] = "年念拈碾捻辇黏辗鲇撵廿蔫埝粘趁";
static const uint8_t S_COST_niang[] = { 33, 43 };
static const char    S_CHARS_niang[] = "娘酿";
static const uint8_t S_COST_niao[] = { 37, 40, 51, 55, 65, 48, 59 };
static const char    S_CHARS_niao[] = "鸟尿袅脲茑溺尥";
static const uint8_t S_COST_nie[] = { 43, 45, 45, 46, 46, 51, 52, 52, 55, 55, 56, 57, 63, 69, 69, 74, 74, 33, 36, 36 };
static const char    S_CHARS_nie[] = "捏涅聂孽镍蹑啮嗫颞蘖镊臬陧菍糵嵲𫔶哪泥幸";
static const uint8_t S_COST_nin[] = { 35, 50 };
static const char    S_CHARS_nin[] = "您恁";
static const uint8_t S_COST_ning[] = { 33, 38, 47, 48, 50, 50, 51, 53, 53, 66, 74, 21, 35, 36, 36, 47 };
static const char    S_CHARS_ning[] = "宁凝拧甯柠狞佞咛泞聍苧年疑冰泥攘";
static const uint8_t S_COST_niu[] = { 34, 39, 40, 49, 50, 54, 66, 47, 53, 66 };
static const char    S_CHARS_niu[] = "牛纽扭妞钮忸狃拗蚴杻";
static const uint8_t S_COST_nong[] = { 29, 37, 37, 46, 50, 55, 58, 74, 50 };
static const char    S_CHARS_nong[] = "农弄浓脓哝侬秾𬪩咔";
static const uint8_t S_COST_nou[] = { 60, 50 };
static const char    S_CHARS_nou[] = "耨嬬";
static const uint8_t S_COST_nu[] = { 35, 35, 37, 47, 57, 60, 60, 61, 68, 68, 32, 49, 55, 56, 59 };
static const char    S_CHARS_nu[] = "怒奴努弩驽胬孥傉砮笯仅褥呶帑肭";
static const uint8_t S_COST_nuan[] = { 36, 47, 52, 58 };
static const char    S_CHARS_nuan[] = "暖濡暧臑";
static const uint8_t S_COST_nun[] = { 74 };
static const char    S_CHARS_nun[] = "媆";
static const uint8_t S_COST_nuo[] = { 37, 43, 44, 48, 50, 54, 54, 59, 25, 31, 33, 35, 40, 44, 47, 74, 74 };
static const char    S_CHARS_nuo[] = "诺挪糯懦喏搦傩锘那需哪掉娜呐濡𦰡堧";
static const uint8_t S_COST_nv[] = { 28, 57, 59, 66, 46, 60, 66 };
static const char    S_CHARS_nv[] = "女衄钕恧絮胬狃";
static const uint8_t S_COST_nve[] = { 46, 50 };
static const char    S_CHARS_nve[] = "虐疟";
static const uint8_t S_COST_o[] = { 41, 47, 54, 74 };
static const char    S_CHARS_o[] = "哦噢喔嚄";
static const uint8_t S_COST_ou[] = { 33, 39, 40, 45, 49, 50, 52, 52, 54, 54, 55, 74, 74, 74, 25, 34, 36, 50, 63 };
static const char    S_CHARS_ou[] = "欧偶藕呕殴鸥瓯耦讴沤怄𬉼𠙶𫭟区遇握渥吽";
static const uint8_t S_COST_pa[] = { 33, 39, 41, 46, 46, 47, 54, 56, 57, 59, 68, 27, 31, 33, 45, 46, 46, 47, 50 };
static const char    S_CHARS_pa[] = "怕爬帕啪趴琶杷葩舥潖筢把派吧扒叭芭耙钯";
static const uint8_t S_COST_pai[] = { 31, 32, 34, 36, 48, 50, 54, 56, 60, 62, 36, 41, 43, 74 };
static const char    S_CHARS_pai[] = "派排牌拍徘湃俳哌蒎簰迫脾啡椑";
static const uint8_t S_COST_pan[] = { 34, 34, 40, 41, 42, 43, 44, 47, 49, 52, 53, 54, 59, 60, 60, 67, 30, 31, 31, 35 };
static const char    S_CHARS_pan[] = "判盘叛盼攀潘畔蟠磐槃蹒爿袢襻泮磻半片般繁";
static const uint8_t S_COST_pang[] = { 35, 38, 41, 47, 51, 52, 53, 57, 58, 61, 63, 69, 24, 31, 38, 40, 40, 41, 42, 44 };
static const char    S_CHARS_pang[] = "旁胖庞乓彷螃滂耪厖逄鳑雱方房仿榜彭逢膀傍";
static const uint8_t S_COST_pao[] = { 34, 35, 39, 40, 41, 47, 51, 51, 51, 54, 58, 67, 30, 36, 36, 49, 58, 61, 64, 74 };
static const char    S_CHARS_pao[] = "炮跑泡抛袍刨咆庖疱狍匏脬包抱胞苞趵摽藨脟";
static const uint8_t S_COST_pei[] = { 33, 35, 39, 39, 41, 44, 45, 46, 48, 51, 51, 56, 57, 63, 66, 74, 74, 35, 37, 39 };
static const char    S_CHARS_pei[] = "配培陪佩赔胚沛裴呸辔霈旆帔锫醅𬇙衃坏倍妃";
static const uint8_t S_COST_pen[] = { 38, 40, 58, 41, 48 };
static const char    S_CHARS_pen[] = "盆喷湓吩汾";
static const uint8_t S_COST_peng[] = { 36, 39, 40, 41, 41, 41, 42, 42, 43, 45, 45, 48, 48, 49, 49, 53, 55, 56, 59, 74 };
static const char    S_CHARS_peng[] = "朋碰彭捧蓬鹏棚膨烹篷砰澎怦抨硼嘭堋蟛弸椪";
static const uint8_t S_COST_pi[] = { 31, 33, 39, 39, 39, 41, 41, 42, 43, 45, 46, 46, 46, 46, 47, 48, 49, 50, 51, 51 };
static const char    S_CHARS_pi[] = "批皮匹披辟疲脾屁劈僻啤譬丕毗琵嚭霹坯纰癖";
static const uint8_t S_COST_pian[] = { 31, 36, 37, 40, 50, 50, 51, 58, 64, 64, 68, 74, 74, 26, 28, 40, 41, 50, 59, 68 };
static const char    S_CHARS_pian[] = "片偏篇骗谝骈翩胼犏蹁楩𡎚㛹平便辨扁蝙褊萹";
static const uint8_t S_COST_piao[] = { 34, 40, 41, 49, 50, 50, 51, 52, 53, 59, 63, 69, 41, 54, 55, 61, 61, 74 };
static const char    S_CHARS_piao[] = "票飘漂瓢嫖瞟缥剽嘌殍螵薸朴骠膘摽莩蔈";
static const uint8_t S_COST_pie[] = { 45, 45, 56, 66, 74, 43 };
static const char    S_CHARS_pie[] = "撇瞥苤氕𬭯蔽";
static const uint8_t S_COST_pin[] = { 27, 36, 37, 37, 40, 47, 48, 54, 55, 74, 74, 45, 45, 47 };
static const char    S_CHARS_pin[] = "品频贫聘拼嫔姘颦牝玭𬞟泵匕娉";
static const uint8_t S_COST_ping[] = { 26, 34, 37, 39, 40, 44, 44, 44, 47, 47, 53, 55, 59, 64, 66, 68, 74, 74, 74, 74 };
static const char    S_CHARS_ping[] = "平评凭瓶屏苹萍坪乒娉枰鲆俜洴帡淜泙玶荓涄";
static const uint8_t S_COST_po[] = { 32, 36, 36, 37, 38, 39, 42, 43, 48, 50, 51, 53, 56, 56, 63, 63, 66, 74, 74, 35 };
static const char    S_CHARS_po[] = "破迫婆坡泊颇泼魄珀鄱叵粕笸钋皤钷桲䥽酦繁";
static const uint8_t S_COST_pou[] = { 43, 62, 63, 64, 24, 35, 36, 51, 57, 60, 65, 67 };
static const char    S_CHARS_pou[] = "剖裒掊抔部培抱涪瓿棓踣垺";
static const uint8_t S_COST_pu[] = { 33, 36, 38, 39, 41, 41, 41, 41, 43, 43, 44, 44, 47, 48, 48, 48, 48, 49, 51, 51 };
static const char    S_CHARS_pu[] = "普铺扑谱葡仆朴浦蒲菩埔瀑濮噗曝圃脯溥璞莆";
static const uint8_t S_COST_qi[] = { 25, 25, 27, 27, 30, 31, 32, 33, 34, 35, 35, 36, 37, 37, 37, 39, 39, 40, 40, 41 };
static const char    S_CHARS_qi[] = "其起期气器企七奇齐旗汽骑妻启弃棋岂欺漆契";
static const uint8_t S_COST_qia[] = { 39, 46, 48, 57, 61, 63, 31, 35, 53, 54, 59, 60, 66 };
static const char    S_CHARS_qia[] = "恰洽掐髂拤葜客卡袷挈疴絜矻";
static const uint8_t S_COST_qian[] = { 25, 30, 32, 35, 36, 37, 39, 39, 39, 39, 41, 41, 42, 44, 44, 45, 45, 46, 46, 47 };
static const char    S_CHARS_qian[] = "前千钱潜签迁浅牵乾遣谦欠铅嵌歉谴黔褰骞搴";
static const uint8_t S_COST_qiang[] = { 29, 35, 35, 37, 39, 45, 47, 48, 49, 50, 52, 55, 55, 55, 56, 56, 58, 60, 61, 62 };
static const char    S_CHARS_qiang[] = "强枪墙抢腔羌呛羟跄蔷锵戗戕襁炝樯锖嫱蜣镪";
static const uint8_t S_COST_qiao[] = { 32, 36, 37, 40, 40, 41, 41, 45, 45, 46, 46, 46, 47, 48, 49, 50, 51, 52, 52, 52 };
static const char    S_CHARS_qiao[] = "桥瞧巧乔侨悄敲鞘窍峭翘俏撬跷憔樵硚锹荞橇";
static const uint8_t S_COST_qie[] = { 31, 32, 43, 44, 44, 51, 54, 55, 55, 61, 69, 39, 40, 41, 43, 45, 46, 52, 53, 53 };
static const char    S_CHARS_qie[] = "且切窃怯妾惬挈锲箧郄癿捷漆契砌伽茄沏喋婕";
static const uint8_t S_COST_qin[] = { 30, 35, 36, 38, 38, 41, 42, 43, 44, 46, 50, 52, 53, 54, 55, 55, 57, 59, 61, 62 };
static const char    S_CHARS_qin[] = "亲侵秦勤琴钦擒禽寝沁芹噙嗪衾揿芩锓吣骎檎";
static const uint8_t S_COST_qing[] = { 27, 28, 31, 31, 32, 34, 37, 38, 41, 42, 43, 47, 50, 51, 52, 53, 54, 58, 59, 62 };
static const char    S_CHARS_qing[] = "情清青请轻庆倾顷卿氢晴擎氰箐罄蜻磬黥鲭檠";
static const uint8_t S_COST_qiong[] = { 37, 44, 50, 54, 54, 55, 58, 59, 60, 74, 49, 62 };
static const char    S_CHARS_qiong[] = "穷琼穹邛蛩芎筇銎茕䓖鞠嬛";
static const uint8_t S_COST_qiu[] = { 29, 31, 35, 38, 44, 44, 46, 47, 49, 49, 51, 51, 54, 55, 57, 57, 58, 63, 63, 64 };
static const char    S_CHARS_qiu[] = "求球秋丘裘囚邱酋蚯鳅俅虬楸遒逑泅巯糗犰璆";
static const uint8_t S_COST_qu[] = { 25, 25, 29, 34, 37, 38, 38, 39, 40, 42, 43, 44, 46, 47, 50, 50, 51, 52, 55, 55 };
static const char    S_CHARS_qu[] = "区去取曲趣趋屈驱渠娶躯衢觑瞿祛龋岖蛆蛐劬";
static const uint8_t S_COST_quan[] = { 24, 28, 37, 37, 38, 38, 38, 44, 46, 48, 50, 50, 50, 51, 52, 53, 53, 55, 58, 59 };
static const char    S_CHARS_quan[] = "全权泉圈券拳劝犬醛佺痊荃诠蜷绻颧铨棬悛鬈";
static const uint8_t S_COST_que[] = { 29, 31, 35, 42, 42, 47, 50, 52, 53, 55, 65, 65, 74, 38, 40, 51, 51, 54, 56, 61 };
static const char    S_CHARS_que[] = "却确缺雀阙鹊榷瘸埆阕碏悫𬒈屈猎芍炔傕觳攉";
static const uint8_t S_COST_qun[] = { 30, 43, 54, 58, 44, 48, 59 };
static const char    S_CHARS_qun[] = "群裙囷逡蹲遁麇";
static const uint8_t S_COST_ran[] = { 25, 36, 38, 48, 49, 57, 61, 74 };
static const char    S_CHARS_ran[] = "然染燃冉髯苒蚺䎃";
static const uint8_t S_COST_rang[] = { 30, 39, 44, 47, 49, 54, 56, 57, 63, 63 };
static const char    S_CHARS_rang[] = "让壤嚷攘儴瓤禳穰瀼蘘";
static const uint8_t S_COST_rao[] = { 37, 39, 42, 49, 54, 63 };
static const char    S_CHARS_rao[] = "绕扰饶娆桡荛";
static const uint8_t S_COST_re[] = { 30, 42, 32, 50, 50 };
static const char    S_CHARS_re[] = "热惹若喏偌";
static const uint8_t S_COST_ren[] = { 18, 27, 29, 37, 37, 40, 46, 48, 48, 48, 49, 51, 55, 56, 56, 57, 63, 74, 74, 28 };
static const char    S_CHARS_ren[] = "人任认忍仁刃韧妊仞壬饪纫稔衽荏轫葚纴讱儿";
static const uint8_t S_COST_reng[] = { 33, 42, 35, 46, 52, 57 };
static const char    S_CHARS_reng[] = "仍扔耳戎艿穰";
static const uint8_t S_COST_ri[] = { 24, 69 };
static const char    S_CHARS_ri[] = "日驲";
static const uint8_t S_COST_rong[] = { 30, 35, 36, 39, 41, 42, 45, 46, 47, 49, 51, 53, 54, 58, 63, 74, 37 };
static const char    S_CHARS_rong[] = "容荣融溶蓉熔绒戎茸榕冗镕嵘蝾狨瑢隔";
static const uint8_t S_COST_rou[] = { 34, 39, 45, 49, 52, 56, 74 };
static const char    S_CHARS_rou[] = "肉柔揉蹂鞣糅𫐓";
static const uint8_t S_COST_ru[] = { 25, 27, 39, 40, 40, 41, 47, 48, 49, 49, 50, 51, 52, 55, 57, 58, 59, 60, 62, 62 };
static const char    S_CHARS_ru[] = "如入乳辱汝儒濡蠕孺褥嬬茹嚅缛铷洳薷襦溽颥";
static const uint8_t S_COST_ruan[] = { 35, 43, 74, 74, 74, 31, 47 };
static const char    S_CHARS_ruan[] = "软阮堧媆瓀需濡";
static const uint8_t S_COST_rui[] = { 37, 41, 45, 47, 55, 57, 59, 60, 64, 26, 41, 58, 59 };
static const char    S_CHARS_rui[] = "瑞锐睿蕊芮蚋枘蕤汭内兑踒棁";
static const uint8_t S_COST_run[] = { 36, 51 };
static const char    S_CHARS_run[] = "润闰";
static const uint8_t S_COST_ruo[] = { 32, 36, 50, 54, 59, 66, 68, 42, 48, 55, 60 };
static const char    S_CHARS_ruo[] = "若弱偌鄀箬爇蒻惹溺芮婼";
static const uint8_t S_COST_sa[] = { 36, 39, 43, 46, 51, 52, 55, 56, 66, 67, 41, 56, 61, 66 };
static const char    S_CHARS_sa[] = "萨撒洒潵卅飒仨挲靸脎蔡趿檫霅";
static const uint8_t S_COST_sai[] = { 32, 36, 47, 48, 55, 30 };
static const char    S_CHARS_sai[] = "赛塞腮鳃噻思";
static const uint8_t S_COST_san[] = { 25, 34, 42, 57, 57, 60, 61, 46, 54 };
static const char    S_CHARS_san[] = "三散伞糁馓叁毵潵霰";
static const uint8_t S_COST_sang[] = { 39, 40, 44, 54, 62, 68 };
static const char    S_CHARS_sang[] = "丧桑嗓搡颡磉";
static const uint8_t S_COST_sao[] = { 38, 42, 43, 48, 51, 52, 54, 54, 62, 41, 43, 44, 69 };
static const char    S_CHARS_sao[] = "扫嫂骚搔臊缫埽瘙溞哨燥梢缲";
static const uint8_t S_COST_se[] = { 28, 44, 46, 52, 53, 54, 55, 74, 36, 38, 44, 54, 74, 74 };
static const char    S_CHARS_se[] = "色瑟涩啬洓穑铯璱塞寨泣槭㴔溹";
static const uint8_t S_COST_sen[] = { 36, 43 };
static const char    S_CHARS_sen[] = "森洒";
static const uint8_t S_COST_seng[] = { 37 };
static const char    S_CHARS_seng[] = "僧";
static const uint8_t S_COST_sha[] = { 32, 32, 40, 40, 41, 41, 42, 43, 44, 45, 46, 46, 50, 53, 54, 58, 65, 28, 35, 42 };
static const char    S_CHARS_sha[] = "杀沙砂啥傻纱厦刹莎煞霎鲨裟铩痧歃唼接哈杉";
static const uint8_t S_COST_shai[] = { 42, 46, 64, 28 };
static const char    S_CHARS_shai[] = "晒筛酾色";
static const uint8_t S_COST_shan[] = { 26, 33, 37, 38, 39, 41, 41, 41, 42, 44, 44, 46, 47, 49, 49, 50, 50, 50, 51, 51 };
static const char    S_CHARS_shan[] = "山善闪陕扇珊衫擅杉膳缮删煽汕鳝讪赡姗蟮苫";
static const uint8_t S_COST_shang[] = { 21, 29, 32, 33, 37, 43, 47, 51, 52, 52, 56, 57, 65, 37, 74 };
static const char    S_CHARS_shang[] = "上商伤尚赏晌裳绱熵墒觞殇垧汤埫";
static const uint8_t S_COST_shao[] = { 28, 35, 36, 38, 41, 44, 44, 45, 47, 47, 49, 51, 52, 58, 59, 61, 62, 67, 74, 74 };
static const char    S_CHARS_shao[] = "少烧绍稍哨梢邵苕勺捎韶芍艄劭蛸筲潲睄玿柖";
static const uint8_t S_COST_she[] = { 27, 28, 32, 37, 37, 38, 39, 40, 43, 44, 47, 48, 49, 53, 53, 54, 54, 56, 58, 65 };
static const char    S_CHARS_she[] = "设社射涉舍摄蛇舌赦奢慑麝畲赊厍歙畬佘猞滠";
static const uint8_t S_COST_shei[] = { 33 };
static const char    S_CHARS_shei[] = "谁";
static const uint8_t S_COST_shen[] = { 27, 29, 29, 30, 31, 32, 35, 37, 39, 41, 41, 42, 42, 43, 45, 47, 48, 50, 51, 52 };
static const char    S_CHARS_shen[] = "身什神深甚审伸申沈肾慎屾渗婶绅呻娠砷椹哂";
static const uint8_t S_COST_sheng[] = { 22, 27, 27, 32, 32, 35, 35, 37, 40, 40, 47, 49, 52, 54, 65, 66, 34, 36, 42, 43 };
static const char    S_CHARS_sheng[] = "生省声胜升盛圣剩牲绳笙甥昇嵊眚陞姓乘丞甸";
static const uint8_t S_COST_shi[] = { 18, 22, 24, 25, 25, 26, 27, 27, 27, 28, 28, 29, 30, 30, 30, 30, 31, 31, 31, 31 };
static const char    S_CHARS_shi[] = "是时市事实十使石世师式史始士示势施失视食";
static const uint8_t S_COST_shou[] = { 26, 28, 29, 29, 33, 34, 35, 38, 39, 40, 47, 50, 58, 35 };
static const char    S_CHARS_shou[] = "手首受收守授售寿兽瘦狩绶艏熟";
static const uint8_t S_COST_shu[] = { 27, 28, 28, 30, 33, 34, 34, 35, 35, 37, 37, 38, 39, 40, 40, 41, 41, 42, 42, 44 };
static const char    S_CHARS_shu[] = "数书术属树输述熟束署殊叔疏舒枢鼠蜀蔬竖淑";
static const uint8_t S_COST_shua[] = { 39, 43, 53, 48, 53 };
static const char    S_CHARS_shua[] = "刷耍唰唆涮";
static const uint8_t S_COST_shuai[] = { 39, 39, 41, 44, 51, 31 };
static const char    S_CHARS_shuai[] = "衰帅摔甩蟀率";
static const uint8_t S_COST_shuan[] = { 47, 47, 50, 53, 74, 49, 51 };
static const char    S_CHARS_shuan[] = "拴栓闩涮腨汕踹";
static const uint8_t S_COST_shuang[] = { 31, 42, 42, 53, 65, 74, 74, 49, 60, 67 };
static const char    S_CHARS_shuang[] = "双霜爽孀骦礵鹴淙泷漴";
static const uint8_t S_COST_shui[] = { 26, 33, 36, 36, 69, 23 };
static const char    S_CHARS_shui[] = "水谁睡税帨说";
static const uint8_t S_COST_shun[] = { 34, 42, 46, 50, 38, 38, 39, 58, 60 };
static const char    S_CHARS_shun[] = "顺瞬舜吮盾巡俊恂楯";
static const uint8_t S_COST_shuo[] = { 23, 41, 46, 46, 52, 52, 54, 55, 60, 27, 44, 45, 47, 53, 55, 56, 68 };
static const char    S_CHARS_shuo[] = "说硕朔烁蒴搠铄槊妁数嗽溯勺杓濯嗍汋";
static const uint8_t S_COST_si[] = { 28, 28, 29, 29, 30, 35, 35, 36, 41, 42, 42, 42, 42, 43, 46, 47, 48, 49, 49, 54 };
static const char    S_CHARS_si[] = "四司斯死思私丝寺祀肆饲嗣厮撕嘶涘驷泗巳咝";
static const uint8_t S_COST_song[] = { 32, 34, 34, 42, 43, 44, 44, 46, 48, 50, 50, 52, 54, 54, 58, 60, 67, 74 };
static const char    S_CHARS_song[] = "送宋松讼颂诵耸嵩淞怂悚竦崧凇忪菘娀㧐";
static const uint8_t S_COST_sou[] = { 36, 39, 44, 51, 51, 52, 53, 54, 55, 58, 60, 60, 61, 64, 65, 68, 28, 46, 53 };
static const char    S_CHARS_sou[] = "搜艘嗽擞嗖飕叟馊薮溲嗾蒐锼廋螋瞍族敕涑";
static const uint8_t S_COST_su[] = { 31, 31, 32, 34, 36, 38, 39, 39, 44, 45, 45, 48, 51, 52, 53, 54, 54, 56, 56, 58 };
static const char    S_CHARS_su[] = "速素苏诉俗肃宿塑粟溯酥稣簌夙涑谡僳愫窣嗉";
static const uint8_t S_COST_suan[] = { 30, 35, 46, 52, 42 };
static const char    S_CHARS_suan[] = "算酸蒜狻撰";
static const uint8_t S_COST_sui[] = { 30, 31, 33, 38, 39, 41, 44, 45, 47, 47, 49, 51, 52, 52, 53, 57, 58, 63, 74, 74 };
static const char    S_CHARS_sui[] = "随虽岁碎遂隋髓隧穗绥祟邃睢燧濉谇荽眭葰璲";
static const uint8_t S_COST_sun[] = { 33, 36, 48, 50, 52, 52, 52, 53, 38, 56, 69 };
static const char    S_CHARS_sun[] = "孙损笋荪隼狲飧榫餐跣栒";
static const uint8_t S_COST_suo[] = { 25, 34, 36, 39, 45, 45, 46, 47, 48, 51, 53, 54, 56, 56, 60, 74, 26, 32, 39, 40 };
static const char    S_CHARS_suo[] = "所索缩锁梭琐娑嗦唆羧唢蓑嗍睃桫溹些沙衰霍";
static const uint8_t S_COST_ta[] = { 21, 26, 28, 36, 39, 43, 46, 50, 51, 51, 54, 56, 56, 57, 57, 58, 66, 74, 74, 74 };
static const char    S_CHARS_ta[] = "他她它塔踏塌榻蹋獭鳎挞遢趿闼铊褟鞳溻鿎溚";
static const uint8_t S_COST_tai[] = { 28, 29, 32, 37, 37, 40, 45, 45, 46, 48, 51, 55, 56, 58, 59, 59, 19, 24, 60, 60 };
static const char    S_CHARS_tai[] = "太台态泰抬胎汰苔钛肽邰薹跆酞鲐炱大能呔釐";
static const uint8_t S_COST_tan[] = { 33, 35, 35, 36, 37, 40, 40, 40, 41, 41, 42, 42, 44, 45, 45, 46, 49, 50, 50, 52 };
static const char    S_CHARS_tan[] = "谈探坦坛叹滩碳贪炭摊潭谭毯痰檀瘫昙袒坍忐";
static const uint8_t S_COST_tang[] = { 33, 34, 37, 37, 38, 41, 41, 43, 44, 44, 46, 48, 51, 52, 52, 53, 54, 55, 56, 56 };
static const char    S_CHARS_tang[] = "堂唐糖汤倘躺塘趟烫膛棠淌搪镗蹚羰螳傥帑瑭";
static const uint8_t S_COST_tao[] = { 35, 35, 35, 36, 38, 41, 42, 42, 43, 46, 48, 51, 51, 52, 53, 57, 58, 65, 68, 69 };
static const char    S_CHARS_tao[] = "讨套桃逃陶萄涛掏淘韬滔绦饕洮啕鼗弢绹梼慆";
static const uint8_t S_COST_te[] = { 27, 48, 52, 62, 66, 74, 28, 47 };
static const char    S_CHARS_te[] = "特忒忑慝铽螣式匿";
static const uint8_t S_COST_tei[] = { 48 };
static const char    S_CHARS_tei[] = "忒";
static const uint8_t S_COST_teng[] = { 38, 39, 43, 48, 51, 66, 67, 74, 74 };
static const char    S_CHARS_teng[] = "腾疼藤滕誊縢熥䲢螣";
static const uint8_t S_COST_ti[] = { 25, 27, 28, 36, 40, 41, 43, 45, 46, 46, 47, 47, 48, 50, 52, 52, 53, 57, 58, 58 };
static const char    S_CHARS_ti[] = "体提题替梯踢蹄惕啼剃涕剔屉锑缇嚏倜悌绨擿";
static const uint8_t S_COST_tian[] = { 24, 33, 39, 39, 39, 50, 51, 51, 55, 55, 58, 59, 60, 65, 69, 74, 74, 74, 74, 34 };
static const char    S_CHARS_tian[] = "天田添填甜舔恬腆阗忝殄湉畋掭晪沺盷淟黇典";
static const uint8_t S_COST_tiao[] = { 28, 35, 37, 48, 51, 54, 54, 55, 55, 56, 58, 61, 63, 63, 64, 74, 30, 32, 35, 41 };
static const char    S_CHARS_tiao[] = "条跳挑眺迢祧笤窕龆粜佻髫朓蜩鲦嬥调超桃姚";
static const uint8_t S_COST_tie[] = { 31, 36, 42, 54, 55, 31, 45, 57 };
static const char    S_CHARS_tie[] = "铁贴帖萜餮占蝶怙";
static const uint8_t S_COST_ting[] = { 29, 34, 34, 35, 35, 38, 38, 39, 48, 48, 49, 49, 52, 53, 58, 61, 61, 62, 63, 64 };
static const char    S_CHARS_ting[] = "听停庭廷厅艇亭挺烃汀婷霆町蜓梃烶渟莛葶圢";
static const uint8_t S_COST_tong[] = { 25, 26, 28, 34, 36, 36, 41, 42, 43, 46, 47, 48, 48, 49, 49, 51, 51, 53, 57, 59 };
static const char    S_CHARS_tong[] = "同通统痛童铜筒桐桶酮潼捅佟瞳彤僮恸嗵鲖仝";
static const uint8_t S_COST_tou[] = { 26, 30, 36, 39, 49, 63, 44, 45, 74 };
static const char    S_CHARS_tou[] = "头投透偷骰钭愉逗褕";
static const uint8_t S_COST_tu[] = { 29, 30, 32, 36, 37, 38, 39, 42, 43, 43, 45, 50, 54, 56, 60, 61, 65, 68, 69, 69 };
static const char    S_CHARS_tu[] = "土图突途徒吐涂屠凸兔秃荼钍菟葖梌堍腯稌酴";
static const uint8_t S_COST_tuan[] = { 29, 48, 55, 57, 61, 74, 74, 36, 39, 46, 48 };
static const char    S_CHARS_tuan[] = "团湍疃抟彖猯煓税敦揣痪";
static const uint8_t S_COST_tui[] = { 31, 33, 37, 47, 48, 49, 60, 74, 74, 32, 34, 35, 36, 48, 64 };
static const char    S_CHARS_tui[] = "推退腿褪颓蜕煺𬯎魋弟追脱税忒焞";
static const uint8_t S_COST_tun[] = { 40, 42, 44, 47, 52, 58, 59, 64, 68, 74, 33, 34, 36, 39, 47, 47, 51, 51, 59, 62 };
static const char    S_CHARS_tun[] = "吞屯豚臀饨鲀暾焞忳坉吴逐吨敦炖褪沌囤肫窀";
static const uint8_t S_COST_tuo[] = { 35, 35, 39, 40, 41, 41, 43, 46, 46, 48, 48, 50, 52, 53, 53, 54, 55, 55, 56, 60 };
static const char    S_CHARS_tuo[] = "托脱拖拓妥陀驼椭唾驮庹沱鸵坨橐佗鼍跎砣侂";
static const uint8_t S_COST_wa[] = { 35, 39, 40, 44, 45, 45, 47, 48, 54, 58, 66, 74, 40, 45, 58, 60, 74 };
static const char    S_CHARS_wa[] = "瓦挖娃哇蛙洼袜娲佤畖腽窊鞋凹靺姽坬";
static const uint8_t S_COST_wai[] = { 25, 43, 55, 49 };
static const char    S_CHARS_wai[] = "外歪崴夭";
static const uint8_t S_COST_wan[] = { 27, 29, 33, 34, 37, 38, 39, 41, 42, 42, 43, 43, 43, 43, 45, 47, 47, 48, 48, 51 };
static const char    S_CHARS_wan[] = "万完晚湾玩碗弯顽宛挽腕丸皖婉烷蜿豌绾纨惋";
static const uint8_t S_COST_wang[] = { 28, 31, 31, 31, 35, 36, 40, 41, 42, 44, 50, 51, 57, 61, 61, 24, 29, 41, 47, 61 };
static const char    S_CHARS_wang[] = "王网望往亡忘汪旺妄枉惘罔魍辋尪方皇芒匡尢";
static const uint8_t S_COST_wei[] = { 21, 26, 27, 31, 31, 32, 32, 33, 33, 33, 35, 36, 36, 37, 37, 37, 37, 38, 40, 40 };
static const char    S_CHARS_wei[] = "为位委未维围卫味微威危韦谓尾唯伟魏违慰纬";
static const uint8_t S_COST_wen[] = { 25, 27, 32, 33, 35, 39, 41, 46, 46, 47, 48, 49, 49, 51, 53, 57, 61, 63, 64, 68 };
static const char    S_CHARS_wen[] = "文问闻温稳纹吻蚊紊璺汶瘟雯刎炆阌芠鳁玟榅";
static const uint8_t S_COST_weng[] = { 41, 46, 47, 48, 61, 62, 64, 74, 54 };
static const char    S_CHARS_weng[] = "翁滃瓮嗡蕹蓊鹟𬭩壅";
static const uint8_t S_COST_wo[] = { 21, 36, 40, 41, 41, 44, 45, 46, 50, 50, 50, 53, 53, 54, 55, 58, 58, 59, 74, 49 };
static const char    S_CHARS_wo[] = "我握窝卧沃涡斡倭挝渥蜗龌幄硪肟莴踒偓涴瘟";
static const uint8_t S_COST_wu[] = { 26, 26, 27, 27, 28, 33, 35, 35, 35, 35, 36, 37, 38, 39, 40, 40, 42, 43, 44, 44 };
static const char    S_CHARS_wu[] = "无物武务五吴午屋舞误乌伍污吾雾悟屼兀呜勿";
static const uint8_t S_COST_xi[] = { 26, 27, 31, 32, 32, 32, 32, 33, 34, 35, 35, 37, 38, 38, 38, 38, 39, 39, 39, 39 };
static const char    S_CHARS_xi[] = "西系息席习细喜希吸戏析洗袭稀悉惜锡熙溪浠";
static const uint8_t S_COST_xia[] = { 23, 33, 33, 37, 39, 40, 40, 41, 42, 42, 43, 45, 45, 49, 53, 53, 54, 58, 59, 67 };
static const char    S_CHARS_xia[] = "下夏辖峡吓瞎侠狭霞虾暇匣瑕遐黠罅狎硖柙翈";
static const uint8_t S_COST_xian[] = { 25, 27, 28, 29, 31, 32, 34, 34, 34, 35, 35, 37, 38, 38, 39, 39, 39, 40, 41, 41 };
static const char    S_CHARS_xian[] = "现先县线显限鲜险献仙宪陷闲贤咸纤弦腺衔嫌";
static const uint8_t S_COST_xiang[] = { 26, 26, 27, 30, 30, 31, 31, 32, 33, 34, 36, 37, 37, 39, 39, 41, 43, 43, 43, 45 };
static const char    S_CHARS_xiang[] = "相想向像象项响乡香襄享祥湘详箱巷厢翔橡镶";
static const uint8_t S_COST_xiao[] = { 25, 30, 30, 31, 32, 33, 36, 38, 41, 41, 43, 43, 45, 45, 46, 46, 47, 47, 47, 47 };
static const char    S_CHARS_xiao[] = "小笑校消效销孝晓萧肖啸硝潇宵逍嚣霄枭箫哮";
static const uint8_t S_COST_xie[] = { 26, 32, 32, 35, 36, 39, 39, 40, 40, 40, 40, 41, 42, 42, 44, 44, 45, 45, 45, 48 };
static const char    S_CHARS_xie[] = "些协写谢械斜胁鞋歇携邪泄谐卸蟹屑泻挟懈蝎";
static const uint8_t S_COST_xin[] = { 25, 25, 28, 37, 39, 40, 43, 43, 46, 48, 49, 50, 50, 52, 56, 66, 74, 74, 74, 35 };
static const char    S_CHARS_xin[] = "心新信辛薪欣芯锌馨衅昕歆鑫忻囟伈炘䜣𫷷款";
static const uint8_t S_COST_xing[] = { 23, 26, 28, 30, 31, 31, 34, 36, 36, 36, 43, 44, 44, 48, 50, 53, 53, 53, 54, 55 };
static const char    S_CHARS_xing[] = "行性形型兴星姓刑幸醒杏腥邢惺猩铏荥悻硎陉";
static const uint8_t S_COST_xiong[] = { 34, 34, 36, 39, 39, 42, 47, 51, 74, 74, 24, 35, 55, 61 };
static const char    S_CHARS_xiong[] = "兄雄胸熊凶匈汹夐讻诇能宪芎昫";
static const uint8_t S_COST_xiu[] = { 32, 35, 36, 39, 39, 42, 45, 47, 47, 49, 50, 52, 54, 56, 58, 58, 58, 59, 61, 63 };
static const char    S_CHARS_xiu[] = "修秀休袖绣羞朽嗅锈溴馐岫珛庥鸺咻琇髹脩貅";
static const uint8_t S_COST_xu[] = { 30, 31, 32, 32, 35, 35, 36, 37, 40, 41, 42, 43, 46, 46, 46, 46, 48, 48, 50, 50 };
static const char    S_CHARS_xu[] = "许需须续序虚徐绪叙蓄旭吁絮墟婿胥嘘恤戌煦";
static const uint8_t S_COST_xuan[] = { 29, 33, 37, 38, 39, 41, 42, 44, 47, 47, 48, 49, 49, 49, 50, 51, 51, 53, 55, 56 };
static const char    S_CHARS_xuan[] = "选宣旋悬玄轩璇喧眩漩炫绚渲萱煊玹暄癣谖铉";
static const uint8_t S_COST_xue[] = { 23, 32, 35, 38, 39, 43, 47, 49, 50, 53, 59, 74, 47, 51, 57, 59, 63 };
static const char    S_CHARS_xue[] = "学血雪穴削薛靴踅谑鳕茓峃哮炔噱嚯敩";
static const uint8_t S_COST_xun[] = { 34, 35, 36, 37, 37, 38, 39, 41, 42, 43, 45, 45, 45, 46, 46, 50, 50, 51, 52, 52 };
static const char    S_CHARS_xun[] = "训寻迅讯询巡循逊勋旬鲟驯汛熏殉荀埙薰徇巽";
static const uint8_t S_COST_ya[] = { 30, 32, 35, 37, 37, 40, 41, 41, 41, 42, 43, 43, 43, 43, 45, 46, 52, 52, 53, 54 };
static const char    S_CHARS_ya[] = "亚压牙雅呀衙押鸭丫崖哑鸦涯芽讶轧蚜娅桠垭";
static const uint8_t S_COST_yan[] = { 29, 29, 29, 32, 32, 32, 34, 35, 35, 35, 36, 37, 38, 38, 38, 40, 40, 41, 41, 42 };
static const char    S_CHARS_yan[] = "眼言研严演验沿岩烟延盐堰颜燕炎掩宴艳咽厌";
static const uint8_t S_COST_yang[] = { 27, 29, 31, 32, 32, 33, 36, 37, 38, 38, 44, 46, 49, 49, 50, 50, 50, 50, 51, 52 };
static const char    S_CHARS_yang[] = "样阳央杨养洋扬羊氧仰痒鸯疡秧佯殃漾恙炀鞅";
static const uint8_t S_COST_yao[] = { 22, 31, 36, 38, 39, 39, 40, 40, 41, 41, 41, 41, 42, 42, 45, 46, 47, 47, 49, 49 };
static const char    S_CHARS_yao[] = "要药摇腰咬邀妖耀遥瑶幺姚肴窑谣钥吆尧夭徭";
static const uint8_t S_COST_ye[] = { 23, 24, 33, 33, 34, 35, 35, 36, 39, 40, 47, 48, 48, 48, 48, 49, 49, 50, 54, 57 };
static const char    S_CHARS_ye[] = "也业夜叶爷野页液耶冶椰腋谒曳掖邺噎晔靥烨";
static const uint8_t S_COST_yi[] = { 18, 22, 26, 27, 27, 28, 31, 32, 32, 32, 32, 33, 33, 33, 33, 34, 34, 34, 35, 36 };
static const char    S_CHARS_yi[] = "一以已意义议易医依艺亿益衣宜异遗移伊疑仪";
static const uint8_t S_COST_yin[] = { 27, 31, 31, 32, 32, 35, 36, 37, 40, 41, 41, 42, 43, 44, 47, 47, 47, 47, 49, 55 };
static const char    S_CHARS_yin[] = "因引音银印阴隐饮殷姻吟尹淫荫胤瘾寅茵蚓垠";
static const uint8_t S_COST_ying[] = { 27, 30, 30, 31, 35, 36, 37, 40, 40, 40, 42, 44, 44, 45, 46, 46, 46, 47, 47, 48 };
static const char    S_CHARS_ying[] = "应英影营迎硬映赢盈鹰婴颖瀛郢蝇樱莹荧萤瑛";
static const uint8_t S_COST_yo[] = { 43, 51, 30 };
static const char    S_CHARS_yo[] = "哟唷育";
static const uint8_t S_COST_yong[] = { 24, 34, 35, 37, 38, 40, 42, 43, 44, 46, 46, 47, 49, 50, 50, 51, 51, 52, 53, 54 };
static const char    S_CHARS_yong[] = "用永拥勇涌庸佣泳雍鳙咏甬俑踊蛹饔恿痈臃壅";
static const uint8_t S_COST_you[] = { 19, 26, 26, 29, 32, 32, 32, 33, 36, 38, 38, 39, 39, 40, 40, 41, 43, 46, 47, 48 };
static const char    S_CHARS_you[] = "有又由游右优油友尤幼忧犹邮幽悠诱佑铀攸祐";
static const uint8_t S_COST_yu[] = { 23, 25, 30, 30, 31, 31, 32, 32, 33, 34, 35, 35, 36, 36, 37, 37, 38, 39, 39, 39 };
static const char    S_CHARS_yu[] = "于与育鱼语余域玉预遇雨御予欲誉宇渔愈羽豫";
static const uint8_t S_COST_yuan[] = { 26, 26, 27, 27, 31, 31, 33, 33, 35, 35, 37, 37, 39, 39, 42, 42, 45, 45, 45, 46 };
static const char    S_CHARS_yuan[] = "员院原元源远园愿袁圆缘援怨垸冤渊苑垣猿沅";
static const uint8_t S_COST_yue[] = { 26, 30, 32, 36, 37, 37, 37, 42, 44, 44, 53, 56, 57, 57, 63, 64, 66, 74, 74, 74 };
static const char    S_CHARS_yue[] = "月约越曰岳跃阅悦瀹粤钺刖彟樾籥玥龠𫐄𬸚爚";
static const uint8_t S_COST_yun[] = { 28, 32, 39, 41, 41, 41, 42, 43, 43, 48, 48, 49, 50, 50, 51, 52, 52, 52, 53, 53 };
static const char    S_CHARS_yun[] = "运云允郧孕晕韵蕴匀陨酝芸纭耘涢氲郓熨赟愠";
static const uint8_t S_COST_za[] = { 34, 42, 44, 51, 52, 61, 62, 34, 38, 52, 54, 55 };
static const char    S_CHARS_za[] = "杂砸咋匝咂拶臜咱扎啐嘁籴";
static const uint8_t S_COST_zai[] = { 19, 28, 34, 38, 40, 41, 42, 46, 51, 53, 28 };
static const char    S_CHARS_zai[] = "在再载灾仔宰栽哉崽甾才";
static const uint8_t S_COST_zan[] = { 34, 37, 38, 46, 50, 50, 55, 56, 60, 63, 74, 41, 60, 61, 65 };
static const char    S_CHARS_zan[] = "咱赞暂攒簪瓒錾趱糌昝寁涔湔拶酂";
static const uint8_t S_COST_zang[] = { 38, 39, 47, 51, 51, 60, 66, 33, 55 };
static const char    S_CHARS_zang[] = "葬脏赃臧奘牂驵藏戕";
static const uint8_t S_COST_zao[] = { 30, 31, 35, 40, 42, 42, 43, 43, 43, 43, 45, 46, 47, 52, 58, 65, 68, 74, 74, 33 };
static const char    S_CHARS_zao[] = "造早遭藻糟枣燥灶凿躁噪皂澡蚤唣璪慥簉𥖨草";
static const uint8_t S_COST_ze[] = { 30, 32, 34, 35, 51, 51, 52, 55, 55, 59, 60, 66, 67, 34, 36, 41, 44, 52 };
static const char    S_CHARS_ze[] = "则责泽择帻啧仄笮赜舴昃迮箦侧措稷咋柞";
static const uint8_t S_COST_zei[] = { 37, 74 };
static const char    S_CHARS_zei[] = "贼鲗";
static const uint8_t S_COST_zen[] = { 31, 60, 40 };
static const char    S_CHARS_zen[] = "怎谮僭";
static const uint8_t S_COST_zeng[] = { 29, 43, 46, 54, 56, 57, 61, 64, 74, 74, 31, 36, 74 };
static const char    S_CHARS_zeng[] = "增赠憎甑锃缯罾鄫矰䎖曾综鬷";
static const uint8_t S_COST_zha[] = { 37, 38, 43, 43, 44, 45, 46, 46, 46, 47, 49, 52, 52, 52, 54, 54, 55, 60, 62, 62 };
static const char    S_CHARS_zha[] = "炸扎闸诈榨眨栅渣乍札楂铡喳柞吒蚱咤劄哳鲝";
static const uint8_t S_COST_zhai[] = { 38, 39, 41, 41, 42, 42, 69, 26, 34, 35, 37, 39, 51, 52, 58, 59 };
static const char    S_CHARS_zhai[] = "寨债宅斋摘窄瘵度侧择祭柴翟疵擿豸";
static const uint8_t S_COST_zhan[] = { 26, 27, 31, 31, 40, 40, 43, 44, 44, 44, 45, 46, 47, 47, 48, 50, 54, 57, 68, 68 };
static const char    S_CHARS_zhan[] = "战展站占斩粘盏瞻沾詹湛绽毡栈崭蘸旃谵飐搌";
static const uint8_t S_COST_zhang[] = { 23, 28, 32, 33, 36, 37, 38, 38, 39, 40, 41, 41, 41, 46, 46, 49, 50, 52, 53, 54 };
static const char    S_CHARS_zhang[] = "长张章掌障丈涨仗帐杖胀账璋彰漳樟瘴獐嶂鄣";
static const uint8_t S_COST_zhao[] = { 31, 32, 32, 34, 35, 38, 40, 41, 42, 42, 43, 44, 45, 57, 58, 58, 62, 74, 74, 74 };
static const char    S_CHARS_zhao[] = "照招找召赵诏兆昭罩爪沼钊肇棹笊啁曌𬬿旐𬶐";
static const uint8_t S_COST_zhe[] = { 21, 24, 26, 35, 38, 39, 42, 45, 46, 46, 48, 50, 53, 53, 53, 54, 55, 56, 58, 58 };
static const char    S_CHARS_zhe[] = "这着者折哲浙遮蔗鹧褶辙辄蛰锗谪赭蜇柘磔晢";
static const uint8_t S_COST_zhei[] = { 21 };
static const char    S_CHARS_zhei[] = "这";
static const uint8_t S_COST_zhen[] = { 29, 32, 33, 35, 36, 36, 37, 40, 40, 40, 41, 41, 42, 43, 45, 47, 48, 49, 49, 49 };
static const char    S_CHARS_zhen[] = "真镇阵针震珍振诊侦圳祯贞朕枕斟疹甄赈臻帧";
static const uint8_t S_COST_zheng[] = { 24, 26, 29, 30, 31, 33, 37, 38, 39, 41, 42, 42, 46, 47, 48, 52, 52, 53, 55, 63 };
static const char    S_CHARS_zheng[] = "政正争整证征症郑蒸挣睁怔铮筝拯狰徵诤峥钲";
static const uint8_t S_COST_zhi[] = { 23, 26, 26, 27, 27, 28, 28, 29, 29, 29, 30, 31, 31, 31, 31, 32, 33, 33, 34, 35 };
static const char    S_CHARS_zhi[] = "之只制治知直至指职质值支织志置致止执植智";
static const uint8_t S_COST_zhong[] = { 20, 25, 25, 29, 32, 34, 36, 39, 41, 43, 47, 48, 48, 50, 54, 60, 61, 69, 74, 36 };
static const char    S_CHARS_zhong[] = "中种重众终钟忠肿仲衷冢踵锺盅柊舯螽穜茽童";
static const uint8_t S_COST_zhou[] = { 28, 30, 31, 40, 41, 41, 41, 43, 44, 44, 45, 46, 47, 49, 50, 50, 54, 55, 59, 60 };
static const char    S_CHARS_zhou[] = "州周洲轴舟宙皱骤昼粥咒肘绉帚胄纣诌妯籀酎";
static const uint8_t S_COST_zhu[] = { 23, 29, 32, 32, 33, 33, 34, 34, 35, 35, 36, 36, 37, 38, 38, 41, 41, 42, 42, 43 };
static const char    S_CHARS_zhu[] = "主住注助著筑朱逐诸驻竹珠柱祝猪铸株烛煮嘱";
static const uint8_t S_COST_zhua[] = { 36, 66, 42, 50 };
static const char    S_CHARS_zhua[] = "抓髽爪挝";
static const uint8_t S_COST_zhuai[] = { 45, 30 };
static const char    S_CHARS_zhuai[] = "拽转";
static const uint8_t S_COST_zhuan[] = { 29, 30, 40, 42, 42, 46, 47, 55, 59, 69, 74, 74, 29, 48, 51, 52 };
static const char    S_CHARS_zhuan[] = "专转砖撰赚篆馔颛啭瑑䏝僎传湍沌巽";
static const uint8_t S_COST_zhuang[] = { 30, 32, 35, 36, 38, 43, 44, 67, 47, 51, 51, 51, 60, 60 };
static const char    S_CHARS_zhuang[] = "装状庄壮撞妆桩漴幢憧僮奘艟戆";
static const uint8_t S_COST_zhui[] = { 34, 43, 44, 45, 49, 51, 53, 58, 58, 32, 38, 44, 45, 46, 49, 60, 67 };
static const char    S_CHARS_zhui[] = "追锥坠缀赘惴缒骓隹致垂椎隧揣槌萑倕";
static const uint8_t S_COST_zhun[] = { 30, 53, 59, 62, 67, 74, 39, 42, 43, 52, 60, 67, 68, 74 };
static const char    S_CHARS_zhun[] = "准谆肫窀衠𬘯敦屯淳盹胗圫忳䐃";
static const uint8_t S_COST_zhuo[] = { 37, 40, 41, 45, 45, 46, 47, 48, 50, 50, 50, 52, 52, 55, 55, 56, 57, 58, 58, 59 };
static const char    S_CHARS_zhuo[] = "桌捉卓灼浊拙酌啄镯擢涿斫晫茁濯浞叕倬诼棁";
static const uint8_t S_COST_zi[] = { 23, 23, 27, 30, 36, 39, 39, 40, 41, 42, 44, 46, 47, 47, 48, 49, 49, 49, 50, 52 };
static const char    S_CHARS_zi[] = "子自资字紫姊滋咨姿兹缁梓秭孜籽淄渍孳恣辎";
static const uint8_t S_COST_zong[] = { 27, 32, 36, 36, 40, 44, 50, 52, 57, 62, 63, 68, 74, 26, 50, 58 };
static const char    S_CHARS_zong[] = "总宗纵综踪棕粽鬃偬倧腙疭鬷从熜枞";
static const uint8_t S_COST_zou[] = { 28, 36, 45, 51, 57, 59, 60, 61, 69, 28, 37 };
static const char    S_CHARS_zou[] = "走奏邹揍诹陬鄹驺鲰族趣";
static const uint8_t S_COST_zu[] = { 28, 29, 31, 34, 36, 38, 39, 50, 51, 54, 63, 74, 36, 47, 51, 52, 53, 54, 54, 56 };
static const char    S_CHARS_zu[] = "族组足祖阻租卒镞诅俎崒珇姐沮淬啐蹴嘁槭苴";
static const uint8_t S_COST_zuan[] = { 39, 47, 50, 63, 66, 42, 46 };
static const char    S_CHARS_zuan[] = "钻纂攥缵躜赚撮";
static const uint8_t S_COST_zui[] = { 26, 35, 35, 39, 60, 66, 38, 44, 44, 46, 51, 63 };
static const char    S_CHARS_zui[] = "最罪嘴醉蕞槜堆摧咀撮羧觜";
static const uint8_t S_COST_zun[] = { 36, 39, 55, 57, 64, 68, 74, 74, 74, 42, 44, 59, 74 };
static const char    S_CHARS_zun[] = "尊遵樽鳟撙嶟僔噂𨱔奠蹲捽僎";
static const uint8_t S_COST_zuo[] = { 24, 29, 31, 32, 32, 37, 42, 45, 50, 51, 59, 60, 61, 67, 68, 43, 43, 45, 46, 46 };
static const char    S_CHARS_zuo[] = "作做左坐座昨佐琢祚唑捽胙怍阼岞挫凿醋撮乍";

const pe_syllable_t pe_dict_syllables[PE_SYLLABLE_COUNT] = {
    { "a", S_CHARS_a, S_COST_a, 7 },
    { "ai", S_CHARS_ai, S_COST_ai, 20 },
    { "an", S_CHARS_an, S_COST_an, 20 },
    { "ang", S_CHARS_ang, S_COST_ang, 6 },
    { "ao", S_CHARS_ao, S_COST_ao, 20 },
    { "ba", S_CHARS_ba, S_COST_ba, 20 },
    { "bai", S_CHARS_bai, S_COST_bai, 20 },
    { "ban", S_CHARS_ban, S_COST_ban, 20 },
    { "bang", S_CHARS_bang, S_COST_bang, 20 },
    { "bao", S_CHARS_bao, S_COST_bao, 20 },
    { "bei", S_CHARS_bei, S_COST_bei, 20 },
    { "ben", S_CHARS_ben, S_COST_ben, 14 },
    { "beng", S_CHARS_beng, S_COST_beng, 20 },
    { "bi", S_CHARS_bi, S_COST_bi, 20 },
    { "bian", S_CHARS_bian, S_COST_bian, 20 },
    { "biao", S_CHARS_biao, S_COST_biao, 20 },
    { "bie", S_CHARS_bie, S_COST_bie, 14 },
    { "bin", S_CHARS_bin, S_COST_bin, 20 },
    { "bing", S_CHARS_bing, S_COST_bing, 20 },
    { "bo", S_CHARS_bo, S_COST_bo, 20 },
    { "bu", S_CHARS_bu, S_COST_bu, 20 },
    { "ca", S_CHARS_ca, S_COST_ca, 4 },
    { "cai", S_CHARS_cai, S_COST_cai, 12 },
    { "can", S_CHARS_can, S_COST_can, 14 },
    { "cang", S_CHARS_cang, S_COST_cang, 8 },
    { "cao", S_CHARS_cao, S_COST_cao, 13 },
    { "ce", S_CHARS_ce, S_COST_ce, 8 },
    { "cen", S_CHARS_cen, S_COST_cen, 4 },
    { "ceng", S_CHARS_ceng, S_COST_ceng, 8 },
    { "cha", S_CHARS_cha, S_COST_cha, 20 },
    { "chai", S_CHARS_chai, S_COST_chai, 12 },
    { "chan", S_CHARS_chan, S_COST_chan, 20 },
    { "chang", S_CHARS_chang, S_COST_chang, 20 },
    { "chao", S_CHARS_chao, S_COST_chao, 17 },
    { "che", S_CHARS_che, S_COST_che, 14 },
    { "chen", S_CHARS_chen, S_COST_chen, 20 },
    { "cheng", S_CHARS_cheng, S_COST_cheng, 20 },
    { "chi", S_CHARS_chi, S_COST_chi, 20 },
    { "chong", S_CHARS_chong, S_COST_chong, 20 },
    { "chou", S_CHARS_chou, S_COST_chou, 20 },
    { "chu", S_CHARS_chu, S_COST_chu, 20 },
    { "chua", S_CHARS_chua, S_COST_chua, 2 },
    { "chuai", S_CHARS_chuai, S_COST_chuai, 6 },
    { "chuan", S_CHARS_chuan, S_COST_chuan, 15 },
    { "chuang", S_CHARS_chuang, S_COST_chuang, 13 },
    { "chui", S_CHARS_chui, S_COST_chui, 13 },
    { "chun", S_CHARS_chun, S_COST_chun, 18 },
    { "chuo", S_CHARS_chuo, S_COST_chuo, 20 },
    { "ci", S_CHARS_ci, S_COST_ci, 20 },
    { "cong", S_CHARS_cong, S_COST_cong, 16 },
    { "cou", S_CHARS_cou, S_COST_cou, 9 },
    { "cu", S_CHARS_cu, S_COST_cu, 17 },
    { "cuan", S_CHARS_cuan, S_COST_cuan, 10 },
    { "cui", S_CHARS_cui, S_COST_cui, 20 },
    { "cun", S_CHARS_cun, S_COST_cun, 8 },
    { "cuo", S_CHARS_cuo, S_COST_cuo, 20 },
    { "da", S_CHARS_da, S_COST_da, 20 },
    { "dai", S_CHARS_dai, S_COST_dai, 20 },
    { "dan", S_CHARS_dan, S_COST_dan, 20 },
    { "dang", S_CHARS_dang, S_COST_dang, 17 },
    { "dao", S_CHARS_dao, S_COST_dao, 20 },
    { "de", S_CHARS_de, S_COST_de, 9 },
    { "dei", S_CHARS_dei, S_COST_dei, 2 },
    { "den", S_CHARS_den, S_COST_den, 1 },
    { "deng", S_CHARS_deng, S_COST_deng, 16 },
    { "di", S_CHARS_di, S_COST_di, 20 },
    { "dia", S_CHARS_dia, S_COST_dia, 1 },
    { "dian", S_CHARS_dian, S_COST_dian, 20 },
    { "diao", S_CHARS_diao, S_COST_diao, 20 },
    { "die", S_CHARS_die, S_COST_die, 20 },
    { "ding", S_CHARS_ding, S_COST_ding, 20 },
    { "diu", S_CHARS_diu, S_COST_diu, 2 },
    { "dong", S_CHARS_dong, S_COST_dong, 20 },
    { "dou", S_CHARS_dou, S_COST_dou, 18 },
    { "du", S_CHARS_du, S_COST_du, 20 },
    { "duan", S_CHARS_duan, S_COST_duan, 12 },
    { "dui", S_CHARS_dui, S_COST_dui, 12 },
    { "dun", S_CHARS_dun, S_COST_dun, 20 },
    { "duo", S_CHARS_duo, S_COST_duo, 20 },
    { "e", S_CHARS_e, S_COST_e, 20 },
    { "ei", S_CHARS_ei, S_COST_ei, 1 },
    { "en", S_CHARS_en, S_COST_en, 3 },
    { "er", S_CHARS_er, S_COST_er, 20 },
    { "fa", S_CHARS_fa, S_COST_fa, 13 },
    { "fan", S_CHARS_fan, S_COST_fan, 20 },
    { "fang", S_CHARS_fang, S_COST_fang, 20 },
    { "fei", S_CHARS_fei, S_COST_fei, 20 },
    { "fen", S_CHARS_fen, S_COST_fen, 20 },
    { "feng", S_CHARS_feng, S_COST_feng, 20 },
    { "fo", S_CHARS_fo, S_COST_fo, 1 },
    { "fou", S_CHARS_fou, S_COST_fou, 6 },
    { "fu", S_CHARS_fu, S_COST_fu, 20 },
    { "ga", S_CHARS_ga, S_COST_ga, 14 },
    { "gai", S_CHARS_gai, S_COST_gai, 20 },
    { "gan", S_CHARS_gan, S_COST_gan, 20 },
    { "gang", S_CHARS_gang, S_COST_gang, 20 },
    { "gao", S_CHARS_gao, S_COST_gao, 20 },
    { "ge", S_CHARS_ge, S_COST_ge, 20 },
    { "gei", S_CHARS_gei, S_COST_gei, 1 },
    { "gen", S_CHARS_gen, S_COST_gen, 7 },
    { "geng", S_CHARS_geng, S_COST_geng, 20 },
    { "gong", S_CHARS_gong, S_COST_gong, 20 },
    { "gou", S_CHARS_gou, S_COST_gou, 20 },
    { "gu", S_CHARS_gu, S_COST_gu, 20 },
    { "gua", S_CHARS_gua, S_COST_gua, 17 },
    { "guai", S_CHARS_guai, S_COST_guai, 5 },
    { "guan", S_CHARS_guan, S_COST_guan, 20 },
    { "guang", S_CHARS_guang, S_COST_guang, 14 },
    { "gui", S_CHARS_gui, S_COST_gui, 20 },
    { "gun", S_CHARS_gun, S_COST_gun, 10 },
    { "guo", S_CHARS_guo, S_COST_guo, 20 },
    { "ha", S_CHARS_ha, S_COST_ha, 7 },
    { "hai", S_CHARS_hai, S_COST_hai, 16 },
    { "han", S_CHARS_han, S_COST_han, 20 },
    { "hang", S_CHARS_hang, S_COST_hang, 16 },
    { "hao", S_CHARS_hao, S_COST_hao, 20 },
    { "he", S_CHARS_he, S_COST_he, 20 },
    { "hei", S_CHARS_hei, S_COST_hei, 4 },
    { "hen", S_CHARS_hen, S_COST_hen, 8 },
    { "heng", S_CHARS_heng, S_COST_heng, 13 },
    { "hng", S_CHARS_hng, S_COST_hng, 1 },
    { "hong", S_CHARS_hong, S_COST_hong, 20 },
    { "hou", S_CHARS_hou, S_COST_hou, 20 },
    { "hu", S_CHARS_hu, S_COST_hu, 20 },
    { "hua", S_CHARS_hua, S_COST_hua, 20 },
    { "huai", S_CHARS_huai, S_COST_huai, 10 },
    { "huan", S_CHARS_huan, S_COST_huan, 20 },
    { "huang", S_CHARS_huang, S_COST_huang, 20 },
    { "hui", S_CHARS_hui, S_COST_hui, 20 },
    { "hun", S_CHARS_hun, S_COST_hun, 18 },
    { "huo", S_CHARS_huo, S_COST_huo, 20 },
    { "ji", S_CHARS_ji, S_COST_ji, 20 },
    { "jia", S_CHARS_jia, S_COST_jia, 20 },
    { "jian", S_CHARS_jian, S_COST_jian, 20 },
    { "jiang", S_CHARS_jiang, S_COST_jiang, 20 },
    { "jiao", S_CHARS_jiao, S_COST_jiao, 20 },
    { "jie", S_CHARS_jie, S_COST_jie, 20 },
    { "jin", S_CHARS_jin, S_COST_jin, 20 },
    { "jing", S_CHARS_jing, S_COST_jing, 20 },
    { "jiong", S_CHARS_jiong, S_COST_jiong, 13 },
    { "jiu", S_CHARS_jiu, S_COST_jiu, 20 },
    { "ju", S_CHARS_ju, S_COST_ju, 20 },
    { "juan", S_CHARS_juan, S_COST_juan, 20 },
    { "jue", S_CHARS_jue, S_COST_jue, 20 },
    { "jun", S_CHARS_jun, S_COST_jun, 20 },
    { "ka", S_CHARS_ka, S_COST_ka, 6 },
    { "kai", S_CHARS_kai, S_COST_kai, 20 },
    { "kan", S_CHARS_kan, S_COST_kan, 20 },
    { "kang", S_CHARS_kang, S_COST_kang, 17 },
    { "kao", S_CHARS_kao, S_COST_kao, 14 },
    { "ke", S_CHARS_ke, S_COST_ke, 20 },
    { "kei", S_CHARS_kei, S_COST_kei, 2 },
    { "ken", S_CHARS_ken, S_COST_ken, 10 },
    { "keng", S_CHARS_keng, S_COST_keng, 7 },
    { "kong", S_CHARS_kong, S_COST_kong, 12 },
    { "kou", S_CHARS_kou, S_COST_kou, 15 },
    { "ku", S_CHARS_ku, S_COST_ku, 20 },
    { "kua", S_CHARS_kua, S_COST_kua, 8 },
    { "kuai", S_CHARS_kuai, S_COST_kuai, 15 },
    { "kuan", S_CHARS_kuan, S_COST_kuan, 5 },
    { "kuang", S_CHARS_kuang, S_COST_kuang, 20 },
    { "kui", S_CHARS_kui, S_COST_kui, 20 },
    { "kun", S_CHARS_kun, S_COST_kun, 19 },
    { "kuo", S_CHARS_kuo, S_COST_kuo, 8 },
    { "la", S_CHARS_la, S_COST_la, 18 },
    { "lai", S_CHARS_lai, S_COST_lai, 16 },
    { "lan", S_CHARS_lan, S_COST_lan, 20 },
    { "lang", S_CHARS_lang, S_COST_lang, 20 },
    { "lao", S_CHARS_lao, S_COST_lao, 20 },
    { "le", S_CHARS_le, S_COST_le, 11 },
    { "lei", S_CHARS_lei, S_COST_lei, 20 },
    { "len", S_CHARS_len, S_COST_len, 1 },
    { "leng", S_CHARS_leng, S_COST_leng, 7 },
    { "li", S_CHARS_li, S_COST_li, 20 },
    { "lia", S_CHARS_lia, S_COST_lia, 1 },
    { "lian", S_CHARS_lian, S_COST_lian, 20 },
    { "liang", S_CHARS_liang, S_COST_liang, 20 },
    { "liao", S_CHARS_liao, S_COST_liao, 20 },
    { "lie", S_CHARS_lie, S_COST_lie, 20 },
    { "lin", S_CHARS_lin, S_COST_lin, 20 },
    { "ling", S_CHARS_ling, S_COST_ling, 20 },
    { "liu", S_CHARS_liu, S_COST_liu, 20 },
    { "lo", S_CHARS_lo, S_COST_lo, 1 },
    { "long", S_CHARS_long, S_COST_long, 20 },
    { "lou", S_CHARS_lou, S_COST_lou, 20 },
    { "lu", S_CHARS_lu, S_COST_lu, 20 },
    { "luan", S_CHARS_luan, S_COST_luan, 12 },
    { "lun", S_CHARS_lun, S_COST_lun, 10 },
    { "luo", S_CHARS_luo, S_COST_luo, 20 },
    { "lv", S_CHARS_lv, S_COST_lv, 20 },
    { "lve", S_CHARS_lve, S_COST_lve, 6 },
    { "m", S_CHARS_m, S_COST_m, 3 },
    { "ma", S_CHARS_ma, S_COST_ma, 20 },
    { "mai", S_CHARS_mai, S_COST_mai, 14 },
    { "man", S_CHARS_man, S_COST_man, 20 },
    { "mang", S_CHARS_mang, S_COST_mang, 17 },
    { "mao", S_CHARS_mao, S_COST_mao, 20 },
    { "me", S_CHARS_me, S_COST_me, 4 },
    { "mei", S_CHARS_mei, S_COST_mei, 20 },
    { "men", S_CHARS_men, S_COST_men, 13 },
    { "meng", S_CHARS_meng, S_COST_meng, 20 },
    { "mi", S_CHARS_mi, S_COST_mi, 20 },
    { "mian", S_CHARS_mian, S_COST_mian, 20 },
    { "miao", S_CHARS_miao, S_COST_miao, 19 },
    { "mie", S_CHARS_mie, S_COST_mie, 6 },
    { "min", S_CHARS_min, S_COST_min, 20 },
    { "ming", S_CHARS_ming, S_COST_ming, 17 },
    { "miu", S_CHARS_miu, S_COST_miu, 2 },
    { "mo", S_CHARS_mo, S_COST_mo, 20 },
    { "mou", S_CHARS_mou, S_COST_mou, 14 },
    { "mu", S_CHARS_mu, S_COST_mu, 20 },
    { "n", S_CHARS_n, S_COST_n, 5 },
    { "na", S_CHARS_na, S_COST_na, 18 },
    { "nai", S_CHARS_nai, S_COST_nai, 15 },
    { "nan", S_CHARS_nan, S_COST_nan, 14 },
    { "nang", S_CHARS_nang, S_COST_nang, 8 },
    { "nao", S_CHARS_nao, S_COST_nao, 15 },
    { "ne", S_CHARS_ne, S_COST_ne, 6 },
    { "nei", S_CHARS_nei, S_COST_nei, 4 },
    { "nen", S_CHARS_nen, S_COST_nen, 5 },
    { "neng", S_CHARS_neng, S_COST_neng, 3 },
    { "ng", S_CHARS_ng, S_COST_ng, 4 },
    { "ni", S_CHARS_ni, S_COST_ni, 20 },
    { "nian", S_CHARS_nian, S_COST_nian, 15 },
    { "niang", S_CHARS_niang, S_COST_niang, 2 },
    { "niao", S_CHARS_niao, S_COST_niao, 7 },
    { "nie", S_CHARS_nie, S_COST_nie, 20 },
    { "nin", S_CHARS_nin, S_COST_nin, 2 },
    { "ning", S_CHARS_ning, S_COST_ning, 16 },
    { "niu", S_CHARS_niu, S_COST_niu, 10 },
    { "nong", S_CHARS_nong, S_COST_nong, 9 },
    { "nou", S_CHARS_nou, S_COST_nou, 2 },
    { "nu", S_CHARS_nu, S_COST_nu, 15 },
    { "nuan", S_CHARS_nuan, S_COST_nuan, 4 },
    { "nun", S_CHARS_nun, S_COST_nun, 1 },
    { "nuo", S_CHARS_nuo, S_COST_nuo, 17 },
    { "nv", S_CHARS_nv, S_COST_nv, 7 },
    { "nve", S_CHARS_nve, S_COST_nve, 2 },
    { "o", S_CHARS_o, S_COST_o, 4 },
    { "ou", S_CHARS_ou, S_COST_ou, 19 },
    { "pa", S_CHARS_pa, S_COST_pa, 19 },
    { "pai", S_CHARS_pai, S_COST_pai, 14 },
    { "pan", S_CHARS_pan, S_COST_pan, 20 },
    { "pang", S_CHARS_pang, S_COST_pang, 20 },
    { "pao", S_CHARS_pao, S_COST_pao, 20 },
    { "pei", S_CHARS_pei, S_COST_pei, 20 },
    { "pen", S_CHARS_pen, S_COST_pen, 5 },
    { "peng", S_CHARS_peng, S_COST_peng, 20 },
    { "pi", S_CHARS_pi, S_COST_pi, 20 },
    { "pian", S_CHARS_pian, S_COST_pian, 20 },
    { "piao", S_CHARS_piao, S_COST_piao, 18 },
    { "pie", S_CHARS_pie, S_COST_pie, 6 },
    { "pin", S_CHARS_pin, S_COST_pin, 14 },
    { "ping", S_CHARS_ping, S_COST_ping, 20 },
    { "po", S_CHARS_po, S_COST_po, 20 },
    { "pou", S_CHARS_pou, S_COST_pou, 12 },
    { "pu", S_CHARS_pu, S_COST_pu, 20 },
    { "qi", S_CHARS_qi, S_COST_qi, 20 },
    { "qia", S_CHARS_qia, S_COST_qia, 13 },
    { "qian", S_CHARS_qian, S_COST_qian, 20 },
    { "qiang", S_CHARS_qiang, S_COST_qiang, 20 },
    { "qiao", S_CHARS_qiao, S_COST_qiao, 20 },
    { "qie", S_CHARS_qie, S_COST_qie, 20 },
    { "qin", S_CHARS_qin, S_COST_qin, 20 },
    { "qing", S_CHARS_qing, S_COST_qing, 20 },
    { "qiong", S_CHARS_qiong, S_COST_qiong, 12 },
    { "qiu", S_CHARS_qiu, S_COST_qiu, 20 },
    { "qu", S_CHARS_qu, S_COST_qu, 20 },
    { "quan", S_CHARS_quan, S_COST_quan, 20 },
    { "que", S_CHARS_que, S_COST_que, 20 },
    { "qun", S_CHARS_qun, S_COST_qun, 7 },
    { "ran", S_CHARS_ran, S_COST_ran, 8 },
    { "rang", S_CHARS_rang, S_COST_rang, 10 },
    { "rao", S_CHARS_rao, S_COST_rao, 6 },
    { "re", S_CHARS_re, S_COST_re, 5 },
    { "ren", S_CHARS_ren, S_COST_ren, 20 },
    { "reng", S_CHARS_reng, S_COST_reng, 6 },
    { "ri", S_CHARS_ri, S_COST_ri, 2 },
    { "rong", S_CHARS_rong, S_COST_rong, 17 },
    { "rou", S_CHARS_rou, S_COST_rou, 7 },
    { "ru", S_CHARS_ru, S_COST_ru, 20 },
    { "ruan", S_CHARS_ruan, S_COST_ruan, 7 },
    { "rui", S_CHARS_rui, S_COST_rui, 13 },
    { "run", S_CHARS_run, S_COST_run, 2 },
    { "ruo", S_CHARS_ruo, S_COST_ruo, 11 },
    { "sa", S_CHARS_sa, S_COST_sa, 14 },
    { "sai", S_CHARS_sai, S_COST_sai, 6 },
    { "san", S_CHARS_san, S_COST_san, 9 },
    { "sang", S_CHARS_sang, S_COST_sang, 6 },
    { "sao", S_CHARS_sao, S_COST_sao, 13 },
    { "se", S_CHARS_se, S_COST_se, 14 },
    { "sen", S_CHARS_sen, S_COST_sen, 2 },
    { "seng", S_CHARS_seng, S_COST_seng, 1 },
    { "sha", S_CHARS_sha, S_COST_sha, 20 },
    { "shai", S_CHARS_shai, S_COST_shai, 4 },
    { "shan", S_CHARS_shan, S_COST_shan, 20 },
    { "shang", S_CHARS_shang, S_COST_shang, 15 },
    { "shao", S_CHARS_shao, S_COST_shao, 20 },
    { "she", S_CHARS_she, S_COST_she, 20 },
    { "shei", S_CHARS_shei, S_COST_shei, 1 },
    { "shen", S_CHARS_shen, S_COST_shen, 20 },
    { "sheng", S_CHARS_sheng, S_COST_sheng, 20 },
    { "shi", S_CHARS_shi, S_COST_shi, 20 },
    { "shou", S_CHARS_shou, S_COST_shou, 14 },
    { "shu", S_CHARS_shu, S_COST_shu, 20 },
    { "shua", S_CHARS_shua, S_COST_shua, 5 },
    { "shuai", S_CHARS_shuai, S_COST_shuai, 6 },
    { "shuan", S_CHARS_shuan, S_COST_shuan, 7 },
    { "shuang", S_CHARS_shuang, S_COST_shuang, 10 },
    { "shui", S_CHARS_shui, S_COST_shui, 6 },
    { "shun", S_CHARS_shun, S_COST_shun, 9 },
    { "shuo", S_CHARS_shuo, S_COST_shuo, 17 },
    { "si", S_CHARS_si, S_COST_si, 20 },
    { "song", S_CHARS_song, S_COST_song, 18 },
    { "sou", S_CHARS_sou, S_COST_sou, 19 },
    { "su", S_CHARS_su, S_COST_su, 20 },
    { "suan", S_CHARS_suan, S_COST_suan, 5 },
    { "sui", S_CHARS_sui, S_COST_sui, 20 },
    { "sun", S_CHARS_sun, S_COST_sun, 11 },
    { "suo", S_CHARS_suo, S_COST_suo, 20 },
    { "ta", S_CHARS_ta, S_COST_ta, 20 },
    { "tai", S_CHARS_tai, S_COST_tai, 20 },
    { "tan", S_CHARS_tan, S_COST_tan, 20 },
    { "tang", S_CHARS_tang, S_COST_tang, 20 },
    { "tao", S_CHARS_tao, S_COST_tao, 20 },
    { "te", S_CHARS_te, S_COST_te, 8 },
    { "tei", S_CHARS_tei, S_COST_tei, 1 },
    { "teng", S_CHARS_teng, S_COST_teng, 9 },
    { "ti", S_CHARS_ti, S_COST_ti, 20 },
    { "tian", S_CHARS_tian, S_COST_tian, 20 },
    { "tiao", S_CHARS_tiao, S_COST_tiao, 20 },
    { "tie", S_CHARS_tie, S_COST_tie, 8 },
    { "ting", S_CHARS_ting, S_COST_ting, 20 },
    { "tong", S_CHARS_tong, S_COST_tong, 20 },
    { "tou", S_CHARS_tou, S_COST_tou, 9 },
    { "tu", S_CHARS_tu, S_COST_tu, 20 },
    { "tuan", S_CHARS_tuan, S_COST_tuan, 11 },
    { "tui", S_CHARS_tui, S_COST_tui, 15 },
    { "tun", S_CHARS_tun, S_COST_tun, 20 },
    { "tuo", S_CHARS_tuo, S_COST_tuo, 20 },
    { "wa", S_CHARS_wa, S_COST_wa, 17 },
    { "wai", S_CHARS_wai, S_COST_wai, 4 },
    { "wan", S_CHARS_wan, S_COST_wan, 20 },
    { "wang", S_CHARS_wang, S_COST_wang, 20 },
    { "wei", S_CHARS_wei, S_COST_wei, 20 },
    { "wen", S_CHARS_wen, S_COST_wen, 20 },
    { "weng", S_CHARS_weng, S_COST_weng, 9 },
    { "wo", S_CHARS_wo, S_COST_wo, 20 },
    { "wu", S_CHARS_wu, S_COST_wu, 20 },
    { "xi", S_CHARS_xi, S_COST_xi, 20 },
    { "xia", S_CHARS_xia, S_COST_xia, 20 },
    { "xian", S_CHARS_xian, S_COST_xian, 20 },
    { "xiang", S_CHARS_xiang, S_COST_xiang, 20 },
    { "xiao", S_CHARS_xiao, S_COST_xiao, 20 },
    { "xie", S_CHARS_xie, S_COST_xie, 20 },
    { "xin", S_CHARS_xin, S_COST_xin, 20 },
    { "xing", S_CHARS_xing, S_COST_xing, 20 },
    { "xiong", S_CHARS_xiong, S_COST_xiong, 14 },
    { "xiu", S_CHARS_xiu, S_COST_xiu, 20 },
    { "xu", S_CHARS_xu, S_COST_xu, 20 },
    { "xuan", S_CHARS_xuan, S_COST_xuan, 20 },
    { "xue", S_CHARS_xue, S_COST_xue, 17 },
    { "xun", S_CHARS_xun, S_COST_xun, 20 },
    { "ya", S_CHARS_ya, S_COST_ya, 20 },
    { "yan", S_CHARS_yan, S_COST_yan, 20 },
    { "yang", S_CHARS_yang, S_COST_yang, 20 },
    { "yao", S_CHARS_yao, S_COST_yao, 20 },
    { "ye", S_CHARS_ye, S_COST_ye, 20 },
    { "yi", S_CHARS_yi, S_COST_yi, 20 },
    { "yin", S_CHARS_yin, S_COST_yin, 20 },
    { "ying", S_CHARS_ying, S_COST_ying, 20 },
    { "yo", S_CHARS_yo, S_COST_yo, 3 },
    { "yong", S_CHARS_yong, S_COST_yong, 20 },
    { "you", S_CHARS_you, S_COST_you, 20 },
    { "yu", S_CHARS_yu, S_COST_yu, 20 },
    { "yuan", S_CHARS_yuan, S_COST_yuan, 20 },
    { "yue", S_CHARS_yue, S_COST_yue, 20 },
    { "yun", S_CHARS_yun, S_COST_yun, 20 },
    { "za", S_CHARS_za, S_COST_za, 12 },
    { "zai", S_CHARS_zai, S_COST_zai, 11 },
    { "zan", S_CHARS_zan, S_COST_zan, 15 },
    { "zang", S_CHARS_zang, S_COST_zang, 9 },
    { "zao", S_CHARS_zao, S_COST_zao, 20 },
    { "ze", S_CHARS_ze, S_COST_ze, 18 },
    { "zei", S_CHARS_zei, S_COST_zei, 2 },
    { "zen", S_CHARS_zen, S_COST_zen, 3 },
    { "zeng", S_CHARS_zeng, S_COST_zeng, 13 },
    { "zha", S_CHARS_zha, S_COST_zha, 20 },
    { "zhai", S_CHARS_zhai, S_COST_zhai, 16 },
    { "zhan", S_CHARS_zhan, S_COST_zhan, 20 },
    { "zhang", S_CHARS_zhang, S_COST_zhang, 20 },
    { "zhao", S_CHARS_zhao, S_COST_zhao, 20 },
    { "zhe", S_CHARS_zhe, S_COST_zhe, 20 },
    { "zhei", S_CHARS_zhei, S_COST_zhei, 1 },
    { "zhen", S_CHARS_zhen, S_COST_zhen, 20 },
    { "zheng", S_CHARS_zheng, S_COST_zheng, 20 },
    { "zhi", S_CHARS_zhi, S_COST_zhi, 20 },
    { "zhong", S_CHARS_zhong, S_COST_zhong, 20 },
    { "zhou", S_CHARS_zhou, S_COST_zhou, 20 },
    { "zhu", S_CHARS_zhu, S_COST_zhu, 20 },
    { "zhua", S_CHARS_zhua, S_COST_zhua, 4 },
    { "zhuai", S_CHARS_zhuai, S_COST_zhuai, 2 },
    { "zhuan", S_CHARS_zhuan, S_COST_zhuan, 16 },
    { "zhuang", S_CHARS_zhuang, S_COST_zhuang, 14 },
    { "zhui", S_CHARS_zhui, S_COST_zhui, 17 },
    { "zhun", S_CHARS_zhun, S_COST_zhun, 14 },
    { "zhuo", S_CHARS_zhuo, S_COST_zhuo, 20 },
    { "zi", S_CHARS_zi, S_COST_zi, 20 },
    { "zong", S_CHARS_zong, S_COST_zong, 16 },
    { "zou", S_CHARS_zou, S_COST_zou, 11 },
    { "zu", S_CHARS_zu, S_COST_zu, 20 },
    { "zuan", S_CHARS_zuan, S_COST_zuan, 7 },
    { "zui", S_CHARS_zui, S_COST_zui, 12 },
    { "zun", S_CHARS_zun, S_COST_zun, 13 },
    { "zuo", S_CHARS_zuo, S_COST_zuo, 20 },
};
