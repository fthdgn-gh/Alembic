// SPDX-License-Identifier: GPL-3.0-or-later
// Pieces of the Settings page and its dialogs.
import { useState } from "preact/hooks";
import { t } from "../../lib/i18n";
import { goBack } from "../../input/history";
import type { Hint } from "../../components/Overlay";

export const Head = (props: { text: string }) => <div class="head2"><div class="sect">{props.text}</div></div>;
export const SmallBtn = (props: { k: string; text: string; cls?: string; onClick: () => void; disabled?: boolean }) =>
  <button class={"btn sm" + (props.cls ? " " + props.cls : "")} data-key={props.k} disabled={props.disabled} onClick={props.onClick}>{props.text}</button>;

// A labelled text field of a dialog form.
export function Field(props: { label: string; value: string; onInput: (v: string) => void; placeholder?: string; type?: string; inputMode?: string }) {
  return [
    <div class="lbl">{props.label}</div>,
    <input class="input mono" spellcheck={false} autocapitalize="off" type={props.type} placeholder={props.placeholder} inputMode={props.inputMode as any}
      value={props.value} onInput={(e) => props.onInput((e.target as HTMLInputElement).value)} />,
  ];
}
// A dialog form's message and buttons.
export function useForm() {
  const [msg, setMsg] = useState<{ text: string; err: boolean }>({ text: "", err: false });
  const [busy, setBusy] = useState(false);
  return { msg, setMsg, busy, setBusy, Msg: () => <span class={"msg" + (msg.err ? " err" : "")}>{msg.text}</span> };
}
export const formHints = () => ({ hints: [["x", t("Choose")], ["o", t("Cancel"), goBack]] as Hint[] });
