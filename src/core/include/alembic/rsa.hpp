// SPDX-License-Identifier: GPL-3.0-or-later
// The RSA operations package writers need, on big-endian byte strings whose
// length is the modulus length (256 bytes for PS4, 384 for PS5). No key
// material lives here: moduli and exponents come from files the user
// supplies or the engine downloads (pinned by SHA-256).
//
// mt_wrap is the deterministic key wrap both console package formats use:
// EME-PKCS#1 v1.5 whose non-zero padding comes from MT19937 seeded with
// SHA-256(SHA-256(modulus || data)). Ported from pearlxcore/ProsperoPkgTool
// (MIT) Crypto/ProsperoRsaKeys.cs and Crypto/MersenneTwister.cs, the same
// scheme as maxton/LibOrbisPkg (LGPL-3.0) Crypto.RSA2048EncryptKey.
#pragma once

#include "alembic/crypto.hpp"
#include "alembic/io.hpp"

#include <cstdint>
#include <vector>

namespace alembic::rsa {

// value^exponent mod modulus. `value` must be shorter than or as long as the
// modulus and smaller than it; the modulus must be odd and a multiple of 4 bytes.
std::vector<std::uint8_t> power(Bytes modulus, Bytes exponent, Bytes value);
// value^65537 mod modulus.
std::vector<std::uint8_t> public_op(Bytes modulus, Bytes value);
// The MT19937-padded wrap of `data` (at most modulus length - 11 bytes).
std::vector<std::uint8_t> mt_wrap(Bytes modulus, Bytes data);
// An RSASSA-PKCS#1 v1.5 signature of a SHA-256 digest with the private exponent.
std::vector<std::uint8_t> sign_sha256(Bytes modulus, Bytes private_exponent, const crypto::Digest& digest);

} // namespace alembic::rsa
