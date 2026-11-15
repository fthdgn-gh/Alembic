// SPDX-License-Identifier: GPL-3.0-or-later
// Games installed on the console and the one running now, for dumping a
// running game (convert's decrypt_self on its mounted tree).
//
// While an installed or disc game runs, the system mounts its tree decrypted
// at /mnt/sandbox/pfsmnt/<TITLE_ID>-app0 (with an update also
// <TITLE_ID>-app0-patch0-union, the update over the base). A game started
// from an image through ShadowMount+ never appears there (console-checked),
// so that folder is the list of running games that can be dumped. Titles,
// versions and icons come from /user/appmeta/<TITLE_ID>/, which the mounted
// tree lacks.
#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace alembic::engine::games {

struct Paths {
    std::string pfsmnt = "/mnt/sandbox/pfsmnt";
    std::string appmeta = "/user/appmeta";
    std::string user_patch = "/user/patch";
};

// Four capital letters and five digits (PPSA01234): the only form a title ID
// takes in paths built from it.
bool valid_title_id(std::string_view id);

// The title IDs with a mounted app0 now, sorted.
std::vector<std::string> mounted(const Paths& paths);

// The tree a dump reads: the base with its update laid over it (the union)
// when the game has an update installed, else the base. `with_update` says
// which. Not checked against what is mounted.
std::string source_tree(const Paths& paths, const std::string& id, bool* with_update = nullptr);

struct Info {
    std::string title_id;
    std::string title; // "" when appmeta has no param.json
    std::string version;
    std::string content_id;
    std::string icon; // appmeta's icon0.png, "" when missing
};
// From appmeta (title_id is always `id`).
Info info(const Paths& paths, const std::string& id);

// Which games are mounted and since when. A dump waits until a game has been
// mounted for `settle_seconds`: decrypting swaps the kernel's pager for every
// file mapping in the system, so not while the game is still loading its own
// executables.
class Watcher {
public:
    explicit Watcher(Paths paths = {}, std::int64_t settle_seconds = 30);

    // Looks at the mounts once (`now` in seconds). True when the running or
    // settled games changed.
    bool poll(std::int64_t now);

    bool running(const std::string& id) const;
    bool settled(const std::string& id) const;
    std::vector<std::string> running() const; // sorted
    // Takes the game as not running until the next poll finds it again (then
    // it settles anew): after a dump saw it close, before the watcher did.
    void forget(const std::string& id);
    const Paths& paths() const { return paths_; }

private:
    Paths paths_;
    std::int64_t settle_;
    mutable std::mutex mutex_;
    std::map<std::string, std::int64_t> since_; // id -> first seen
    std::map<std::string, bool> settled_;
};

// When a dump's progress is worth a toast: every `min_gap` seconds (15 s,
// owner: 5% steps, about 30 s apart on a 27 GB game, felt sparse) with the
// percentage reached, when it moved since the last one; the first one
// `min_gap` into a stage too (owner: not right after the "dumping" toast),
// its end at once. Not for stages without a total.
class ProgressSteps {
public:
    explicit ProgressSteps(std::int64_t min_gap = 15) : min_gap_(min_gap) {}
    // The percentage to show now, if any.
    std::optional<int> update(std::uint64_t job, const std::string& stage, std::uint64_t done, std::uint64_t total,
                              std::int64_t now);
    void forget(std::uint64_t job);

private:
    struct State {
        std::string stage;
        int shown = 0;           // last step shown (percent)
        std::int64_t at = 0;     // when
    };
    std::int64_t min_gap_;
    std::map<std::uint64_t, State> jobs_;
};

} // namespace alembic::engine::games
