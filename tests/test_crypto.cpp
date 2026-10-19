// SPDX-License-Identifier: GPL-3.0-or-later
// Official test vectors: FIPS-197, IEEE 1619, NIST SP 800-38A, FIPS-202, RFC 4231.
#include "alembic/crypto.hpp"
#include "test.hpp"

#include <algorithm>
#include <string>
#include <vector>

using namespace alembic;

namespace {

std::vector<std::uint8_t> hex(const std::string& text) {
    std::vector<std::uint8_t> out;
    for (std::size_t i = 0; i + 1 < text.size(); i += 2) out.push_back(static_cast<std::uint8_t>(std::stoi(text.substr(i, 2), nullptr, 16)));
    return out;
}

std::string to_hex(const std::uint8_t* p, std::size_t n) {
    static const char d[] = "0123456789abcdef";
    std::string s;
    for (std::size_t i = 0; i < n; ++i) {
        s += d[p[i] >> 4];
        s += d[p[i] & 15];
    }
    return s;
}

std::string str_hex(const std::string& s) {
    return to_hex(crypto::sha3_256(Bytes(reinterpret_cast<const std::uint8_t*>(s.data()), s.size())).data(), 32);
}

} // namespace

TEST_CASE(aes128_fips197) {
    auto key = hex("000102030405060708090a0b0c0d0e0f");
    auto pt = hex("00112233445566778899aabbccddeeff");
    crypto::Aes128 aes(key.data());
    std::uint8_t out[16], back[16];
    aes.encrypt_block(pt.data(), out);
    CHECK(to_hex(out, 16) == "69c4e0d86a7b0430d8cdb78070b4c55a");
    aes.decrypt_block(out, back);
    CHECK(to_hex(back, 16) == "00112233445566778899aabbccddeeff");
}

TEST_CASE(aes128_cbc_sp800_38a) {
    auto key = hex("2b7e151628aed2a6abf7158809cf4f3c");
    auto iv = hex("000102030405060708090a0b0c0d0e0f");
    auto data = hex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51");
    crypto::Aes128 aes(key.data());
    crypto::cbc_encrypt(aes, iv.data(), data);
    CHECK(to_hex(data.data(), 32) == "7649abac8119b246cee98e9b12e9197d5086cb9b507219ee95db113a917678b2");
    crypto::cbc_decrypt(aes, iv.data(), data);
    CHECK(to_hex(data.data(), 32) == "6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51");
}

TEST_CASE(aes128_xts_ieee1619) {
    // Vector 1: zero keys, sector 0, 32 zero bytes.
    std::uint8_t zero[16] = {};
    crypto::Aes128 k1(zero), k2(zero);
    std::vector<std::uint8_t> data(32, 0);
    crypto::xts_encrypt(k1, k2, 0, data);
    CHECK(to_hex(data.data(), 32) == "917cf69ebd68b2ec9b9fe9a3eadda692cd43d2f59598ed858c02c2652fbf922e");
    crypto::xts_decrypt(k1, k2, 0, data);
    CHECK(data == std::vector<std::uint8_t>(32, 0));
    // Vector 2: key1 = 11.., key2 = 22.., sector 0x3333333333, plaintext 44..
    std::vector<std::uint8_t> a(16, 0x11), b(16, 0x22);
    crypto::Aes128 data_key(a.data()), tweak_key(b.data());
    std::vector<std::uint8_t> v2(32, 0x44);
    crypto::xts_encrypt(data_key, tweak_key, 0x3333333333ull, v2);
    CHECK(to_hex(v2.data(), 32) == "c454185e6a16936e39334038acef838bfb186fff7480adc4289382ecd6d394f0");
}

