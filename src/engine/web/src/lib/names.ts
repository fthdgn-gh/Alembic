// SPDX-License-Identifier: GPL-3.0-or-later
// Names suggested when renaming a game, from its title, title ID, version and
// format. The app has the same rules (app/src/app/actions.cpp rename_suggestions).
import type { Entry, Meta } from "../api/types";
import { KIND_LABEL } from "./kinds";
import { t } from "./i18n";

// A title made safe for every file system the engine writes to (exFAT, SMB,
// Windows): no / \ * ? " < > | or control characters, ":" becomes " - ".
export function safeName(s: string): string {
  return s.replace(/[\u0000-\u001f\u007f]/g, " ").replace(/\s*:\s*/g, " - ").replace(/[\\/*?"<>|]/g, "")
    .replace(/\s+/g, " ").replace(/^[\s.]+|[\s.]+$/g, "");
}

// The suggestions, each with what it is made of.
// A hidden name (".name") stays hidden; a file keeps its extension.
export function nameSuggestions(e: Entry, m: Meta): { name: string; format: string }[] {
  const title = safeName(m.title || ""), id = safeName(m.title_id || ""), ver = safeName(m.version || "");
  if (!title) return [];
  const hidden = e.name.length > 1 && e.name[0] === "." ? "." : "";
  const plain = e.name.slice(hidden.length), dot = plain.lastIndexOf(".");
  const ext = e.kind !== "folder" && dot > 0 ? plain.slice(dot) : "";
  const kind = e.kind !== "folder" ? KIND_LABEL[e.kind] || "" : "";
  const all: { name: string; format: string }[] = [];
  const add = (name: string, format: string) => {
    const full = hidden + name + ext;
    if (full !== e.name && !all.some((s) => s.name === full)) all.push({ name: full, format });
  };
  if (id) {
    add(title + " [" + id + "]", t("Title [title ID]"));
    if (ver) add(title + " [" + id + "] [v" + ver + "]", t("Title [title ID] [version]"));
    if (kind) add(title + " [" + id + "] [" + kind + "]", t("Title [title ID] [format]"));
    add(id + " - " + title, t("Title ID - title"));
    add(id, t("Title ID"));
  } else add(title, t("Title"));
  return all;
}
