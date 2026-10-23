// SPDX-License-Identifier: GPL-3.0-or-later
import { useEffect, useState } from "preact/hooks";
import { t } from "../../lib/i18n";
import { Sheet, openOverlay, closeOverlay } from "../../components/Overlay";
import { inputLog } from "../../input/controller";

// Shows what each button sends; nothing acts while it is open, except ✕ on Close.
export function openControllerTest() {
  openOverlay(() => <ControllerTest />, { hints: [["x", t("Close (press it last)")]] });
}
function ControllerTest() {
  // What came before it opened (the press that opened it, too) is not shown:
  // the log's newest entry then marks where to stop.
  const [start] = useState(Date.now());
  const [before] = useState(() => inputLog.items[0]);
  const [, redraw] = useState(0);
  useEffect(() => {
    inputLog.test = () => redraw((n) => n + 1);
    return () => { inputLog.test = null; };
  }, []);
  const end = inputLog.items.indexOf(before);
  const items = end < 0 ? inputLog.items : inputLog.items.slice(0, end);
  return (
    <Sheet title={t("Controller test")} cls="wide" foot={
      <button class="btn primary" data-test-done data-autofocus onClick={() => { inputLog.test = null; inputLog.closing = true; closeOverlay(null); }}>{t("Close")}</button>}>
      <div>
        <p style="margin-top:0">{t("Press each button once: D-pad, ○, △, □, L1, R1, L2, R2, Options. Each one shows below what the browser sends. Press ✕ last: it closes this.")}</p>
        <div class="card" style="min-height:16rem">
          <div class="mono" style="font-size:.85rem;line-height:1.7">
            {!items.length ? <div class="muted">{t("Waiting for a button…")}</div> : null}
            {items.slice(0, 16).map((x) => <div>{(Math.max(0, x.t - start) / 1000).toFixed(1) + " s  " + x.text}</div>)}
          </div>
        </div>
      </div>
    </Sheet>
  );
}
