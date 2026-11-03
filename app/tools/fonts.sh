# SPDX-License-Identifier: GPL-3.0-or-later
# Sourced by app/build.sh and app/preview.sh: bakes the app's fonts (accented
# Latin, Greek and Cyrillic: the kit's "european" set; and Japanese, below)
# into $work/fonts.
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
