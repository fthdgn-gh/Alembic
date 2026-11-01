// SPDX-License-Identifier: GPL-3.0-or-later
// The formats Rename suggests a game's name with (name_formats.hpp): added,
// changed, moved up or removed here, kept by the engine.
import { useEffect, useState } from "preact/hooks";
import { signal } from "@preact/signals";
import type { Letters, NameFormat, Spaces } from "../../api/types";
import { api } from "../../api/client";
import { t, tMsg } from "../../lib/i18n";
import { ICONS } from "../../components/Icons";
import { Sheet, openOverlay, closeOverlay } from "../../components/Overlay";
import { ChoiceRow, Facts } from "../../components/Rows";
import { ask } from "../../components/Dialogs";
import { toast } from "../../components/Toasts";
import { Head, SmallBtn, Field, useForm, formHints } from "./parts";

const formats = signal<NameFormat[] | null>(null);
// Patterns written out (built, so tools/i18n_keys.py does not take them for texts).
const field = (name: string) => "{" + name + "}";
const FIELDS = ["title", "titleid", "contentid", "version", "format", "platform", "name"].map(field);
const EXAMPLE = field("title") + " [" + field("titleid") + "]";
// Examples side by side, each kept on one line.
const chips = (list: string[]) => list.map((x, i) => [i ? " " : null, <span style="display:inline-block;white-space:nowrap">{x}</span>]);

const spacesOptions = (): { value: Spaces; label: string }[] => [
  { value: "keep", label: t("Keep") }, { value: "none", label: t("Remove") }, { value: "_", label: "_" }, { value: ".", label: "." }, { value: "-", label: "-" }];
const lettersOptions = (): { value: Letters; label: string }[] => [
  { value: "keep", label: t("As they are") }, { value: "upper", label: t("UPPERCASE") }, { value: "lower", label: t("lowercase") }];

async function loadFormats() {
  try { formats.value = (await api<{ formats: NameFormat[] }>("GET", "/api/names")).formats; } catch (e) { formats.value = null; }
}
// Saves the whole list; the engine checks every format and says what is wrong.
async function saveFormats(list: NameFormat[], done: string): Promise<string> {
  try {
    const plain = list.map((f) => ({ pattern: f.pattern, spaces: f.spaces, letters: f.letters }));
    formats.value = (await api<{ formats: NameFormat[] }>("POST", "/api/names", { formats: plain })).formats;
    toast(done, "ok");
    return "";
  } catch (e: any) { return tMsg(e.message); }
}

export function NameFormats() {
  useEffect(() => { loadFormats(); }, []);
  const list = formats.value;
  if (!list) return null; // an engine before rename formats
  return (
    <>
      <Head text={t("Rename formats")} />
      <div class="card">
        <p class="muted" style="margin:0 0 .5rem">{t("Rename offers a game these names, made from its title, title ID, version and format, in this order. Each shows what it makes of a sample game.")}</p>
        {list.map((f, i) => (
          <div class="ftprow">
            <div class="nm">{ICONS.file()}</div>
            <div style="min-width:0;flex:1"><div class="ellip mono">{f.pattern}</div><div class="muted ellip" style="font-size:.85rem">{f.sample || "–"}</div></div>
            <SmallBtn k={"nf:ed:" + i} text={t("Edit")} onClick={() => formatDialog(i)} />
            {i ? <SmallBtn k={"nf:up:" + i} text={t("Up")} onClick={() => { const l = list.slice(); l.splice(i - 1, 0, l.splice(i, 1)[0]); saveFormats(l, t("Format moved up")).then((m) => m && toast(m, "bad")); }} /> : null}
            <SmallBtn k={"nf:rm:" + i} cls="danger" text={t("Remove")} onClick={() => saveFormats(list.filter((_, k) => k !== i), t("Format removed")).then((m) => m && toast(m, "bad"))} />
          </div>
        ))}
        {!list.length ? <p class="muted" style="margin:0">{t("No formats: Rename asks for a name straight away.")}</p> : null}
        <div class="btns">
          <SmallBtn k="nf:reset" text={t("Restore the defaults")} onClick={resetFormats} />
          <SmallBtn k="nf:add" text={t("Add a format…")} onClick={() => formatDialog(-1)} />
        </div>
      </div>
    </>
  );
}

async function resetFormats() {
  if (!await ask({ title: t("Restore the default formats?"), body: <p class="muted">{t("Your own formats are removed.")}</p>, ok: t("Restore") })) return;
  try { formats.value = (await api<{ formats: NameFormat[] }>("POST", "/api/names", { reset: true })).formats; toast(t("Default formats restored"), "ok"); }
  catch (e: any) { toast(tMsg(e.message), "bad"); }
}

// Adds a format (index -1) or changes one.
function formatDialog(index: number) { openOverlay(() => <FormatForm index={index} />, formHints()); }
function FormatForm(props: { index: number }) {
  const old = props.index >= 0 && formats.value ? formats.value[props.index] : null;
  const [pattern, setPattern] = useState(old ? old.pattern : EXAMPLE);
  const [spaces, setSpaces] = useState<Spaces>(old ? old.spaces : "keep");
  const [letters, setLetters] = useState<Letters>(old ? old.letters : "keep");
  const [preview, setPreview] = useState<{ name?: string; error?: string }>({});
  const f = useForm();
  // What it makes of the sample game, asked again a moment after each change.
  useEffect(() => {
    let gone = false;
    const timer = setTimeout(() => {
      api<{ name?: string; error?: string }>("POST", "/api/names/preview", { pattern, spaces, letters })
        .then((p) => { if (!gone) setPreview(p); }, () => {});
    }, 250);
    return () => { gone = true; clearTimeout(timer); };
  }, [pattern, spaces, letters]);
  async function save() {
    const list = (formats.value || []).slice(), next: NameFormat = { pattern: pattern.trim(), spaces, letters };
    if (old) list[props.index] = next; else list.push(next);
    f.setBusy(true);
    const err = await saveFormats(list, old ? t("Format saved") : t("Format added"));
    if (err) { f.setMsg({ text: err, err: true }); f.setBusy(false); } else closeOverlay(true);
  }
  return (
    <Sheet title={old ? t("Edit format") : t("Add a format")} cls="narrow" foot={<><f.Msg /><button class="btn" onClick={() => closeOverlay(null)}>{t("Cancel")}</button>
      <button class="btn primary" disabled={f.busy} onClick={save}>{old ? t("Save") : t("Add")}</button></>}>
      <div>
        <Field label={t("Format")} value={pattern} onInput={setPattern} placeholder={EXAMPLE} />
        <div class={preview.error ? "msg err" : "muted mono"} style="display:block;margin:.3rem 0 .6rem;font-size:.9rem;word-break:break-word">
          {preview.error ? tMsg(preview.error) : preview.name ? t("Sample: {name}", { name: preview.name }) : " "}
        </div>
        <ChoiceRow dataKey="nf:spaces" label={t("Spaces")} options={spacesOptions()} value={spaces} onChange={setSpaces} />
        <ChoiceRow dataKey="nf:letters" label={t("Letters")} options={lettersOptions()} value={letters} onChange={setLetters} />
        <Facts rows={[
          [t("Fields"), chips(FIELDS), "mono"],
          [t("Also written"), "[titleid]", "mono"],
          [t("Modifiers"), chips(["{title:upper}", ":lower", ":nospace", ":under", ":dash", ":dot", ":20", "{version:short}"]), "mono"],
          [t("Left out when empty"), "<[v{version}]>", "mono"],
          [t("Braces as text"), "{{ }}", "mono"],
        ]} />
      </div>
    </Sheet>
  );
}
