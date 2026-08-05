# 相模日语辞典（New Nintendo 3DS）

离线日中／中日开源词典。上屏显示词条、读音、词性和释义；下屏提供触摸搜索、候选列表与软键盘。

## 当前 v0.1 功能

- New 3DS / New 2DS 专用，启用 804 MHz CPU 模式。
- 日语汉字、假名查中文释义。
- 中文使用无声调拼音反查日语词条。
- 下屏触摸假名／拼音键盘与候选词列表。
- 十字键、摇杆、C 摇杆、A/B/X/Y、L/R、ZL/ZR、START、SELECT 均有功能。
- 词库来自 Kaikki 的中文维基词典日语数据导出。

## 操作

- 摇杆／十字键：移动候选
- A：查询或打开词条
- B：返回／删除
- X：临时收藏当前词条；输入时清空
- Y：切换日中／中日
- L/R：候选翻页
- ZL/ZR：跳到首条／末条
- C 摇杆：滚动上屏释义
- START：打开搜索
- SELECT：帮助
- START + SELECT：退出

## 生成词库

```sh
python tools/build_dictionary.py data/kaikki-ja-zh.jsonl romfs/dict \
  --pinyin-dic ../romfs/pinyin.dic
```

转换器可以直接读取现有的 `pinyin.dic` 生成中文分词和拼音反查；若额外安装 `jieba`、`opencc-python-reimplemented` 和 `pypinyin`，还能补充更多中文分词、简繁索引和生僻词拼音。

## 编译

需要 devkitPro 的 `3ds-dev` 组件组，以及用于 CIA 的 `makerom`。

```sh
make
make cia
```

## 许可

- 程序代码：MIT
- Kaikki / Wiktionary 数据：CC BY-SA 及 GFDL，具体署名见 `licenses/DICTIONARY-SOURCE.md`
- Noto Sans CJK 字体子集：SIL OFL 1.1
