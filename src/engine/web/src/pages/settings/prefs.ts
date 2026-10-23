// SPDX-License-Identifier: GPL-3.0-or-later
// The page's own settings, kept in this browser: text size and language.
import { api } from "../../api/client";
import { store } from "../../lib/storage";
import { lang } from "../../lib/i18n";

export const SCALES = [{ value: 0.85, label: "Small" }, { value: 1, label: "Normal" }, { value: 1.15, label: "Large" }, { value: 1.3, label: "Larger" }];
export function applyScale() {
  const w = window.innerWidth, s = store.get<number>("scale") || 1;
  const px = w >= 2400 ? 26 : w >= 1700 ? 20 : w >= 1200 ? 17 : w >= 760 ? 16 : 15;
  document.documentElement.style.fontSize = (px * s).toFixed(1) + "px";
}

export function setLang(v: string) {
  lang.value = v;
  store.set("lang", v);
  api("POST", "/api/lang", { lang: v }).catch(() => {});
  document.documentElement.lang = v;
}
