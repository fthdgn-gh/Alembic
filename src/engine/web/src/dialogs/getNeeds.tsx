// SPDX-License-Identifier: GPL-3.0-or-later
import type { Plan } from "../api/types";
import { api } from "../api/client";
import { loadInfo } from "../state/engine";
import { bytes } from "../lib/format";
import { t } from "../lib/i18n";
import { ask } from "../components/Dialogs";
import { toast } from "../components/Toasts";

// Downloads the components a planned job needs, after asking. False when declined.
export async function getNeeds(plan: Plan, intro: string) {
  for (const dep of plan.needs || []) {
    const body = <div>
      <p style="margin-top:0">{t(intro, { name: t(dep.label) })}</p>
      <div class="facts"><div>{t("Why")}</div><div>{t(dep.purpose)}</div><div>{t("Source")}</div><div>{dep.source}</div><div>{t("Size")}</div><div>{bytes(dep.bytes)}</div></div>
      <p class="muted">{t("It's checked against a pinned SHA-256 before it's saved.")}</p>
    </div>;
    if (!await ask({ title: t("Download {name}?", { name: t(dep.label) }), body, ok: t("Download and continue") })) return false;
    toast(t("Downloading {name}…", { name: t(dep.label) }));
    await api("POST", "/api/deps/" + dep.id + "/install");
    toast(t("{name} downloaded", { name: t(dep.label) }), "ok");
    loadInfo().catch(() => {});
  }
  return true;
}

// ampr_emu into an existing game: a folder gets it in place, an image a
