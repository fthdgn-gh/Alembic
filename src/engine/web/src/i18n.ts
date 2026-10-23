// SPDX-License-Identifier: GPL-3.0-or-later
// Translations are in i18n.js (window.I18N), keyed by the English text;
// anything missing stays English. tools/i18n_keys.py finds the texts by
// their t() calls, so pass texts to t() as plain string literals.
import { signal } from "@preact/signals";

declare global {
  interface Window { I18N?: { messages?: string[]; [lang: string]: Record<string, string> | string[] | undefined } }
}

export const LANGS: [string, string][] = [["en", "English"], ["es", "Español"], ["fr", "Français"], ["de", "Deutsch"], ["ja", "日本語"], ["tr", "Türkçe"]];

function saved(): string | null {
  try { return JSON.parse(localStorage.getItem("alembic.lang") as string); } catch (e) { return null; }
}
function pickLang(): string {
  const s = saved();
  if (s && LANGS.some((l) => l[0] === s)) return s;
  const list = navigator.languages && navigator.languages.length ? navigator.languages : [navigator.language || "en"];
  for (const x of list) { const c = String(x).toLowerCase().slice(0, 2); if (LANGS.some((l) => l[0] === c)) return c; }
  return "en";
}

// Components that call t() redraw when it changes.
export const lang = signal(pickLang());
document.documentElement.lang = lang.value;

type Vars = Record<string, string | number>;
function table(): Record<string, string> | null {
  const d = (window.I18N || {})[lang.value];
  return d && !Array.isArray(d) ? d : null;
}

// English text in, translated text out; {name} is filled from `vars`.
export function t(text: string, vars?: Vars): string {
  const d = table();
  let out = d && d[text] != null ? d[text] : text;
  if (vars) out = out.replace(/\{(\w+)\}/g, (m, k) => (vars[k] != null ? String(vars[k]) : m));
  return out;
}

export function plural(n: number, one: string, many: string): string {
  return t(n === 1 ? one : many, { n: n.toLocaleString(lang.value) });
}

// The engine's place names: "Internal storage", "USB 0 · homebrew", ...
export function placeLabel(s: string): string {
  return String(s).replace(/^Internal storage$/, t("Internal storage")).replace(/^Internal\b/, t("Internal"))
    .replace(/^USB (\d+)/, (m, n) => t("USB {n}", { n })).replace(/^Extended (\d+)/, (m, n) => t("Extended {n}", { n }));
}

// How the engine did a conversion ("LZ4-packed and built a new exFAT image ...").
export function tMethod(m: string | undefined): string {
  if (!m) return "";
  const p = /^(LZ4-packed|LZ4-unpacked|added the AMPR emulator) and (.*)$/.exec(m);
  if (p) return t(p[1] + " and {rest}", { rest: tMethod(p[2]) });
  const c = /^(re)?compressed the (exFAT|UFS2|PFS) image unchanged$/.exec(m);
  if (c) return t(c[1] ? "recompressed the {kind} image unchanged" : "compressed the {kind} image unchanged", { kind: c[2] });
  return t(m);
}

// Another engine's paths inside messages, as people read them; set by state.ts.
let namePaths: (text: string) => string = (s) => s;
export function setPathNamer(fn: (text: string) => string) { namePaths = fn; }

// An engine message in the page's language: an exact key, else the first of
// I18N.messages (longest first) whose {name} parts match. The matched parts are
// translated too, so a system reason inside an error ("No space left on
// device") is. The engine's log keeps the English text.
export function tMsg(text: string | null | undefined): string {
  if (text == null || text === "") return "";
  text = namePaths(String(text));
  const d = table();
  if (!d) return text;
  if (d[text] != null) return d[text];
  for (const p of (window.I18N && window.I18N.messages) || []) {
    const vars = matchPattern(p, text);
    if (vars) { for (const k in vars) vars[k] = tMsg(vars[k]); return t(p, vars); }
  }
  return text;
}

// {name: text} when `text` has the shape of `pattern` ("cannot write '{path}': {reason}").
function matchPattern(pattern: string, text: string): Record<string, string> | null {
  const parts = pattern.split(/\{(\w+)\}/); // literal, name, literal, ..., literal
  if (parts.length === 1 || text.indexOf(parts[0]) !== 0) return null;
  let pos = parts[0].length;
  const vars: Record<string, string> = {};
  for (let i = 1; i < parts.length; i += 2) {
    const next = parts[i + 1], last = i + 2 >= parts.length;
    let end;
    if (last) { end = text.length - next.length; if (end <= pos || text.slice(end) !== next) return null; }
    else { end = text.indexOf(next, pos + 1); if (end < 0) return null; }
    vars[parts[i]] = text.slice(pos, end);
    pos = end + next.length;
  }
  return vars;
}

// The shortcut status: a message, then where it came from in brackets.
export function tStatus(s: string): string {
  const m = /^(.*?) \(((?:at start|added by|registered again by)[^;]*); (.*)\)$/.exec(s);
  return m ? tMsg(m[1]) + " (" + m[2] + "; " + m[3] + ")" : tMsg(s);
}
