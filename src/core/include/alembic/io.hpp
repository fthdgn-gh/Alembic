// SPDX-License-Identifier: GPL-3.0-or-later
// File and folder access, byte sinks and progress reporting. Paths are UTF-8
// with '/' between parts on every platform. io.cpp has the POSIX code (macOS,
// Linux, the PS5); io_win32.cpp the Windows code, which also takes '\\' and
// drive letters ("C:/Games/x") and converts paths to UTF-16 for Win32.
#pragma once

#include "alembic/result.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace alembic {

using Bytes = std::span<const std::uint8_t>;
using MutableBytes = std::span<std::uint8_t>;

// Receives a forward-only byte stream.
class ByteSink {
public:
    virtual ~ByteSink() = default;
    virtual Status write(Bytes data) = 0;
};

// Called while long operations run. Return false to cancel.
struct ProgressEvent {
    std::string_view stage;
    std::uint64_t done;
    std::uint64_t total;
};
using ProgressFn = std::function<bool(const ProgressEvent&)>;

// Something that can be read at any offset: a file, part of a file, or a
// payload decoded on demand.
class RandomAccess {
public:
    virtual ~RandomAccess() = default;
    virtual Result<std::uint64_t> size() const = 0;
    // Reads exactly `out.size()` bytes at `offset`, or fails.
    virtual Status read_at(std::uint64_t offset, MutableBytes out) const = 0;
};

// A window [offset, offset + length) of another RandomAccess.
class Slice final : public RandomAccess {
public:
    Slice(std::shared_ptr<const RandomAccess> base, std::uint64_t offset, std::uint64_t length)
        : base_(std::move(base)), offset_(offset), length_(length) {}
    Result<std::uint64_t> size() const override { return length_; }
    Status read_at(std::uint64_t offset, MutableBytes out) const override;

private:
    std::shared_ptr<const RandomAccess> base_;
    std::uint64_t offset_;
    std::uint64_t length_;
};

// An open file somewhere else (a mounted FileSystem, below).
class FileHandle {
public:
    virtual ~FileHandle() = default; // closes it
    virtual Result<std::uint64_t> size() const = 0;
    virtual Status read_at(std::uint64_t offset, MutableBytes out) const = 0;
    virtual Status write_at(std::uint64_t offset, Bytes data) = 0;
    virtual Status truncate(std::uint64_t length) = 0;
    virtual Status sync() = 0;
    virtual Status close() = 0;
};

class File final : public RandomAccess {
public:
    enum class Mode { read, create_new, read_write };

    File() = default;
    ~File();
    File(File&& other) noexcept;
    File& operator=(File&& other) noexcept;
    File(const File&) = delete;
    File& operator=(const File&) = delete;

    static Result<File> open(const std::string& path, Mode mode);

    bool is_open() const { return fd_ >= 0 || handle_ != nullptr; }
    const std::string& path() const { return path_; }

    Result<std::uint64_t> size() const override;
    Status read_at(std::uint64_t offset, MutableBytes out) const override;
    // Writes all of `data` at `offset`.
    Status write_at(std::uint64_t offset, Bytes data);
    Status truncate(std::uint64_t length);
    Status sync();
    Status close();

private:
    std::intptr_t fd_ = -1; // a file descriptor, or a Windows HANDLE
    std::unique_ptr<FileHandle> handle_; // or a file in a mounted FileSystem
    std::string path_;
};

// Appends sequentially to a File starting at a given offset.
class FileSink final : public ByteSink {
public:
    FileSink(File& file, std::uint64_t start) : file_(file), position_(start) {}
    Status write(Bytes data) override;
    std::uint64_t position() const { return position_; }

private:
    File& file_;
    std::uint64_t position_;
};

// Counts bytes and discards them. Useful for dry runs and tests.
class CountingSink final : public ByteSink {
public:
    Status write(Bytes data) override {
        count_ += data.size();
        return {};
    }
    std::uint64_t count() const { return count_; }

private:
    std::uint64_t count_ = 0;
};

// Writes to `<path>.<random>.tmp` and renames over `path` on commit. The
// temporary file is removed if the object is destroyed before commit.
class AtomicOutput {
public:
    ~AtomicOutput();
    // Creates missing parent folders. Fails if `path` exists, unless `overwrite`.
    static Result<AtomicOutput> create(const std::string& path, bool overwrite = false);
    // A resumable output: writes to `partial_path`, which is opened as it is
    // when `reuse` (it must exist) and created or emptied otherwise. It is
    // never deleted here: the caller decides when a partial file is dropped.
    static Result<AtomicOutput> create_partial(const std::string& path, const std::string& partial_path, bool reuse,
                                               bool overwrite = false);
    AtomicOutput(AtomicOutput&& other) noexcept;
    AtomicOutput& operator=(AtomicOutput&&) = delete;

    File& file() { return file_; }
    Status commit();

private:
    AtomicOutput() = default;
    File file_;
    std::string final_path_;
    std::string temp_path_;
    bool committed_ = false;
    bool overwrite_ = false;
    bool keep_ = false; // a partial file: kept when not committed
};

