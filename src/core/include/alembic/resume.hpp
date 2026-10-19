// SPDX-License-Identifier: GPL-3.0-or-later
// Resumable writes. A writer that supports them writes into a fixed partial
// file and, every `interval` bytes of input, makes what it wrote durable
// (fsync) and hands a small State to `save`. A later run given that State
// continues from it: the writer drops whatever the partial file holds past
// the checkpoint and produces the rest, so the result is byte-identical to
// an uninterrupted run. The caller makes sure the input and options are the
// same as when the State was saved.
#pragma once

#include "alembic/io.hpp"
#include "alembic/result.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>

namespace alembic::resume {

// A writer's checkpoint: a few named numbers ("format", "blocks", ...).
using State = std::map<std::string, std::uint64_t>;

struct Session {
    std::string partial_path;                 // kept between runs; renamed to the output at the end
    std::optional<State> from;                // continue from this checkpoint; empty: start over
    std::function<Status(const State&)> save; // called once the data up to the State is on disk
    std::uint64_t interval = 1ull << 30;      // input bytes between checkpoints
};

// Bump when a resumable writer's output bytes change, so a checkpoint from
// an older build is not continued by a newer one.
inline constexpr std::uint64_t kLayoutVersion = 1;

// Writer ids, stored as State["format"]: a State is only resumed by the writer that saved it.
inline constexpr std::uint64_t kFfpfscFormat = 1;
inline constexpr std::uint64_t kSequentialFormat = 2; // exFAT images and image copies
inline constexpr std::uint64_t kUfs2Format = 3;       // + timestamp, pid
inline constexpr std::uint64_t kPfsFormat = 4;        // + timestamp
inline constexpr std::uint64_t kFolderFormat = 5;     // done (files already written are kept)
inline constexpr std::uint64_t kFpkgFormat = 6;       // + side files (sidecar_path)
inline constexpr std::uint64_t kLz4Format = 7;        // LZ4 packing (see lz4pack.hpp)

// Side files a writer keeps next to the partial output when a State is too
// small for its checkpoint; they go with it.
inline constexpr const char* kSidecarSuffixes[] = {".state", ".digests", ".lz4"};
// The partial output's name: "<output>" + this (a folder output has none,
// but its side files use the same name).
inline constexpr const char* kPartialSuffix = ".alembic-partial";
inline std::string sidecar_path(const Session& session, const char* suffix) { return session.partial_path + suffix; }
// Deletes a partial output and its side files.
void remove_partial(const std::string& partial_path);

// A value of a State, or ~0 when it is missing.
inline std::uint64_t get(const State& s, const char* key) {
    auto it = s.find(key);
    return it == s.end() ? ~std::uint64_t{0} : it->second;
}

// Writes sequentially into `file` from `start`; with a session, syncs and
// saves {format, length, written} plus `extra` every interval bytes.
class CheckpointSink final : public ByteSink {
public:
    CheckpointSink(File& file, std::uint64_t start, std::uint64_t length, Session* session,
                   std::uint64_t format = kSequentialFormat, State extra = {})
        : file_(file), position_(start), last_(start), length_(length), session_(session), format_(format),
          extra_(std::move(extra)) {}
    Status write(Bytes data) override;
    std::uint64_t position() const { return position_; }
    // Syncs and saves a checkpoint at the current position (after a pause or an error).
    Status save_now();

private:
    Status save();
    File& file_;
    std::uint64_t position_, last_, length_;
    Session* session_;
    std::uint64_t format_;
    State extra_;
};

// For sequential writers: where to start (0 without a checkpoint), after
// checking that `from` was saved by the `format` writer for a `length`-byte output.
Result<std::uint64_t> sequential_start(const Session* session, std::uint64_t length,
                                       std::uint64_t format = kSequentialFormat);

// Drops the first `start` bytes written to it and passes the rest on, for
// writers that continue an image part way through.
class SkipSink final : public ByteSink {
public:
    SkipSink(ByteSink& out, std::uint64_t start) : out_(out), start_(start) {}
    Status write(Bytes data) override;
    std::uint64_t position() const { return position_; }  // bytes received, dropped ones included
    std::uint64_t skippable() const { return start_ > position_ ? start_ - position_ : 0; }
    void skip(std::uint64_t n) { position_ += n < skippable() ? n : skippable(); } // as if written

private:
    ByteSink& out_;
    std::uint64_t start_, position_ = 0;
};

} // namespace alembic::resume
