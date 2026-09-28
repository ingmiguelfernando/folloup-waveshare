#!/usr/bin/env python3
"""Generate Latin-1 (U+00A0..U+00FF) glyph extensions for the existing e-paper fonts.

The ASCII tables in generated_epaper_fonts.cpp are kept byte-for-byte; this adds
accented letters, ñ, ¿, ¡, «, » etc. as separate tables looked up by
epaper_fonts::FindExtension(). Spec format matches generate_epaper_fonts.py.

Usage:
  python3 scripts/generate_epaper_font_extensions.py \
    --output components/epaper_ui/generated_epaper_font_extensions.cpp \
    fonts/Inter_18pt-SemiBold.ttf:kInter22SemiBold:22 ...
"""

from __future__ import annotations

import sys

from generate_epaper_fonts import (
    FontGenError,
    emit_byte_array,
    load_font,
    parse_args,
    render_glyph,
)

kExtensionFirst = 0xA0
kExtensionLast = 0xFF


def generate_source(specs) -> str:
    out = [
        '#include "epaper_ui/generated_epaper_font_extensions.h"',
        "",
        '#include "epaper_ui/generated_epaper_fonts.h"',
        "",
        "namespace epaper_fonts {",
        "namespace {",
        "",
    ]

    for spec in specs:
        font = load_font(spec)
        bitmap_bytes: list[int] = []
        glyph_lines: list[str] = []
        for code in range(kExtensionFirst, kExtensionLast + 1):
            glyph = render_glyph(font, code)
            offset = len(bitmap_bytes)
            bitmap_bytes.extend(glyph.bitmap)
            glyph_lines.append(
                "    { %5u, %4u, %3u, %3u, %4d, %4d, %3u },"
                % (
                    offset,
                    len(glyph.bitmap),
                    glyph.width,
                    glyph.height,
                    glyph.bearing_x,
                    glyph.bearing_y,
                    glyph.advance,
                )
            )

        out.extend(
            [
                f"const epaper_font::GlyphBitmap {spec.symbol}_ext_glyphs[] = {{",
                *glyph_lines,
                "};",
                "",
                f"const uint8_t {spec.symbol}_ext_bitmaps[] = {{",
            ]
        )
        if bitmap_bytes:
            out.append(emit_byte_array(bitmap_bytes, indent="    "))
        out.extend(
            [
                "};",
                "",
                f"const epaper_font::GlyphRange {spec.symbol}_ext = {{",
                f"    0x{kExtensionFirst:02X},",
                f"    0x{kExtensionLast:02X},",
                f"    {spec.symbol}_ext_glyphs,",
                f"    {spec.symbol}_ext_bitmaps,",
                "};",
                "",
            ]
        )

    out.extend(
        [
            "}  // namespace",
            "",
            "const epaper_font::GlyphRange* FindExtension(const epaper_font::BitmapFont& font)",
            "{",
        ]
    )
    for spec in specs:
        out.append(f"    if (&font == &{spec.symbol}) {{")
        out.append(f"        return &{spec.symbol}_ext;")
        out.append("    }")
    out.extend(
        [
            "    return nullptr;",
            "}",
            "",
            "}  // namespace epaper_fonts",
            "",
        ]
    )
    return "\n".join(out)


def main(argv: list[str]) -> int:
    try:
        output_path, specs = parse_args(argv)
        output_path.write_text(generate_source(specs), encoding="utf-8")
        return 0
    except FontGenError as exc:
        sys.stderr.write(f"font extension generation failed: {exc}\n")
        return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
