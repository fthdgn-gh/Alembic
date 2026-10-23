// SPDX-License-Identifier: GPL-3.0-or-later
// The two everyday dialogs: a question (or a line of text to enter), and a
// list of choices. Both resolve with the answer, or null when cancelled.
import type { ComponentChildren } from "preact";
import { useEffect, useRef, useState } from "preact/hooks";
import { Sheet, openOverlay, closeOverlay } from "./Overlay";
import { goBack } from "../input/history";
import { t } from "../lib/i18n";
import { IS_PS5 } from "../lib/env";

export interface AskOptions { title: string; body?: ComponentChildren; input?: string; ok?: string; danger?: boolean }

// A question with OK and Cancel, or a line of text to enter: resolves with
// true, the text, or null.
export function ask(o: AskOptions): Promise<string | true | null> {
  return openOverlay<string | true>(() => <Ask {...o} />, { hints: [["x", t("Choose")], ["o", t("Cancel"), goBack]] });
}
function Ask(o: AskOptions) {
  const [value, setValue] = useState(o.input != null ? o.input : "");
  const input = useRef<HTMLInputElement>(null);
  const hasInput = o.input != null;
  // Typing first on a PC; on the console the keyboard opens only when asked.
  useEffect(() => { if (hasInput && !IS_PS5) setTimeout(() => input.current && input.current.select(), 10); }, []);
  const ok = () => closeOverlay(hasInput ? value.trim() : true);
  return (
    <Sheet title={o.title} cls="narrow" foot={<>
      <button class="btn" onClick={() => closeOverlay(null)}>{t("Cancel")}</button>
      <button class={"btn " + (o.danger ? "danger solid" : "primary")} data-autofocus={hasInput && !IS_PS5 ? undefined : ""} onClick={ok}>{o.ok || t("OK")}</button>
    </>}>
      <div>
        {o.body || null}
        {hasInput ? (
          <div style="margin-top:.6rem">
            <input ref={input} class="input" value={value} spellcheck={false} autocapitalize="off" data-autofocus={IS_PS5 ? undefined : ""}
              onInput={(e) => setValue((e.target as HTMLInputElement).value)}
              onKeyDown={(e) => { if (e.key === "Enter") { e.preventDefault(); ok(); } }} />
          </div>
        ) : null}
      </div>
    </Sheet>
  );
}

// A choice, or a heading between groups (not a choice: the D-pad passes over it).
export type ChoiceItem<T> = { label: string; sub?: string | null; value: T; selected?: boolean; danger?: boolean } | { heading: string };

// A list of choices; resolves with the chosen item's value.
export function choose<T>(title: string, items: ChoiceItem<T>[], sub?: string): Promise<T | null> {
  const firstChoice = items.findIndex((x) => !("heading" in x));
  const anySelected = items.some((x) => !("heading" in x) && x.selected);
  return openOverlay<T>(() => (
    <div class="sheet narrow">
      <div class="sh-head">{title}</div>
      {sub ? <div class="sh-sub mono">{sub}</div> : null}
      <div class="sh-body scroll">
        <div class="menu-list">
          {items.map((it, i) => "heading" in it ? <div class="menu-head">{it.heading}</div> : (
            <button class={it.danger ? "danger" : ""} data-autofocus={it.selected || (i === firstChoice && !anySelected) ? "" : undefined} onClick={() => closeOverlay(it.value)}>
              {it.label}{it.sub ? <small>{it.sub}</small> : null}
            </button>
          ))}
        </div>
      </div>
    </div>
  ), { hints: [["x", t("Choose")], ["o", t("Cancel"), goBack]] });
}
