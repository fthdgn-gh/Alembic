// SPDX-License-Identifier: GPL-3.0-or-later
// Names Rename suggests for a game, from formats the person can change in
// Settings (kept in <state>/name_formats.json).
//
// A format is a pattern plus what to do with spaces and letters:
//   {title} [{titleid}] [v{version:short}]
//   - {field} or [field] is replaced by a value: title, titleid (id),
//     contentid, version (ver), format (kind: PKG, EXFAT, FFPFSC, FFPKG,
//     FFPFS; none for a folder), platform (PS5, PS4) or name (the current
//     name without its extension). Case, '_' and '-' in field names do not
//     matter. [word] stays as it is when word is no field, so [USA] is text;
//     {word} must be a field. {{ and }} are a brace.
//   - :modifiers after a field, one or more: upper, lower, nospace, under
//     (spaces to _), dash (to -), dot (to .), short (01.004.000 -> 1.4), and
//     a number (at most that many characters): {title:upper:20}.
//   - <...> is left out when a field in it has no value: <[v{version}]>.
//     A field outside <...> with no value means the format does not apply.
// Values lose what file systems refuse (/ \ * ? " < > | and control
// characters; ':' becomes " - "). Then the whole name gets the format's
// letters (keep, upper, lower; ASCII letters) and spaces (keep, none, "_",
// ".", "-").
#pragma once

#include "alembic/result.hpp"

#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace alembic::engine {

struct NameFormat {
    std::string pattern;
    std::string spaces = "keep";  // keep | none | _ | . | -
    std::string letters = "keep"; // keep | upper | lower
    bool operator==(const NameFormat&) const = default;
};

struct NameValues {
    std::string title, title_id, content_id, version;
    std::string format;   // PKG, EXFAT, FFPFSC, FFPKG, FFPFS; "" for a folder
    std::string platform; // PS5, PS4 or ""
    std::string name;     // the current name, without "." in front or extension
};

// The values of a game; `entry_name` is its current file or folder name and
// `kind` what the listing calls it (fpkg, exfat, ffpfsc, ffpkg, ffpfs, folder).
NameValues name_values(const std::string& title, const std::string& title_id, const std::string& content_id,
                       const std::string& version, const std::string& platform, const std::string& entry_name,
                       const std::string& kind);

// What is wrong with a format, as an error (the pattern's problem first).
Status check_name_format(const NameFormat& format);

// The name without extension, or nothing when a needed value is missing, the
// result is empty or the format has a problem.
std::optional<std::string> render_name(const NameFormat& format, const NameValues& values);

// `entry_name` renamed with a format: a hidden name (".name") stays hidden
// and a file keeps its extension.
std::optional<std::string> suggested_name(const NameFormat& format, const NameValues& values,
                                          const std::string& entry_name, bool folder);

// A title made safe for every file system the engine writes to.
std::string safe_name(const std::string& text);

std::vector<NameFormat> default_name_formats();

class NameFormats {
public:
    explicit NameFormats(std::string state_dir);
    std::vector<NameFormat> list() const;
    // Checks every format first; nothing changes on a problem.
    Status set(std::vector<NameFormat> formats);
    Status reset();

    static constexpr std::size_t kMax = 32;
    static constexpr std::size_t kMaxPattern = 200;

private:
    Status save_locked() const;
    std::string file_;
    mutable std::mutex mutex_;
    std::vector<NameFormat> formats_;
};

} // namespace alembic::engine
