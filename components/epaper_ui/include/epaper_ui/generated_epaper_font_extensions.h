#ifndef GENERATED_EPAPER_FONT_EXTENSIONS_H
#define GENERATED_EPAPER_FONT_EXTENSIONS_H

#include "epaper_ui/bitmap_font.h"

namespace epaper_fonts {

// Latin-1 glyphs (U+00A0..U+00FF) for `font`, or nullptr when it has none.
const epaper_font::GlyphRange* FindExtension(const epaper_font::BitmapFont& font);

}  // namespace epaper_fonts

#endif  // GENERATED_EPAPER_FONT_EXTENSIONS_H