TEST_CASE(sha3_256_fips202) {
    CHECK(str_hex("") == "a7ffc6f8bf1ed76651c14756a061d662f580ff4de43b49fa82d80a4b80f8434a");
    CHECK(str_hex("abc") == "3a985da74fe225b2045c172d6bd390bd855f086e3e9d525b46bfe24511431532");
    // 136 bytes: exactly one rate block, so padding goes in a block of its own.
    CHECK(str_hex(std::string(136, 'a')) == "3fc5559f14db8e453a0a3091edbd2bc25e11528d81c66fa570a4efdcc2695ee1"); // from Python hashlib
    crypto::Sha3_256 split;
    std::string text(500, 'q');
    split.update(Bytes(reinterpret_cast<const std::uint8_t*>(text.data()), 137));
    split.update(Bytes(reinterpret_cast<const std::uint8_t*>(text.data()) + 137, 363));
    auto whole = crypto::sha3_256(Bytes(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
    CHECK(split.finish() == whole);
    // "abcdbcdecdef..." (448 bits) from the FIPS-202 examples.
    CHECK(str_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "41c0dba2a9d6240849100376a8235e2c82e1b9998a999e21db32dd97496d3376");
}

TEST_CASE(hmac_sha256_rfc4231) {
    std::vector<std::uint8_t> key(20, 0x0b);
    std::string data = "Hi There";
    auto mac = crypto::hmac_sha256(key, Bytes(reinterpret_cast<const std::uint8_t*>(data.data()), data.size()));
    CHECK(to_hex(mac.data(), 32) == "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
    // Test case 6: a 131-byte key is hashed first.
    std::vector<std::uint8_t> long_key(131, 0xaa);
    std::string msg = "Test Using Larger Than Block-Size Key - Hash Key First";
    auto mac6 = crypto::hmac_sha256(long_key, Bytes(reinterpret_cast<const std::uint8_t*>(msg.data()), msg.size()));
    CHECK(to_hex(mac6.data(), 32) == "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
}

// The AES instructions (several blocks at a time) against the table code,
// and XTS / CBC in pieces against a block-by-block reference.
TEST_CASE(aes128_fast_paths_match_reference) {
#if (defined(__ARM_FEATURE_AES) && defined(__aarch64__)) || (defined(__AES__) && defined(__x86_64__))
    CHECK(crypto::Aes128::hardware()); // the self-test passed, so the comparison below covers the instructions
#endif
    std::vector<std::uint8_t> data(16 * 300);
    std::uint32_t x = 12345;
    for (auto& b : data) b = static_cast<std::uint8_t>((x = x * 1103515245u + 12345u) >> 16);
    std::uint8_t key[16], key2[16];
    for (int i = 0; i < 16; ++i) key[i] = static_cast<std::uint8_t>(i * 17 + 3), key2[i] = static_cast<std::uint8_t>(250 - i * 7);
    crypto::Aes128 aes(key), tweak(key2);
    for (std::size_t count : {1u, 7u, 8u, 9u, 16u, 31u, 32u, 33u, 300u}) {
        std::vector<std::uint8_t> fast(data.begin(), data.begin() + 16 * count), slow = fast;
        aes.encrypt_blocks(fast.data(), fast.data(), count);
        for (std::size_t j = 0; j < count; ++j) aes.portable_encrypt(slow.data() + 16 * j, slow.data() + 16 * j);
        CHECK(fast == slow);
        aes.decrypt_blocks(fast.data(), fast.data(), count);
        for (std::size_t j = 0; j < count; ++j) aes.portable_decrypt(slow.data() + 16 * j, slow.data() + 16 * j);
        CHECK(fast == slow);
        CHECK(std::equal(fast.begin(), fast.end(), data.begin()));
    }
    // XTS reference: one block at a time, tweak doubled byte by byte.
    auto reference_xts = [&](std::vector<std::uint8_t> v, std::uint64_t sector) {
        std::uint8_t t[16] = {};
        for (int i = 0; i < 8; ++i) t[i] = static_cast<std::uint8_t>(sector >> (8 * i));
        tweak.portable_encrypt(t, t);
        for (std::size_t off = 0; off < v.size(); off += 16) {
            std::uint8_t b[16];
            for (int i = 0; i < 16; ++i) b[i] = v[off + i] ^ t[i];
            aes.portable_encrypt(b, b);
            for (int i = 0; i < 16; ++i) v[off + i] = b[i] ^ t[i];
            std::uint8_t carry = 0;
            for (int i = 0; i < 16; ++i) {
                std::uint8_t next = t[i] >> 7;
                t[i] = static_cast<std::uint8_t>((t[i] << 1) | carry);
                carry = next;
            }
            if (carry) t[0] ^= 0x87;
        }
        return v;
    };
    std::vector<std::uint8_t> xts = data;
    crypto::xts_encrypt(aes, tweak, 0x123456789ull, xts);
    CHECK(xts == reference_xts(data, 0x123456789ull));
    crypto::xts_decrypt(aes, tweak, 0x123456789ull, xts);
    CHECK(xts == data);
    // CBC: encrypt (sequential) then the piecewise decrypt gives the data back.
    std::uint8_t iv[16] = {9, 8, 7, 6, 5, 4, 3, 2, 1, 0, 1, 2, 3, 4, 5, 6};
    std::vector<std::uint8_t> cbc = data;
    crypto::cbc_encrypt(aes, iv, cbc);
    std::vector<std::uint8_t> manual = data;
    const std::uint8_t* prev = iv;
    for (std::size_t off = 0; off < manual.size(); off += 16) {
        for (int i = 0; i < 16; ++i) manual[off + i] ^= prev[i];
        aes.portable_encrypt(manual.data() + off, manual.data() + off);
        prev = manual.data() + off;
    }
    CHECK(cbc == manual);
    crypto::cbc_decrypt(aes, iv, cbc);
    CHECK(cbc == data);
}

TEST_CASE(crc32c_check_value) {
    const char* text = "123456789";
    CHECK_EQ(crypto::crc32c(Bytes(reinterpret_cast<const std::uint8_t*>(text), 9)), 0xE3069283u);
    // In two parts, and across the 8-byte steps.
    std::uint32_t part = crypto::crc32c(Bytes(reinterpret_cast<const std::uint8_t*>(text), 4));
    CHECK_EQ(crypto::crc32c(Bytes(reinterpret_cast<const std::uint8_t*>(text) + 4, 5), part), 0xE3069283u);
    std::vector<std::uint8_t> zeros(32, 0);
    CHECK_EQ(crypto::crc32c(zeros), 0x8A9136AAu); // RFC 3720 B.4
}

TEST_CASE(sha3_256_state_round_trips) {
    std::string text(1000, 'z');
    const auto* p = reinterpret_cast<const std::uint8_t*>(text.data());
    crypto::Sha3_256 a;
    a.update(Bytes(p, 300)); // two blocks and part of a third
    auto saved = a.save();
    crypto::Sha3_256 b = crypto::Sha3_256::restore(saved.data());
    a.update(Bytes(p + 300, 700));
    b.update(Bytes(p + 300, 700));
    CHECK(a.finish() == b.finish());
}
