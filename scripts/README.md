# Scripts

This repo keeps e-paper asset generators ported from the sibling `followup`
firmware repo.

## E-paper Assets

Source artwork lives in:

- `assets/icons/`
- `assets/logos/`
- `fonts/`

The generators emit monochrome C++ assets for the SSD1677 e-paper UI:

- `generate_epaper_icons.py`: fixed `36x36` icon assets
- `generate_epaper_footer_icons.py`: fixed `44x44` footer icon assets
- `generate_epaper_logos.py`: aspect-ratio-preserving logo assets
- `generate_epaper_fonts.py`: packed ASCII bitmap fonts from TTF files
- `generate_epaper_project_assets.py`: manifest-driven wrapper for all project
  image assets

The PNG generators use macOS `sips`, and the font generator uses macOS
CoreGraphics/CoreText through `ctypes`.

For normal UI work, update `assets/epaper_assets.json` and run:

```bash
python3 scripts/generate_epaper_project_assets.py
```

The lower-level scripts remain available for quick one-off experiments.

Examples:

```bash
python3 scripts/generate_epaper_icons.py \
  --output-header components/project_assets/generated_epaper_icons.h \
  --output-source components/project_assets/generated_epaper_icons.cpp \
  assets/icons/home.png:kHome \
  assets/icons/settings.png:kSettings

python3 scripts/generate_epaper_logos.py \
  --output-header components/project_assets/generated_epaper_logos.h \
  --output-source components/project_assets/generated_epaper_logos.cpp \
  assets/logos/folloup-logo.png:kFollowupLogo

python3 scripts/generate_epaper_fonts.py \
  --output components/epaper_ui/generated_epaper_fonts.cpp \
  fonts/Inter_18pt-SemiBold.ttf:kInter22SemiBold:22
```

Do not hand-edit generated asset files once they are added. Update the source
PNG/TTF files and regenerate instead.

## Bible for the SD card

`bible_json_to_sd.py` converts a YouVersion-style Bible JSON into the
`bible/<abbr>/` layout the on-device Bible reader loads (stdlib only, runs on
any OS). Bible text is copyrighted content and must never be committed here.

```bash
python3 scripts/bible_json_to_sd.py RVR1960_vid_149.json --out ./sd
# then copy ./sd/bible to the root of the SD card
```

The file format is documented at the top of the script.
