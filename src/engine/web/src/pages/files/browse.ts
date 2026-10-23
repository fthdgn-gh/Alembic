// SPDX-License-Identifier: GPL-3.0-or-later
// Moving around in Files: open a folder and put the focus where it belongs.
import type { Listing } from "../../api/types";
import { api } from "../../api/client";
import { cwd, listing, focusEntry, tab } from "../../state/view";
import { loadInfo } from "../../state/engine";
import { dir } from "../../lib/format";
import { tMsg } from "../../lib/i18n";
import { overlays } from "../../components/Overlay";
import { toast } from "../../components/Toasts";
import { focusEl, setZoneLast, visible, zoneOf } from "../../input/focus";
import { pushNav } from "../../input/history";

export async function browse(path: string, push?: boolean, focusKey?: string | null) {
  let data: Listing;
  try { data = await api<Listing>("GET", "/api/browse?path=" + encodeURIComponent(path || "/")); }
  catch (e: any) { toast(tMsg(e.message), "bad"); return; }
  const from = cwd.value;
  if (push !== false && data.path !== from) pushNav({ view: "files", path: data.path, from });
  else if (history.state && !history.state.overlay) history.replaceState(Object.assign({}, history.state, { view: "files", path: data.path }), "");
  const same = data.path === from;
  // Back from a folder: land on that folder's row.
  const a = document.activeElement;
  const key = same ? (a && a.getAttribute("data-key")) : (focusKey || (from && dir(from) === data.path ? from : null));
  cwd.value = data.path;
  listing.value = data;
  setTimeout(() => placeFocus(data, key, same), 0); // once the list is drawn
}

// keepPlace (a refresh): leave the focus where it is when it's outside the list.
function placeFocus(data: Listing, focusKey: string | null, keepPlace: boolean) {
  if (listing.value !== data) return;
  const box = document.getElementById("list");
  if (!box) return;
  box.scrollTop = 0;
  if (!data.entries.length) { focusEntry.value = null; return; }
  const rows = Array.prototype.slice.call(box.querySelectorAll(".row")) as HTMLElement[];
  const toFocus = focusKey ? rows.filter((r) => r.getAttribute("data-key") === focusKey)[0] : null;
  const first = toFocus || rows[0];
  setZoneLast("list", first);
  const a = document.activeElement as HTMLElement | null, elsewhere = !!a && a !== document.body && a.isConnected && visible(a) && zoneOf(a) !== "list";
  if (overlays.value.length || tab.value !== "files" || (keepPlace && elsewhere)) focusEntry.value = data.entries[toFocus ? data.entries.findIndex((e) => e.path === focusKey) : 0];
  else focusEl(first);
}

export function refresh() { if (cwd.value) browse(cwd.value, false); loadInfo().catch(() => {}); }
