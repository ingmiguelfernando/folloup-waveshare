#include "epaper_ui/bitmap_font.h"

#include <algorithm>

#include "epaper_ui/generated_epaper_font_extensions.h"

namespace epaper_font {

namespace {

uint32_t AsciiFallback(uint32_t codepoint)
{
    switch (codepoint) {
        case 0x2018:
        case 0x2019:
        case 0x201A:
        case 0x2032:
            return '\'';
        case 0x201C:
        case 0x201D:
        case 0x201E:
        case 0x2033:
            return '"';
        case 0x2010:
        case 0x2011:
        case 0x2012:
        case 0x2013:
        case 0x2014:
        case 0x2015:
        case 0x2212:
            return '-';
        case 0x2022:
            return 0xB7;
        case 0x2007:
        case 0x2009:
        case 0x200A:
        case 0x202F:
            return ' ';
        default:
            return '?';
    }
}

GlyphRef LookupDirect(const BitmapFont& font, uint32_t codepoint)
{
    if (codepoint >= font.first_char && codepoint <= font.last_char) {
        return {&font.glyphs[codepoint - font.first_char], font.bitmaps};
    }
    const GlyphRange* extension = epaper_fonts::FindExtension(font);
    if (extension != nullptr && codepoint >= extension->first && codepoint <= extension->last) {
        return {&extension->glyphs[codepoint - extension->first], extension->bitmaps};
    }
    return {};
}

}  // namespace

uint32_t NextCodepoint(std::string_view text, size_t* index)
{
    const size_t start = *index;
    const uint8_t lead = static_cast<uint8_t>(text[start]);
    if (lead < 0x80) {
        *index = start + 1;
        return lead;
    }

    size_t length = 0;
    uint32_t codepoint = 0;
    if ((lead & 0xE0) == 0xC0) {
        length = 2;
        codepoint = lead & 0x1F;
    } else if ((lead & 0xF0) == 0xE0) {
        length = 3;
        codepoint = lead & 0x0F;
    } else if ((lead & 0xF8) == 0xF0) {
        length = 4;
        codepoint = lead & 0x07;
    } else {
        *index = start + 1;
        return kReplacementCodepoint;
    }

    if (start + length > text.size()) {
        *index = text.size();
        return 0;
    }
    for (size_t offset = 1; offset < length; ++offset) {
        const uint8_t continuation = static_cast<uint8_t>(text[start + offset]);
        if ((continuation & 0xC0) != 0x80) {
            *index = start + 1;
            return kReplacementCodepoint;
        }
        codepoint = (codepoint << 6) | (continuation & 0x3F);
    }
    *index = start + length;
    return codepoint;
}

GlyphRef FindGlyph(const BitmapFont& font, uint32_t codepoint)
{
    if (codepoint >= 32) {
        const GlyphRef direct = LookupDirect(font, codepoint);
        if (direct.glyph != nullptr) {
            return direct;
        }
        const GlyphRef fallback = LookupDirect(font, AsciiFallback(codepoint));
        if (fallback.glyph != nullptr) {
            return fallback;
        }
    }
    return LookupDirect(font, '?');
}

int MeasureText(const BitmapFont& font, std::string_view text, int tracking) {
    int width = 0;
    bool first = true;

    ForEachGlyph(font, text, [&](uint32_t codepoint, const GlyphRef& ref) {
        if (codepoint == '\r' || codepoint == '\n') {
            return false;
        }
        if (ref.glyph == nullptr) {
            return true;
        }
        if (!first) {
            width += std::max(tracking, 0);
        }
        width += ref.glyph->advance;
        first = false;
        return true;
    });

    return width;
}

}  // namespace epaper_font
