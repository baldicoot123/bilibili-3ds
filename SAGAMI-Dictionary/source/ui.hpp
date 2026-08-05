#pragma once

#include <3ds.h>
#include <citro2d.h>
#include <cstdint>
#include <string>

namespace ui {
constexpr uint32_t BG = C2D_Color32(0x0B, 0x0B, 0x0D, 0xFF);
constexpr uint32_t PANEL = C2D_Color32(0x1B, 0x1B, 0x20, 0xFF);
constexpr uint32_t PANEL_ALT = C2D_Color32(0x27, 0x27, 0x2D, 0xFF);
constexpr uint32_t RED = C2D_Color32(0xC9, 0x00, 0x12, 0xFF);
constexpr uint32_t RED_DARK = C2D_Color32(0x7B, 0x00, 0x0A, 0xFF);
constexpr uint32_t TEXT = C2D_Color32(0xF4, 0xF1, 0xE8, 0xFF);
constexpr uint32_t DIM = C2D_Color32(0xA6, 0xA2, 0x99, 0xFF);
constexpr uint32_t BLACK = C2D_Color32(0, 0, 0, 0xFF);

bool init();
void shutdown();
void begin();
void top();
void bottom();
void end();
void rect(float x, float y, float w, float h, uint32_t color, float z = 0.3f);
void text(float x, float y, float scale, uint32_t color, const std::string& value,
          float z = 0.5f);
void textClipped(float x, float y, float scale, uint32_t color,
                 const std::string& value, float maxWidth);
float textWidth(const std::string& value, float scale);
bool button(float x, float y, float w, float h, const std::string& label,
            bool touched, const touchPosition& touch, bool selected = false);
void wrapped(float x, float y, float width, float lineHeight, float scale,
             uint32_t color, const std::string& value, int firstLine = 0,
             int maxLines = 8);
}
