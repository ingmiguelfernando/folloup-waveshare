#ifndef BITMAP_FONT_H
#define BITMAP_FONT_H

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace epaper_font {

struct GlyphBitmap {
    uint32_t bitmap_offset;
    uint16_t bitmap_byte_count;
    uint8_t width;
    uint8_t height;
    int16_t bearing_x;
    int16_t bearing_y;
    uint8_t advance;
};

struct BitmapFont {
    const char* name;
    uint8_t first_char;
    uint8_t last_char;
    uint8_t line_height;
    uint8_t ascent;
    const GlyphBitmap* glyphs;
    const uint8_t* bitmaps;
};

// Extra glyphs beyond a font's ASCII table (e.g. Latin-1), with their own bitmap pool.
struct GlyphRange {
    uint16_t first;
    uint16_t last;
    const GlyphBitmap* glyphs;
    const uint8_t* bitmaps;
};

struct GlyphRef {
    const GlyphBitmap* glyph = nullptr;
    const uint8_t* bitmaps = nullptr;
};

inline constexpr uint32_t kReplacementCodepoint = 0xFFFD;
inline constexpr uint32_t kEllipsisCodepoint = 0x2026;

// Decodes the UTF-8 sequence at *index and advances past it. Invalid bytes yield
// U+FFFD; a sequence cut off at the end of `text` yields 0 so truncation is invisible.
uint32_t NextCodepoint(std::string_view text, size_t* index);

// Unsupported codepoints resolve to typographic ASCII equivalents, then to '?'.
GlyphRef FindGlyph(const BitmapFont& font, uint32_t codepoint);

// Calls fn(codepoint, glyph_ref) per rendered glyph; fn returns false to stop.
template <typename Fn>
void ForEachGlyph(const BitmapFont& font, std::string_view text, Fn&& fn)
{
    size_t index = 0;
    while (index < text.size()) {
        const uint32_t codepoint = NextCodepoint(text, &index);
        if (codepoint == 0) {
            continue;
        }
        if (codepoint == kEllipsisCodepoint) {
            const GlyphRef dot = FindGlyph(font, '.');
            for (int repeat = 0; repeat < 3; ++repeat) {
                if (!fn(codepoint, dot)) {
                    return;
                }
            }
            continue;
        }
        if (!fn(codepoint, FindGlyph(font, codepoint))) {
            return;
        }
    }
}

int MeasureText(const BitmapFont& font, std::string_view text, int tracking = 0);

}  // namespace epaper_font

#endif  // BITMAP_FONT_H
