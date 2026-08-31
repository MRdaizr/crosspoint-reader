#!/bin/bash

set -e

cd "$(dirname "$0")"
PYTHON_BIN="${PYTHON_BIN:-python}"

READER_FONT_STYLES=("Regular" "Italic" "Bold" "BoldItalic")
NOTOSERIF_FONT_SIZES=(12 14 16 18)
NOTOSANS_FONT_SIZES=(12 14 16 18)
READER_IPA_CHARS="dictionary_ipa_chars.txt"

# Keep the built-in reader fonts limited to the IPA characters actually used
# by the bundled dictionary/flashcard data. The complete IPA ranges remain
# available to SD-card fonts, but do not belong in every built-in reader face.

for size in ${NOTOSERIF_FONT_SIZES[@]}; do
  for style in ${READER_FONT_STYLES[@]}; do
    font_name="notoserif_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
    font_path="../builtinFonts/source/NotoSerif/NotoSerif-${style}.ttf"
    output_path="../builtinFonts/${font_name}.h"
    "$PYTHON_BIN" fontconvert.py $font_name $size $font_path \
      --additional-characters "$READER_IPA_CHARS" --2bit --compress --pnum --zopfli > $output_path
    echo "Generated $output_path"
  done
done

for size in ${NOTOSANS_FONT_SIZES[@]}; do
  for style in ${READER_FONT_STYLES[@]}; do
    font_name="notosans_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
    font_path="../builtinFonts/source/NotoSans/NotoSans-${style}.ttf"
    output_path="../builtinFonts/${font_name}.h"
    "$PYTHON_BIN" fontconvert.py $font_name $size $font_path \
      --additional-characters "$READER_IPA_CHARS" --2bit --compress --pnum --zopfli > $output_path
    echo "Generated $output_path"
  done
done

UI_FONT_SIZES=(10 12)
# Keep the existing generated/API name "regular" for the Medium face so font
# IDs and the C++ registration code remain stable.
UI_FONT_STYLES=("Medium" "Bold")
ZH_CN_CHARS="ui_zh_cn_chars.txt"
JA_CHARS="ui_ja_chars.txt"

# UI faces intentionally contain only the English/Chinese/Japanese set.  The
# CJK fallback fonts also provide Vietnamese and Hebrew codepoints, so exclude
# those ranges explicitly instead of relying on the font stack to omit them.
UI_EXCLUDE_INTERVALS=(
  "0x0590,0x05FF"   # Hebrew
  "0x01A0,0x01A1"   # Vietnamese Latin extension
  "0x01AF,0x01B0"   # Vietnamese Latin extension
  "0x1EA0,0x1EF9"   # Vietnamese precomposed letters
)
UI_EXCLUDE_ARGS=()
for interval in "${UI_EXCLUDE_INTERVALS[@]}"; do
  UI_EXCLUDE_ARGS+=(--exclude-intervals "$interval")
done

