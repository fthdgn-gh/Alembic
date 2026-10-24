# SPDX-License-Identifier: GPL-3.0-or-later
# Sourced by app/build.sh and app/preview.sh: bakes the app's fonts (accented
# Latin, Greek and Cyrillic: the kit's "european" set) into $work/fonts.
fonts="$work/fonts"
mkdir -p "$fonts" "$work/host"
baker="$work/host/bake_font"
[[ -x $baker && $baker -nt $kit/tools/font-baker/bake_font.cpp ]] ||
    "$HOST_CXX" -std=c++20 -O2 -w "$kit/tools/font-baker/bake_font.cpp" -o "$baker"
while read -r source output size range; do
    [[ -f $fonts/$output.huifont && $fonts/$output.huifont -nt $baker ]] && continue
    "$baker" "$kit/third_party/fonts/$source" "$fonts/$output.huifont" "$size" "$range" 2048 european >/dev/null
done < "$app/fonts.txt"
