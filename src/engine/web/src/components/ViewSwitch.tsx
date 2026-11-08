// SPDX-License-Identifier: GPL-3.0-or-later
// List or grid: the two buttons above Files' and Images' contents.
import { viewModes, setViewMode, type ViewMode } from "../state/view";
import { t } from "../lib/i18n";

const LIST = <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M8 6h12M8 12h12M8 18h12" /><circle cx="4" cy="6" r=".6" /><circle cx="4" cy="12" r=".6" /><circle cx="4" cy="18" r=".6" /></svg>;
const GRID = <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linejoin="round"><rect x="4" y="4" width="6.5" height="6.5" rx="1.2" /><rect x="13.5" y="4" width="6.5" height="6.5" rx="1.2" /><rect x="4" y="13.5" width="6.5" height="6.5" rx="1.2" /><rect x="13.5" y="13.5" width="6.5" height="6.5" rx="1.2" /></svg>;

export function ViewSwitch(props: { which: "files" | "images" }) {
  const mode = viewModes.value[props.which];
  const button = (m: ViewMode, icon: any, label: string) => (
    <button class={"btn sm vs" + (mode === m ? " on" : "")} data-key={props.which + ":view-" + m} title={label} aria-pressed={mode === m}
      onClick={() => setViewMode(props.which, m)}>{icon}</button>
  );
  return <span class="viewswitch">{button("list", LIST, t("List"))}{button("grid", GRID, t("Grid"))}</span>;
}
