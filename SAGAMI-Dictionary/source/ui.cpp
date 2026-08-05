#include "ui.hpp"

#include <algorithm>
#include <vector>

namespace ui {
namespace {
C3D_RenderTarget* sTop = nullptr;
C3D_RenderTarget* sBottom = nullptr;
C2D_TextBuf sText = nullptr;
C2D_TextBuf sMeasure = nullptr;
C2D_Font sFont = nullptr;

std::vector<std::string> splitUtf8(const std::string& textValue, std::size_t charsPerLine) {
    std::vector<std::string> lines;
    std::string line;
    std::size_t chars = 0;
    for (std::size_t i = 0; i < textValue.size();) {
        const unsigned char c = static_cast<unsigned char>(textValue[i]);
        std::size_t len = c < 0x80 ? 1 : ((c & 0xE0) == 0xC0 ? 2 : ((c & 0xF0) == 0xE0 ? 3 : 4));
        if (i + len > textValue.size()) len = 1;
        std::string cp = textValue.substr(i, len);
        i += len;
        if (cp == "\n") {
            lines.push_back(line);
            line.clear();
            chars = 0;
            continue;
        }
        line += cp;
        ++chars;
        const bool punctuation = cp == "。" || cp == "；" || cp == ";" || cp == "，";
        if (chars >= charsPerLine || (punctuation && chars > charsPerLine / 2)) {
            lines.push_back(line);
            line.clear();
            chars = 0;
        }
    }
    if (!line.empty()) lines.push_back(line);
    return lines;
}
}

bool init() {
    gfxInitDefault();
    if (!C3D_Init(C3D_DEFAULT_CMDBUF_SIZE)) return false;
    if (!C2D_Init(C2D_DEFAULT_MAX_OBJECTS)) return false;
    C2D_Prepare();
    sTop = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    sBottom = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    sText = C2D_TextBufNew(4096);
    sMeasure = C2D_TextBufNew(128);
    sFont = C2D_FontLoad("romfs:/font.bcfnt");
    if (!sFont) sFont = C2D_FontLoadSystem(CFG_REGION_CHN);
    if (!sFont) sFont = C2D_FontLoadSystem(CFG_REGION_TWN);
    return sTop && sBottom && sText && sMeasure;
}

void shutdown() {
    if (sFont) C2D_FontFree(sFont);
    if (sMeasure) C2D_TextBufDelete(sMeasure);
    if (sText) C2D_TextBufDelete(sText);
    C2D_Fini();
    C3D_Fini();
    gfxExit();
}

void begin() {
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C2D_TextBufClear(sText);
}
void top() {
    C2D_SceneBegin(sTop);
    C2D_TargetClear(sTop, BG);
}
void bottom() {
    C2D_SceneBegin(sBottom);
    C2D_TargetClear(sBottom, BG);
}
void end() { C3D_FrameEnd(0); }

void rect(float x, float y, float w, float h, uint32_t color, float z) {
    C2D_DrawRectSolid(x, y, z, w, h, color);
}

void text(float x, float y, float scale, uint32_t color, const std::string& value, float z) {
    if (value.empty()) return;
    C2D_Text t;
    if (!C2D_TextFontParse(&t, sFont, sText, value.c_str())) return;
    C2D_TextOptimize(&t);
    C2D_DrawText(&t, C2D_WithColor, x, y, z, scale, scale, color);
}

float textWidth(const std::string& value, float scale) {
    if (value.empty()) return 0;
    C2D_TextBufClear(sMeasure);
    C2D_Text t;
    if (!C2D_TextFontParse(&t, sFont, sMeasure, value.c_str())) return 0;
    float w = 0, h = 0;
    C2D_TextGetDimensions(&t, scale, scale, &w, &h);
    return w;
}

void textClipped(float x, float y, float scale, uint32_t color,
                 const std::string& value, float maxWidth) {
    if (textWidth(value, scale) <= maxWidth) {
        text(x, y, scale, color, value);
        return;
    }
    std::string clipped;
    for (std::size_t i = 0; i < value.size();) {
        const unsigned char c = static_cast<unsigned char>(value[i]);
        std::size_t len = c < 0x80 ? 1 : ((c & 0xE0) == 0xC0 ? 2 : ((c & 0xF0) == 0xE0 ? 3 : 4));
        if (i + len > value.size()) len = 1;
        const std::string next = clipped + value.substr(i, len) + "…";
        if (textWidth(next, scale) > maxWidth) break;
        clipped += value.substr(i, len);
        i += len;
    }
    text(x, y, scale, color, clipped + "…");
}

bool button(float x, float y, float w, float h, const std::string& label,
            bool touched, const touchPosition& touch, bool selected) {
    const bool hit = touched && touch.px >= x && touch.px < x + w &&
                     touch.py >= y && touch.py < y + h;
    rect(x, y, w, h, hit ? RED : (selected ? RED_DARK : PANEL_ALT));
    rect(x, y + h - 2, w, 2, BLACK);
    const float scale = 0.48f;
    const float tw = textWidth(label, scale);
    text(x + std::max(3.0f, (w - tw) / 2.0f), y + 4, scale, TEXT, label);
    return hit;
}

void wrapped(float x, float y, float width, float lineHeight, float scale,
             uint32_t color, const std::string& value, int firstLine, int maxLines) {
    const std::size_t chars = static_cast<std::size_t>(std::max(8.0f, width / 16.0f));
    const auto lines = splitUtf8(value, chars);
    for (int n = 0; n < maxLines && firstLine + n < static_cast<int>(lines.size()); ++n)
        text(x, y + n * lineHeight, scale, color, lines[firstLine + n]);
}
}
