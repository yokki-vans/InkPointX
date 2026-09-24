#!/bin/bash

set -e

cd "$(dirname "$0")"

READER_FONT_STYLES=("Regular" "Italic" "Bold" "BoldItalic")
NOTOSERIF_FONT_SIZES=(12 14 16 18)
NOTOSANS_FONT_SIZES=(12 14 16 18)

for size in ${NOTOSERIF_FONT_SIZES[@]}; do
  for style in ${READER_FONT_STYLES[@]}; do
    font_name="notoserif_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
    font_path="../builtinFonts/source/NotoSerif/NotoSerif-${style}.ttf"
    output_path="../builtinFonts/${font_name}.h"
    python fontconvert.py $font_name $size $font_path --2bit --compress --pnum > $output_path
    echo "Generated $output_path"
  done
done

for size in ${NOTOSANS_FONT_SIZES[@]}; do
  for style in ${READER_FONT_STYLES[@]}; do
    font_name="notosans_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
    font_path="../builtinFonts/source/NotoSans/NotoSans-${style}.ttf"
    if [[ "$style" = "Bold" || "$style" = "BoldItalic" ]]; then
      hebrew_path="../builtinFonts/source/NotoSansHebrew/NotoSansHebrew-Bold.ttf"
    else
      hebrew_path="../builtinFonts/source/NotoSansHebrew/NotoSansHebrew-Regular.ttf"
    fi
    output_path="../builtinFonts/${font_name}.h"
    python fontconvert.py $font_name $size $font_path $hebrew_path \
      --2bit --compress --pnum --additional-intervals 0x05D0,0x05F4 > $output_path
    echo "Generated $output_path"
  done
done

# UI glyphs are generated from every translation YAML plus bounded dynamic RTL
# ranges. This downloads build-only Inter sources and emits compact 1-bit
# Medium/SemiBold subsets; no complete UI font is embedded in the firmware.
python ../../../scripts/build_ui_fonts.py

echo ""
echo "Running compression verification..."
python verify_compression.py ../builtinFonts/
