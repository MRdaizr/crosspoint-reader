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
    output_path="../builtinFonts/${font_name}.h"
    python fontconvert.py $font_name $size $font_path --2bit --compress --pnum > $output_path
    echo "Generated $output_path"
  done
done

UI_FONT_SIZES=(10 12)
UI_FONT_STYLES=("Regular" "Bold")
ZH_CN_CHARS="ui_zh_cn_chars.txt"
NOTOSANS_SC_FONT="${NOTOSANS_SC_FONT:-../../../../NotoSansSC-VF.ttf}"

for size in ${UI_FONT_SIZES[@]}; do
  for style in ${UI_FONT_STYLES[@]}; do
    font_name="ubuntu_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
    font_path="../builtinFonts/source/Ubuntu/Ubuntu-${style}.ttf"
    hebrew_path="../builtinFonts/source/NotoSansHebrew/NotoSansHebrew-${style}.ttf"
    # Ubuntu lacks the Latin Extended Additional block (U+1EA0-U+1EF9) used for
    # Vietnamese tone marks. Append a Vietnamese-only Ubuntu cut so those glyphs
    # are filled from it while every glyph Ubuntu already has stays unchanged
    # (fontstack is ordered by descending priority).
    viet_path="../builtinFonts/source/Ubuntu/Ubuntu-Vietnamese-${style}.ttf"
    output_path="../builtinFonts/${font_name}.h"
    zh_args=()
    if [[ -f "$NOTOSANS_SC_FONT" && -f "$ZH_CN_CHARS" ]]; then
      zh_args=("$NOTOSANS_SC_FONT" "--additional-characters" "$ZH_CN_CHARS")
    else
      echo "Warning: Simplified Chinese UI glyphs skipped for ${font_name}; set NOTOSANS_SC_FONT to Noto Sans SC." >&2
    fi
    python fontconvert.py $font_name $size $font_path $hebrew_path $viet_path "${zh_args[@]}" \
      --additional-intervals 0x05D0,0x05EA > $output_path
    echo "Generated $output_path"
  done
done

small_zh_args=()
if [[ -f "$NOTOSANS_SC_FONT" && -f "$ZH_CN_CHARS" ]]; then
  small_zh_args=("$NOTOSANS_SC_FONT" "--additional-characters" "$ZH_CN_CHARS")
else
  echo "Warning: Simplified Chinese UI glyphs skipped for notosans_8_regular; set NOTOSANS_SC_FONT to Noto Sans SC." >&2
fi
python fontconvert.py notosans_8_regular 8 \
  ../builtinFonts/source/NotoSans/NotoSans-Regular.ttf \
  ../builtinFonts/source/NotoSansHebrew/NotoSansHebrew-Regular.ttf \
  "${small_zh_args[@]}" \
  --additional-intervals 0x05D0,0x05EA > ../builtinFonts/notosans_8_regular.h

echo ""
echo "Running compression verification..."
python verify_compression.py ../builtinFonts/
