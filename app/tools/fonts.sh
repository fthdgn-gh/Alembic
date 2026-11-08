# SPDX-License-Identifier: GPL-3.0-or-later
# Sourced by app/build.sh and app/preview.sh: bakes the app's fonts (accented
# Latin, Greek and Cyrillic: the kit's "european" set; Japanese and Chinese,
# below) into $work/fonts.
fonts="$work/fonts"
mkdir -p "$fonts" "$work/host"
baker="$work/host/bake_font"
[[ -x $baker && $baker -nt $kit/tools/font-baker/bake_font.cpp ]] ||
    "$HOST_CXX" -std=c++20 -O2 -w "$kit/tools/font-baker/bake_font.cpp" -o "$baker"
while read -r source output size range; do
    [[ -f $fonts/$output.huifont && $fonts/$output.huifont -nt $baker ]] && continue
    "$baker" "$kit/third_party/fonts/$source" "$fonts/$output.huifont" "$size" "$range" 2048 european >/dev/null
done < "$app/fonts.txt"

# Japanese: IBM Plex Sans JP (SIL OFL 1.1) from google/fonts, pinned to a
# commit and SHA-256, baked with the characters app/tools/japanese_glyphs.py
# lists, Regular and SemiBold (Inter's weights). The app draws them wherever
# Inter or DejaVu has no glyph. TrueType outlines: stb_truetype's distance
# fields skip the cubic curves of CFF fonts (Noto Sans JP's OTF lost them).
plex_commit=2fe1e8c6b137fb13041a7cfca6fdcd8a910ea4dc
plex_dir="$work/ibm-plex-sans-jp"
mkdir -p "$plex_dir"
for plex_spec in "IBMPlexSansJP-Regular.ttf 372f8bba95f386856ae435dc0e69f08db1cac29bed513d99171a0ae2d307ba2a" \
            "IBMPlexSansJP-SemiBold.ttf 5985c29d2d5d444072915cf5874dc80e0a3f5fadbf14a9e3013a03a8fe6d7ff5" \
            "OFL.txt 7e6b2818edbd8f6a01ae80641cc8f16a51080d08fb4e532be3a0b6f74adb07da"; do
    read -r plex_name plex_sha <<<"$plex_spec"
    plex_file="$plex_dir/$plex_name"
    if [[ ! -f $plex_file ]] || ! echo "$plex_sha  $plex_file" | sha256sum --check --strict --quiet 2>/dev/null; then
        curl -fsSL -o "$plex_file.part" "https://raw.githubusercontent.com/google/fonts/$plex_commit/ofl/ibmplexsansjp/$plex_name"
        echo "$plex_sha  $plex_file.part" | sha256sum --check --strict --quiet || { echo "$plex_name: unexpected SHA-256" >&2; exit 1; }
        mv "$plex_file.part" "$plex_file"
    fi
done
cp "$plex_dir/OFL.txt" "$fonts/IBMPlexSansJP-LICENSE.txt"
python3 "$app/tools/japanese_glyphs.py" "$repo/src/engine/web/i18n.ja.json" >"$plex_dir/glyphs.txt.new"
cmp -s "$plex_dir/glyphs.txt.new" "$plex_dir/glyphs.txt" || mv "$plex_dir/glyphs.txt.new" "$plex_dir/glyphs.txt"
rm -f "$plex_dir/glyphs.txt.new"
# (Sourced: names here must not clash with the scripts' own, such as $out.)
for plex_weight in Regular SemiBold; do
    plex_font="$fonts/ibm-plex-sans-jp-$(tr '[:upper:]' '[:lower:]' <<<"$plex_weight").huifont"
    [[ -f $plex_font && ! $plex_font -ot $baker && ! $plex_font -ot $plex_dir/glyphs.txt ]] && continue
    "$baker" "$plex_dir/IBMPlexSansJP-$plex_weight.ttf" "$plex_font" 48 6 4096 "file:$plex_dir/glyphs.txt" 2>/dev/null >/dev/null
done

# Simplified Chinese: IBM Plex Sans SC (SIL OFL 1.1) from IBM/plex, pinned to a
# commit and SHA-256 (unhinted TrueType), baked like the Japanese face with
# the characters app/tools/chinese_glyphs.py lists. With the page in Chinese
# it comes before the Japanese face, so shared hanzi take their Chinese shapes.
plexsc_commit=48e8676114c04f29033b4b53da4ec579f8846297
plexsc_dir="$work/ibm-plex-sans-sc"
mkdir -p "$plexsc_dir"
for plexsc_spec in "IBMPlexSansSC-Regular.ttf 0b8b5c398c3198ec8932f3d3eb5eea0b3e7ff4f6cb5b84fcde180ad890094d5e" \
            "IBMPlexSansSC-SemiBold.ttf a785ab071ae8d75865c8aa9adc74fe6e5af67a9ba7cff76ca533dafba8cb7b2d" \
            "license.txt 91c25c350d3cac39da2736d74f7ba37ef648f5237a4e330a240615bc8d8c4360"; do
    read -r plexsc_name plexsc_sha <<<"$plexsc_spec"
    plexsc_file="$plexsc_dir/$plexsc_name"
    if [[ ! -f $plexsc_file ]] || ! echo "$plexsc_sha  $plexsc_file" | sha256sum --check --strict --quiet 2>/dev/null; then
        curl -fsSL -o "$plexsc_file.part" "https://raw.githubusercontent.com/IBM/plex/$plexsc_commit/packages/plex-sans-sc/fonts/complete/ttf/unhinted/$plexsc_name"
        echo "$plexsc_sha  $plexsc_file.part" | sha256sum --check --strict --quiet || { echo "$plexsc_name: unexpected SHA-256" >&2; exit 1; }
        mv "$plexsc_file.part" "$plexsc_file"
    fi
done
cp "$plexsc_dir/license.txt" "$fonts/IBMPlexSansSC-LICENSE.txt"
python3 "$app/tools/chinese_glyphs.py" "$repo/src/engine/web/i18n.zh.json" >"$plexsc_dir/glyphs.txt.new"
cmp -s "$plexsc_dir/glyphs.txt.new" "$plexsc_dir/glyphs.txt" || mv "$plexsc_dir/glyphs.txt.new" "$plexsc_dir/glyphs.txt"
rm -f "$plexsc_dir/glyphs.txt.new"
for plexsc_weight in Regular SemiBold; do
    plexsc_font="$fonts/ibm-plex-sans-sc-$(tr '[:upper:]' '[:lower:]' <<<"$plexsc_weight").huifont"
    [[ -f $plexsc_font && ! $plexsc_font -ot $baker && ! $plexsc_font -ot $plexsc_dir/glyphs.txt ]] && continue
    "$baker" "$plexsc_dir/IBMPlexSansSC-$plexsc_weight.ttf" "$plexsc_font" 48 6 4096 "file:$plexsc_dir/glyphs.txt" 2>/dev/null >/dev/null
done
