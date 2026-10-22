// SPDX-License-Identifier: GPL-3.0-or-later
// SHA-256 (FIPS 180-4), incremental.
#pragma once

#include "alembic/io.hpp"

#include <array>
#include <cstdint>
#include <string>

namespace alembic {

class Sha256 final : public ByteSink {
public:
    using Digest = std::array<std::uint8_t, 32>;

    Sha256();
    void update(Bytes data);
    Status write(Bytes data) override {
        update(data);
        return {};
    }
    Digest finish(); // the object must not be reused afterwards

    static std::string hex(const Digest& digest);
    // SHA-NI (x86-64) or the ARMv8 SHA-256 instructions are used: the CPU has
    // them and they passed a self-test.
    static bool hardware();
    // The portable code only (the reference in tests).
    static Digest portable(Bytes data);

private:
    void blocks(const std::uint8_t* p, std::size_t count);
    void block(const std::uint8_t* p);
    bool portable_only_ = false;
    std::uint32_t h_[8];
    std::uint8_t buffer_[64];
    std::size_t fill_ = 0;
    std::uint64_t length_ = 0;
};

} // namespace alembic
