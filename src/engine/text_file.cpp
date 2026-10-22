// SPDX-License-Identifier: GPL-3.0-or-later
#include "text_file.hpp"

#include "alembic/io.hpp"

namespace alembic::engine {

Result<std::string> read_text(const std::string& path, std::size_t max_bytes) {
    ALEMBIC_ASSIGN(File file, File::open(path, File::Mode::read));
    ALEMBIC_ASSIGN(std::uint64_t size, file.size());
    if (size > max_bytes) return make_error(Errc::too_large, path + " is too large");
    std::string text(static_cast<std::size_t>(size), '\0');
    ALEMBIC_TRY(file.read_at(0, MutableBytes(reinterpret_cast<std::uint8_t*>(text.data()), text.size())));
    return text;
}

Status write_text(const std::string& path, const std::string& text) {
    ALEMBIC_ASSIGN(AtomicOutput out, AtomicOutput::create(path, true));
    ALEMBIC_TRY(out.file().write_at(0, Bytes(reinterpret_cast<const std::uint8_t*>(text.data()), text.size())));
    return out.commit();
}

} // namespace alembic::engine
