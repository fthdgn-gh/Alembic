// SPDX-License-Identifier: GPL-3.0-or-later
// What a job is, in words: its title, where it goes, its checkpoint.
import type { Job } from "../../api/types";
import { shownPath } from "../../state/paths";
import { base } from "../../lib/format";
import { t } from "../../lib/i18n";

export function jobText(j: Job) {
  const s = j.spec;
  if (s.kind === "copy" || s.kind === "move") return { title: base(s.input), to: t(s.kind === "copy" ? "Copy to {path}" : "Move to {path}", { path: shownPath(s.output) }) };
  if (s.kind === "install") return { title: base(s.input), to: t("Install on this console") };
  if (s.kind === "ampr_emu") return { title: base(s.input), to: j.result && j.result.output ? t("AMPR emulator into {path}", { path: shownPath(j.result.output) }) : t("Add the AMPR emulator") };
  return { title: base(s.input), to: "→ " + shownPath(s.output) };
}
// Where a waiting job continues from, in percent (null without a checkpoint).
export const cpPct = (j: Job) => (j.checkpoint && j.checkpoint.total ? Math.floor(100 * j.checkpoint.done / j.checkpoint.total) : null);
export const cpText = (j: Job) => (cpPct(j) == null ? "" : t("{n}%", { n: cpPct(j)! }));
