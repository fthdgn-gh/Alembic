// SPDX-License-Identifier: GPL-3.0-or-later
#include "alembic/resume.hpp"

#include <unistd.h>

#include <algorithm>

namespace alembic::resume {

Status CheckpointSink::save() {
    ALEMBIC_TRY(file_.sync());
    last_ = position_;
    State s = extra_;
    s["format"] = format_;
    s["length"] = length_;
    s["written"] = position_;
    return session_->save(s);
}

Status CheckpointSink::write(Bytes data) {
    ALEMBIC_TRY(file_.write_at(position_, data));
    position_ += data.size();
    if (session_ && session_->save && position_ - last_ >= session_->interval) ALEMBIC_TRY(save());
    return {};
}

Status CheckpointSink::save_now() {
    if (!session_ || !session_->save || position_ == last_) return {};
    return save();
}

Result<std::uint64_t> sequential_start(const Session* session, std::uint64_t length, std::uint64_t format) {
    if (!session || !session->from) return std::uint64_t{0};
    const State& s = *session->from;
    if (get(s, "format") != format || get(s, "length") != length || get(s, "written") > length)
        return make_error(Errc::invalid_data, "the checkpoint does not match this conversion");
    return get(s, "written");
}

Status SkipSink::write(Bytes data) {
    if (std::uint64_t drop = std::min<std::uint64_t>(skippable(), data.size())) {
        position_ += drop;
        data = data.subspan(static_cast<std::size_t>(drop));
    }
    if (data.empty()) return {};
    position_ += data.size();
    return out_.write(data);
}

void remove_partial(const std::string& partial_path) {
    if (partial_path.empty()) return;
    ::unlink(partial_path.c_str());
    for (const char* suffix : kSidecarSuffixes) ::unlink((partial_path + suffix).c_str());
}

} // namespace alembic::resume
