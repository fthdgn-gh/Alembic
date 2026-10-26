// SPDX-License-Identifier: GPL-3.0-or-later
// A game's platform: PS5 (white, black text) or PS4 (black, white text).
// Nothing for anything else (no param.json or param.sfo, or an older engine).
import type { Meta } from "../api/types";

export function PlatformPill(props: { meta: Meta | null | undefined }) {
  const p = props.meta && props.meta.platform;
  return p === "ps4" || p === "ps5" ? <span class={"fmt " + p}>{p.toUpperCase()}</span> : null;
}
