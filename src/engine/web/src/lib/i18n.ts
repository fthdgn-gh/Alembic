// SPDX-License-Identifier: GPL-3.0-or-later
// Translations are one file per language, i18n.<code>.json (served at
// /i18n.<code>.json; the engine and the console app read the same files),
// keyed by the English text; anything missing stays English.
// i18n.messages.json lists what the engine sends (errors, warnings, console
// notifications) as patterns whose {name} parts match any text, longest
// first. tools/i18n_keys.py finds the texts by their t() calls, so pass texts
// to t() as plain string literals; run it with --check after changing texts
// (missing, unused or repeated keys, placeholder mismatches).
import { signal } from "@preact/signals";

const tables: Record<string, Record<string, string>> = {};
let patterns: string[] = [];

function getJson<T>(path: string): Promise<T | null> {
  return fetch(path).then((r) => (r.ok ? r.json() : null)).catch(() => null);
}

// Fetches a language's table (and the message patterns, once) if it is not
// here yet. It never fails: a language that cannot be had shows English.
let patternsLoaded = false;
export function loadTranslations(code: string): Promise<void> {
  if (code === "en" || tables[code]) return Promise.resolve();
  return Promise.all([
    getJson<Record<string, string>>("/i18n." + code + ".json"),
    patternsLoaded ? Promise.resolve(null) : getJson<string[]>("/i18n.messages.json"),
  ]).then(([table, p]) => {
    if (table) tables[code] = table;
    if (p) { patterns = p; patternsLoaded = true; }
  });
}

export const LANGS: [string, string][] = [["en", "English"], ["tr", "Türkçe"], ["de", "Deutsch"], ["es", "Español"], ["fr", "Français"], ["ru", "Русский"], ["ja", "日本語"]];

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
  return tables[lang.value] || null;
}

// English text in, translated text out; {name} is filled from `vars`.
export function t(text: string, vars?: Vars): string {
  const d = table();
  let out = d && d[text] != null ? d[text] : text;
  if (vars) out = out.replace(/\{(\w+)\}/g, (m, k) => (vars[k] != null ? String(vars[k]) : m));
  return out;
}

// Which form a count takes: Russian has three (1, 21 / 2–4, 22 / 5–20, 11–14),
// the other languages two.
export function pluralForm(code: string, n: number): "one" | "few" | "many" {
  if (code === "ru") {
    const d = n % 10, h = n % 100;
    if (d === 1 && h !== 11) return "one";
    return d >= 2 && d <= 4 && (h < 12 || h > 14) ? "few" : "many";
  }
  return n === 1 ? "one" : "many";
}

// A count in words. Where a language has more forms than English, the
// translation holds them separated by "|": the plural text's as "few|many",
// a text used for every count as "one|few|many".
export function plural(n: number, one: string, many: string): string {
  const form = pluralForm(lang.value, n);
  const parts = t(form === "one" ? one : many).split("|");
  const text = parts.length === 3 ? parts[form === "one" ? 0 : form === "few" ? 1 : 2]
    : parts.length === 2 ? parts[form === "few" ? 0 : 1] : parts[0];
  return text.replace(/\{n\}/g, n.toLocaleString(lang.value));
}

// The engine's place names: "Internal storage", "USB 0 · homebrew", ...
export function placeLabel(s: string): string {
  return String(s).replace(/^Internal storage$/, t("Internal storage")).replace(/^Internal\b/, t("Internal"))
    .replace(/^USB (\d+)/, (_m, n) => t("USB {n}", { n })).replace(/^Extended storage (\d+)/, (_m, n) => t("Extended storage {n}", { n }))
    .replace(/^Extended (\d+)/, (_m, n) => t("Extended {n}", { n }));
}

// How the engine did a conversion ("LZ4-packed and built a new exFAT image ...").
export function tMethod(m: string | undefined): string {
  if (!m) return "";
  const p = /^(fake-signed the executables|LZ4-packed|LZ4-unpacked|zstd-packed|zstd-unpacked|added the AMPR emulator) and (.*)$/.exec(m);
  if (p) return t(p[1] + " and {rest}", { rest: tMethod(p[2]) });
  const c = /^(re)?compressed the (exFAT|UFS2|PFS) image unchanged$/.exec(m);
  if (c) return t(c[1] ? "recompressed the {kind} image unchanged" : "compressed the {kind} image unchanged", { kind: c[2] });
  return t(m);
}

// Another engine's paths inside messages, as people read them; set by state.ts.
let namePaths: (text: string) => string = (s) => s;
export function setPathNamer(fn: (text: string) => string) { namePaths = fn; }

// An engine message in the page's language: an exact key, else the first of
// i18n.messages.json (longest first) whose {name} parts match. The matched parts are
// translated too, so a system reason inside an error ("No space left on
// device") is. The engine's log keeps the English text.
export function tMsg(text: string | null | undefined): string {
  if (text == null || text === "") return "";
  text = namePaths(String(text));
  const d = table();
  if (!d) return text;
  if (d[text] != null) return d[text];
  for (const p of patterns) {
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
