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
JA_CHARS="ui_ja_chars.txt"

# Keep the checked-in generated headers reproducible while making regeneration
# convenient on the Windows development setup used by X3/X4 contributors.
# The font files are intentionally not vendored in this repository; callers
# can always override these paths explicitly.  Git Bash accepts the native
# C:/ form, while the /c form covers MSYS installations that do not resolve
# drive-letter paths in [[ -f ]].
find_optional_font() {
  local requested="$1"
  shift
  if [[ -n "$requested" && -f "$requested" ]]; then
    printf '%s' "$requested"
    return 0
  fi
  local candidate
  for candidate in "$@"; do
    if [[ -f "$candidate" ]]; then
      printf '%s' "$candidate"
      return 0
    fi
  done
  printf '%s' "$requested"
}

NOTOSANS_SC_FONT="$(find_optional_font "${NOTOSANS_SC_FONT:-}" \
  "../../../../NotoSansSC-VF.ttf" \
  "C:/Windows/Fonts/NotoSansSC-VF.ttf" \
  "/c/Windows/Fonts/NotoSansSC-VF.ttf")"
NOTOSANS_JP_FONT="$(find_optional_font "${NOTOSANS_JP_FONT:-}" \
  "../../../../NotoSansJP-VF.ttf" \
  "C:/Windows/Fonts/NotoSansJP-VF.ttf" \
  "/c/Windows/Fonts/NotoSansJP-VF.ttf")"

for size in ${UI_FONT_SIZES[@]}; do
  for style in ${UI_FONT_STYLES[@]}; do
    font_name="ubuntu_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
    font_path="../builtinFonts/source/Ubuntu/Ubuntu-${style}.ttf"
    output_path="../builtinFonts/${font_name}.h"
    cjk_args=()
    if [[ -f "$NOTOSANS_SC_FONT" && -f "$ZH_CN_CHARS" ]]; then
      cjk_args+=("$NOTOSANS_SC_FONT" "--additional-characters" "$ZH_CN_CHARS")
    else
      echo "Warning: Simplified Chinese UI glyphs skipped for ${font_name}; set NOTOSANS_SC_FONT to Noto Sans SC." >&2
    fi
    # Japanese UI coverage is deliberately limited to kana, punctuation,
    # full-width forms and the explicit UI string list. Book text outside the
    # built-in set continues to use the SD-card font fallback.
    if [[ -f "$NOTOSANS_JP_FONT" && -f "$JA_CHARS" ]]; then
      cjk_args+=("$NOTOSANS_JP_FONT" "--additional-characters" "$JA_CHARS")
    else
      echo "Warning: Japanese UI glyphs skipped for ${font_name}; set NOTOSANS_JP_FONT to Noto Sans JP." >&2
    fi
    python fontconvert.py $font_name $size $font_path "${cjk_args[@]}" \
      --additional-intervals 0x3000,0x303F \
      --additional-intervals 0x3040,0x309F \
      --additional-intervals 0x30A0,0x30FF \
      --additional-intervals 0x31F0,0x31FF \
      --additional-intervals 0xFF00,0xFFEF > $output_path
    echo "Generated $output_path"
  done
done

small_zh_args=()
if [[ -f "$NOTOSANS_SC_FONT" && -f "$ZH_CN_CHARS" ]]; then
  small_zh_args=("$NOTOSANS_SC_FONT" "--additional-characters" "$ZH_CN_CHARS")
else
  echo "Warning: Simplified Chinese UI glyphs skipped for notosans_8_regular; set NOTOSANS_SC_FONT to Noto Sans SC." >&2
fi
small_ja_args=()
if [[ -f "$NOTOSANS_JP_FONT" && -f "$JA_CHARS" ]]; then
  small_ja_args=("$NOTOSANS_JP_FONT" "--additional-characters" "$JA_CHARS")
else
  echo "Warning: Japanese UI glyphs skipped for notosans_8_regular; set NOTOSANS_JP_FONT to Noto Sans JP." >&2
fi
python fontconvert.py notosans_8_regular 8 \
  ../builtinFonts/source/NotoSans/NotoSans-Regular.ttf \
  "${small_zh_args[@]}" "${small_ja_args[@]}" \
  --additional-intervals 0x3000,0x303F \
  --additional-intervals 0x3040,0x309F \
  --additional-intervals 0x30A0,0x30FF \
  --additional-intervals 0x31F0,0x31FF \
  --additional-intervals 0xFF00,0xFFEF > ../builtinFonts/notosans_8_regular.h

echo ""
echo "Running compression verification..."
python verify_compression.py ../builtinFonts/
