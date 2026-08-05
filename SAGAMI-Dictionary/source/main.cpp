#include <3ds.h>

#include <algorithm>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "dictionary.hpp"
#include "ui.hpp"

namespace {
enum class Screen { Browse, Entry, Input, Help };

struct App {
    Dictionary dictionary;
    SearchMode mode = SearchMode::Japanese;
    Screen screen = Screen::Browse;
    std::string query = "日本";
    std::vector<DictEntry> results;
    int selected = 0;
    int detailScroll = 0;
    std::set<uint32_t> favorites;
    std::string status;
};

const char* kanaRows[] = {
    "あいうえおかきくけこ",
    "さしすせそたちつてと",
    "なにぬねのはひふへほ",
    "まみむめもやゆよらり",
    "るれろわをんーゃゅょ"
};
const char* latinRows[] = {"qwertyuiop", "asdfghjkl", "zxcvbnm"};

std::vector<std::string> utf8Chars(const std::string& value) {
    std::vector<std::string> out;
    for (std::size_t i = 0; i < value.size();) {
        const unsigned char c = static_cast<unsigned char>(value[i]);
        std::size_t len = c < 0x80 ? 1 : ((c & 0xE0) == 0xC0 ? 2 : ((c & 0xF0) == 0xE0 ? 3 : 4));
        if (i + len > value.size()) len = 1;
        out.push_back(value.substr(i, len));
        i += len;
    }
    return out;
}

void popUtf8(std::string& value) {
    if (value.empty()) return;
    std::size_t i = value.size() - 1;
    while (i > 0 && (static_cast<unsigned char>(value[i]) & 0xC0) == 0x80) --i;
    value.erase(i);
}

void runSearch(App& app) {
    app.selected = 0;
    app.detailScroll = 0;
    if (!app.dictionary.search(app.query, app.mode, app.results))
        app.status = "没有匹配词条，可缩短关键词再试";
    else
        app.status = "找到 " + std::to_string(app.results.size()) + " 条（最多显示 36 条）";
}

void drawTop(const App& app) {
    ui::top();
    ui::rect(0, 0, 400, 34, ui::RED);
    ui::text(12, 5, 0.62f, ui::TEXT, "相模日语辞典");
    ui::text(305, 7, 0.43f, ui::TEXT,
             app.mode == SearchMode::Japanese ? "日 → 中" : "中 → 日");

    if (!app.dictionary.ready()) {
        ui::text(15, 54, 0.58f, ui::TEXT, "词库加载失败");
        ui::wrapped(15, 92, 370, 26, 0.45f, ui::DIM, app.dictionary.error(), 0, 5);
        return;
    }
    if (app.screen == Screen::Help) {
        ui::text(16, 50, 0.66f, ui::TEXT, "操作说明");
        ui::wrapped(16, 88, 370, 23, 0.43f, ui::TEXT,
            "摇杆/十字键：移动候选  A：打开词条  B：返回或删除  X：收藏  Y：切换查询方向  L/R：翻页  ZL/ZR：首尾跳转  C摇杆：滚动释义  START：输入  SELECT：帮助", 0, 7);
        return;
    }
    if (app.screen == Screen::Input) {
        ui::text(16, 54, 0.52f, ui::DIM,
                 app.mode == SearchMode::Japanese ? "输入日语汉字或假名" : "输入中文拼音（不带声调）");
        ui::rect(14, 88, 372, 56, ui::PANEL);
        ui::textClipped(24, 99, 0.72f, ui::TEXT, app.query.empty() ? "…" : app.query, 350);
        ui::text(16, 174, 0.44f, ui::DIM,
                 "触摸下屏键盘；A/START 查询，B 删除，X 清空，Y 切换方向");
        return;
    }
    if (app.results.empty()) {
        ui::text(16, 64, 0.56f, ui::TEXT, app.status);
        return;
    }
    const DictEntry& entry = app.results[std::clamp(app.selected, 0, static_cast<int>(app.results.size()) - 1)];
    ui::textClipped(16, 44, 0.82f, ui::TEXT, entry.word, 365);
    ui::textClipped(18, 83, 0.48f, ui::RED, entry.reading, 360);
    ui::textClipped(18, 110, 0.40f, ui::DIM, entry.partOfSpeech, 360);
    ui::rect(14, 137, 372, 2, ui::RED_DARK);
    ui::wrapped(18, 147, 365, 21, 0.40f, ui::TEXT, entry.gloss,
                app.detailScroll, 4);
    if (app.favorites.count(entry.recordOffset)) ui::text(365, 45, 0.48f, ui::RED, "★");
}

void drawBrowseBottom(App& app, bool touched, const touchPosition& touch) {
    ui::bottom();
    ui::rect(7, 6, 306, 34, ui::PANEL_ALT);
    ui::textClipped(15, 11, 0.48f, ui::TEXT,
                    (app.mode == SearchMode::Japanese ? "日中  " : "中日  ") + app.query, 285);
    if (touched && touch.py < 42) app.screen = Screen::Input;

    const int pageBase = (app.selected / 6) * 6;
    for (int row = 0; row < 6; ++row) {
        const int index = pageBase + row;
        const float y = 46 + row * 27.0f;
        if (index >= static_cast<int>(app.results.size())) break;
        const bool selected = index == app.selected;
        ui::rect(7, y, 306, 25, selected ? ui::RED_DARK : (row % 2 ? ui::PANEL : ui::PANEL_ALT));
        ui::textClipped(13, y + 3, 0.43f, ui::TEXT, app.results[index].word, 128);
        ui::textClipped(146, y + 4, 0.36f, ui::DIM, app.results[index].gloss, 160);
        if (touched && touch.px >= 7 && touch.px < 313 && touch.py >= y && touch.py < y + 25) {
            app.selected = index;
            app.detailScroll = 0;
        }
    }
    ui::text(8, 215, 0.34f, ui::DIM, "START 搜索  Y 方向  L/R 翻页  SELECT 帮助");
}

void drawInputBottom(App& app, bool touched, const touchPosition& touch) {
    ui::bottom();
    ui::rect(4, 4, 312, 30, ui::PANEL);
    ui::textClipped(10, 8, 0.45f, ui::TEXT, app.query.empty() ? "输入…" : app.query, 245);
    if (ui::button(263, 4, 53, 30, "查询", touched, touch, true)) {
        runSearch(app); app.screen = Screen::Browse;
    }

    if (app.mode == SearchMode::Japanese) {
        for (int row = 0; row < 5; ++row) {
            auto chars = utf8Chars(kanaRows[row]);
            for (int col = 0; col < static_cast<int>(chars.size()); ++col) {
                const float x = 3 + col * 31.6f;
                const float y = 39 + row * 32.0f;
                if (ui::button(x, y, 29.5f, 29, chars[col], touched, touch)) app.query += chars[col];
            }
        }
    } else {
        for (int row = 0; row < 3; ++row) {
            const std::string letters = latinRows[row];
            const float x0 = row == 0 ? 3 : (row == 1 ? 18 : 49);
            for (int col = 0; col < static_cast<int>(letters.size()); ++col) {
                std::string letter(1, letters[col]);
                const float x = x0 + col * 31.5f;
                const float y = 55 + row * 35.0f;
                if (ui::button(x, y, 29.5f, 32, letter, touched, touch)) app.query += letter;
            }
        }
        ui::text(8, 169, 0.38f, ui::DIM, "例：学习 → xuexi，日本 → riben");
    }
    const float footerY = 201;
    if (ui::button(3, footerY, 76, 32,
                   app.mode == SearchMode::Japanese ? "日→中" : "中→日", touched, touch, true)) {
        app.mode = app.mode == SearchMode::Japanese ? SearchMode::Chinese : SearchMode::Japanese;
        app.query.clear();
    }
    if (ui::button(83, footerY, 72, 32, "删除", touched, touch)) popUtf8(app.query);
    if (ui::button(159, footerY, 72, 32, "清空", touched, touch)) app.query.clear();
    if (ui::button(235, footerY, 81, 32, "返回", touched, touch)) app.screen = Screen::Browse;
}

void drawHelpBottom(App& app, bool touched, const touchPosition& touch) {
    ui::bottom();
    ui::text(14, 18, 0.52f, ui::TEXT, "界面分工");
    ui::wrapped(14, 53, 292, 22, 0.40f, ui::DIM,
                "上屏专注显示完整词条；下屏负责搜索、候选和触摸操作。所有常用功能同时提供实体按键。", 0, 6);
    if (ui::button(84, 184, 152, 38, "返回词典", touched, touch, true)) app.screen = Screen::Browse;
}

void drawEntryBottom(App& app, bool touched, const touchPosition& touch) {
    ui::bottom();
    if (!app.results.empty()) {
        const DictEntry& entry = app.results[std::clamp(app.selected, 0, static_cast<int>(app.results.size()) - 1)];
        ui::textClipped(12, 18, 0.72f, ui::TEXT, entry.word, 296);
        ui::textClipped(13, 58, 0.45f, ui::RED, entry.reading, 294);
        ui::wrapped(13, 94, 294, 22, 0.40f, ui::DIM,
                    "C 摇杆滚动上屏释义；X 临时收藏；B 或 A 返回候选列表。", 0, 4);
    }
    if (ui::button(84, 188, 152, 38, "返回候选", touched, touch, true)) app.screen = Screen::Browse;
}

void toggleFavorite(App& app) {
    if (app.results.empty()) return;
    const uint32_t offset = app.results[std::clamp(app.selected, 0, static_cast<int>(app.results.size()) - 1)].recordOffset;
    auto it = app.favorites.find(offset);
    if (it == app.favorites.end()) app.favorites.insert(offset);
    else app.favorites.erase(it);
}

void handleButtons(App& app, uint32_t down, uint32_t repeat) {
    if (app.screen == Screen::Help) {
        if (down & (KEY_B | KEY_SELECT)) app.screen = Screen::Browse;
        return;
    }
    if (app.screen == Screen::Input) {
        if (down & KEY_B) popUtf8(app.query);
        if (down & KEY_X) app.query.clear();
        if (down & KEY_Y) {
            app.mode = app.mode == SearchMode::Japanese ? SearchMode::Chinese : SearchMode::Japanese;
            app.query.clear();
        }
        if ((down & (KEY_A | KEY_START)) && !app.query.empty()) {
            runSearch(app); app.screen = Screen::Browse;
        }
        return;
    }
    if (app.screen == Screen::Entry) {
        if (down & (KEY_A | KEY_B)) app.screen = Screen::Browse;
        if (down & KEY_X) toggleFavorite(app);
        if (down & KEY_START) app.screen = Screen::Input;
        if (down & KEY_SELECT) app.screen = Screen::Help;
        const uint32_t nav = down | repeat;
        if (nav & KEY_CSTICK_UP) app.detailScroll = std::max(0, app.detailScroll - 1);
        if (nav & KEY_CSTICK_DOWN) ++app.detailScroll;
        return;
    }
    if (down & KEY_START) app.screen = Screen::Input;
    if (down & KEY_SELECT) app.screen = Screen::Help;
    if (down & KEY_Y) {
        app.mode = app.mode == SearchMode::Japanese ? SearchMode::Chinese : SearchMode::Japanese;
        app.query.clear(); app.screen = Screen::Input;
    }
    if (down & KEY_X) toggleFavorite(app);
    if (down & KEY_B) app.screen = Screen::Input;
    if (app.results.empty()) return;
    if (down & KEY_A) app.screen = Screen::Entry;
    const int count = static_cast<int>(app.results.size());
    const uint32_t nav = down | repeat;
    if (nav & KEY_UP) app.selected = std::max(0, app.selected - 1);
    if (nav & KEY_DOWN) app.selected = std::min(count - 1, app.selected + 1);
    if ((down & KEY_L) || (nav & KEY_LEFT)) app.selected = std::max(0, app.selected - 6);
    if ((down & KEY_R) || (nav & KEY_RIGHT)) app.selected = std::min(count - 1, app.selected + 6);
    if (down & KEY_ZL) app.selected = 0;
    if (down & KEY_ZR) app.selected = count - 1;
    if (nav & KEY_CSTICK_UP) app.detailScroll = std::max(0, app.detailScroll - 1);
    if (nav & KEY_CSTICK_DOWN) ++app.detailScroll;
}
}

