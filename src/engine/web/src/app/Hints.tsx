// SPDX-License-Identifier: GPL-3.0-or-later
import { tab, cwd, focusEntry } from "../state/view";
import { isConvertible } from "../lib/kinds";
import { IS_PS5 } from "../lib/env";
import { t } from "../lib/i18n";
import { Glyph } from "../components/Icons";
import { topOverlay, type Hint } from "../components/Overlay";
import { press } from "../input/controller";
import { goBack, cycleTab, leaveAll } from "../input/history";
import { openActions } from "../pages/files/actions";

// Hints come from the screen on top: the open overlay, else the current tab.
export function Hints() {
  const top = topOverlay();
  let list: Hint[];
  if (top) list = top.hints || [["x", t("Choose")], ["o", t("Back"), goBack]];
  else if (tab.value === "files") {
    const e = focusEntry.value, c = cwd.value;
    list = [["x", e ? (e.kind === "folder" ? t("Open") : isConvertible(e) ? t("Convert") : t("Options")) : t("Choose")]];
    if (c && c !== "/") list.push(["o", t("Back"), goBack]);
    if (e && c !== "/") list.push(["s", t("Options"), () => openActions(e)]);
    list.push(["lr", t("Sections"), () => cycleTab(1)]);
  } else list = [["x", t("Choose")], ["o", t("Back"), goBack], ["lr", t("Sections"), () => cycleTab(1)]];
  // Leaving is for the console's own browser (one more ○ then closes it).
  if (!top && IS_PS5) list.push(["l2r2", t("Leave"), leaveAll]);
  return (
    <footer class="hints" id="hints">
      {list.map(([b, label, fn]) => (
        <button class="hint" tabIndex={-1} data-nonav onMouseDown={(ev) => ev.preventDefault()} onClick={fn || (() => press("ok"))}>
          <span class="g"><Glyph b={b} /></span>{label}
        </button>
      ))}
    </footer>
  );
}

