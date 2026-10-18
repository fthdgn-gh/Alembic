// SPDX-License-Identifier: GPL-3.0-or-later
// Minimal POSIX file I/O, byte sinks and progress reporting. POSIX covers both
// the PC host (macOS/Linux) and the PS5 (FreeBSD-based).
#pragma once

#include "alembic/result.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>

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

    bool is_open() const { return fd_ >= 0; }
    const std::string& path() const { return path_; }

    Result<std::uint64_t> size() const override;
    Status read_at(std::uint64_t offset, MutableBytes out) const override;
    // Writes all of `data` at `offset`.
    Status write_at(std::uint64_t offset, Bytes data);
    Status truncate(std::uint64_t length);
    Status sync();
    Status close();

private:
    int fd_ = -1;
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

bool path_exists(const std::string& path);
bool is_directory(const std::string& path);
bool is_regular_file(const std::string& path); // follows symlinks
// mkdir -p for every folder above `path`.
Status make_parent_directories(const std::string& path);
// Absolute path with symlinks resolved. For a path that does not exist yet,
// resolves the nearest existing parent and appends the rest.
Result<std::string> absolute_path(const std::string& path);
// "/a/b/c" -> "c"; trailing slashes ignored.
std::string base_name(const std::string& path);
std::string os_error_message(int error_number);

} // namespace alembic