# Keep the checked-in generated headers reproducible while making regeneration
# convenient on the Windows development setup used by X3/X4 contributors.
# Ubuntu faces are vendored so the generated UI headers are reproducible.
# CJK fallback fonts are resolved from the local Windows setup and can still
# be overridden explicitly. Git Bash accepts the native C:/ form, while the
# /c form covers MSYS installations that do not resolve drive-letter paths in
# [[ -f ]].
font_exists() {
  if [[ -f "$1" ]]; then
    return 0
  fi
  # When the script is launched from WSL, let Windows Python still receive a
  # native C:/ path while checking the mounted equivalent for existence.
  if [[ "$1" == C:/* && -f "/mnt/c/${1:3}" ]]; then
    return 0
  fi
  return 1
}

find_optional_font() {
  local requested="$1"
  shift
  if [[ -n "$requested" ]] && font_exists "$requested"; then
    printf '%s' "$requested"
    return 0
  fi
  local candidate
  for candidate in "$@"; do
    if font_exists "$candidate"; then
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
    if [ "$style" = "Medium" ]; then
      font_name="ubuntu_${size}_regular"
    else
      font_name="ubuntu_${size}_bold"
    fi
    font_path="../builtinFonts/source/Ubuntu/Ubuntu-${style}.ttf"
    output_path="../builtinFonts/${font_name}.h"
    font_stack=("$font_path")
    char_args=()
    if font_exists "$NOTOSANS_SC_FONT" && [[ -f "$ZH_CN_CHARS" ]]; then
      font_stack+=("$NOTOSANS_SC_FONT")
      char_args+=(--additional-characters "$ZH_CN_CHARS")
    else
      echo "Warning: Simplified Chinese UI glyphs skipped for ${font_name}; set NOTOSANS_SC_FONT to Noto Sans SC." >&2
    fi
    # Japanese UI coverage comes from the static menu string list below. Do
    # not include the complete kana blocks: book text outside the built-in set
    # continues to use the SD-card font fallback.
    if font_exists "$NOTOSANS_JP_FONT" && [[ -f "$JA_CHARS" ]]; then
      font_stack+=("$NOTOSANS_JP_FONT")
      char_args+=(--additional-characters "$JA_CHARS")
    elif font_exists "$NOTOSANS_SC_FONT" && [[ -f "$JA_CHARS" ]]; then
      # Noto Sans SC also contains the kana used by the built-in Japanese UI;
      # keep those glyphs when a separate JP font is unavailable.
      char_args+=(--additional-characters "$JA_CHARS")
    else
      echo "Warning: Japanese UI glyphs skipped for ${font_name}; set NOTOSANS_JP_FONT to Noto Sans JP." >&2
    fi
    # Keep all positional font paths together. argparse stops the fontstack
    # positional at the first option, so interleaving --additional-characters
    # between fallback fonts would silently drop the later font.
    "$PYTHON_BIN" fontconvert.py $font_name $size "${font_stack[@]}" \
      --mono \
      "${UI_EXCLUDE_ARGS[@]}" \
      "${char_args[@]}" \
      --additional-intervals 0x3000,0x303F \
      --additional-intervals 0xFF00,0xFF65 \
      --additional-intervals 0xFFA0,0xFFEF > $output_path
    echo "Generated $output_path"
  done
done

small_font_stack=(../builtinFonts/source/NotoSans/NotoSans-Regular.ttf)
small_char_args=()
if font_exists "$NOTOSANS_SC_FONT" && [[ -f "$ZH_CN_CHARS" ]]; then
  small_font_stack+=("$NOTOSANS_SC_FONT")
  small_char_args+=(--additional-characters "$ZH_CN_CHARS")
else
  echo "Warning: Simplified Chinese UI glyphs skipped for notosans_8_regular; set NOTOSANS_SC_FONT to Noto Sans SC." >&2
fi
if font_exists "$NOTOSANS_JP_FONT" && [[ -f "$JA_CHARS" ]]; then
  small_font_stack+=("$NOTOSANS_JP_FONT")
  small_char_args+=(--additional-characters "$JA_CHARS")
elif font_exists "$NOTOSANS_SC_FONT" && [[ -f "$JA_CHARS" ]]; then
  small_char_args+=(--additional-characters "$JA_CHARS")
else
  echo "Warning: Japanese UI glyphs skipped for notosans_8_regular; set NOTOSANS_JP_FONT to Noto Sans JP." >&2
fi
"$PYTHON_BIN" fontconvert.py notosans_8_regular 8 "${small_font_stack[@]}" \
  "${UI_EXCLUDE_ARGS[@]}" \
  "${small_char_args[@]}" \
  --additional-intervals 0x3000,0x303F \
  --additional-intervals 0xFF00,0xFF65 \
  --additional-intervals 0xFFA0,0xFFEF > ../builtinFonts/notosans_8_regular.h

echo ""
echo "Running compression verification..."
"$PYTHON_BIN" verify_compression.py ../builtinFonts/