// What is at a path. `follow_links` false reports a symbolic link (on
// Windows, any reparse point) as `link` instead of what it points at.
enum class PathKind { missing, file, directory, link, other };
struct PathStat {
    PathKind kind = PathKind::missing;
    std::uint64_t size = 0; // folders: what the system reports (0 on Windows)
    std::int64_t atime = 0; // Unix seconds
    std::int64_t mtime = 0;
    bool is_file() const { return kind == PathKind::file; }
    bool is_directory() const { return kind == PathKind::directory; }
};
// `kind` is `missing` when the path cannot be read (`error` then says why).
PathStat stat_path(const std::string& path, bool follow_links = true, std::string* error = nullptr);
// The names in a folder, without "." and "..", in the order the system gives.
Result<std::vector<std::string>> list_directory(const std::string& path);
// Creates one folder; fails when something is already there.
Status make_directory(const std::string& path);
Status remove_file(const std::string& path);
Status remove_directory(const std::string& path); // must be empty
// Renames a file or folder, replacing a file at `to`. Fails with
// Errc::unsupported when `to` is on another drive (copy instead).
Status rename_path(const std::string& from, const std::string& to);
// Sets the access and modification times (Unix seconds).
Status set_file_times(const std::string& path, std::int64_t atime, std::int64_t mtime);
// Sets both to now.
Status touch_file(const std::string& path);
// Pushes written data to the drives (sync(2)). Windows has no such call for
// files already closed, so there it does nothing.
void flush_file_system();
struct DiskSpace {
    std::uint64_t total = 0;
    std::uint64_t free = 0; // available to this process
};
Result<DiskSpace> disk_space(const std::string& path);
int process_id();
// "/x" on POSIX; "C:/x", "C:\\x" or "//server/share" on Windows.
bool is_absolute_path(const std::string& path);
// '\\' to '/' on Windows; unchanged elsewhere.
std::string portable_path(std::string path);
// Drops trailing '/', but keeps "/" and a drive root ("C:/").
std::string without_trailing_slashes(std::string path);
// dir + "/" + name, without doubling the '/' of "/" or "C:/".
std::string join_path(const std::string& dir, const std::string& name);
// An environment variable as UTF-8 ("" when unset).
std::string environment(const char* name);
// The program's arguments as UTF-8 (Windows passes main() the ANSI code page).
std::vector<std::string> program_arguments(int argc, char** argv);
// Makes stdout and stderr take UTF-8 (the Windows console); else nothing.
void use_utf8_console();
bool stderr_is_terminal();
// fopen() for a UTF-8 path (fopen itself takes the ANSI code page on Windows).
std::FILE* open_stdio(const std::string& path, const char* mode);

// A folder's entries with what stat_path would say about each (one request
// where the files are elsewhere).
struct DirEntry {
    std::string name;
    PathStat stat;
};
Result<std::vector<DirEntry>> list_directory_entries(const std::string& path, bool follow_links = true);

// Files elsewhere, under a path prefix: another engine's files are
// "/peer/<id>/<its path>". Every call above (File::open, stat_path,
// list_directory, rename_path, ...) on a path that starts with a mounted
// prefix goes to its FileSystem, with the full path.
class FileSystem {
public:
    virtual ~FileSystem() = default;
    virtual Result<std::unique_ptr<FileHandle>> open(const std::string& path, File::Mode mode) = 0;
    virtual PathStat stat(const std::string& path, bool follow_links, std::string* error) = 0;
    virtual Result<std::vector<DirEntry>> list(const std::string& path, bool follow_links) = 0;
    virtual Status make_directory(const std::string& path) = 0;
    virtual Status remove_file(const std::string& path) = 0;
    virtual Status remove_directory(const std::string& path) = 0;
    virtual Status rename(const std::string& from, const std::string& to) = 0; // both under this prefix
    virtual Status set_times(const std::string& path, std::int64_t atime, std::int64_t mtime) = 0;
    virtual Result<DiskSpace> disk_space(const std::string& path) = 0;
};
// `prefix` ends in '/' ("/peer/"); a later mount of the same prefix replaces it.
void mount(const std::string& prefix, std::shared_ptr<FileSystem> fs);
void unmount(const std::string& prefix);
bool is_mounted(const std::string& path);

bool path_exists(const std::string& path);
bool is_directory(const std::string& path);
bool is_regular_file(const std::string& path); // follows symlinks
// mkdir -p for every folder above `path`.
Status make_parent_directories(const std::string& path);
// Absolute path with symlinks resolved. For a path that does not exist yet,
// resolves the nearest existing parent and appends the rest.
Result<std::string> absolute_path(const std::string& path);
// Whether `name` (from an image, or typed in the page) is one plain name that
// stays inside its folder here: no '/', "." or "..", and on Windows none of
// \\ : * ? " < > | or control characters either (':' would write to an
// alternate data stream).
bool safe_file_name(std::string_view name);
// "/a/b/c" -> "c"; trailing slashes ignored.
std::string base_name(const std::string& path);
std::string os_error_message(int error_number);
// The message for the calling thread's last failed system call (errno, or
// GetLastError on Windows).
std::string last_os_error();

} // namespace alembic