int main() {
    romfsInit();
    bool isNew3DS = false;
    APT_CheckNew3DS(&isNew3DS);
    if (isNew3DS) osSetSpeedupEnable(true);
    if (!ui::init()) {
        romfsExit();
        return 1;
    }
    hidSetRepeatParameters(18, 5);
    App app;
    if (isNew3DS && app.dictionary.open()) runSearch(app);
    else if (!isNew3DS) app.status = "本软件仅支持 New Nintendo 3DS / New 2DS";

    while (aptMainLoop()) {
        hidScanInput();
        const uint32_t down = hidKeysDown();
        const uint32_t held = hidKeysHeld();
        const uint32_t repeat = hidKeysDownRepeat();
        if ((held & (KEY_START | KEY_SELECT)) == (KEY_START | KEY_SELECT)) break;
        touchPosition touch{};
        hidTouchRead(&touch);
        handleButtons(app, down, repeat);

        const bool touched = (down & KEY_TOUCH) != 0;
        ui::begin();
        drawTop(app);
        if (app.screen == Screen::Input) drawInputBottom(app, touched, touch);
        else if (app.screen == Screen::Help) drawHelpBottom(app, touched, touch);
        else if (app.screen == Screen::Entry) drawEntryBottom(app, touched, touch);
        else drawBrowseBottom(app, touched, touch);
        ui::end();
    }
    app.dictionary.close();
    ui::shutdown();
    romfsExit();
    return 0;
}
