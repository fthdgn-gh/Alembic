// SPDX-License-Identifier: GPL-3.0-or-later
// The page: header, the open section, button hints, toasts and dialogs.
import { tab } from "../state/view";
import { Overlays } from "../components/Overlay";
import { Toasts } from "../components/Toasts";
import { FilesPage } from "../pages/files/FilesPage";
import { QueuePage } from "../pages/queue/QueuePage";
import { DepsPage } from "../pages/deps/DepsPage";
import { SettingsPage } from "../pages/settings/SettingsPage";
import { Header } from "./Header";
import { Hints } from "./Hints";

export function App() {
  const current = tab.value;
  return (
    <>
      <Header />
      <FilesPage hidden={current !== "files"} />
      <QueuePage hidden={current !== "queue"} />
      <DepsPage hidden={current !== "deps"} />
      <SettingsPage hidden={current !== "settings"} />
      <Hints />
      <Toasts />
      <Overlays />
    </>
  );
}
