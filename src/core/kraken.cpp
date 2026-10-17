// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from pearlxcore/ProsperoPkgTool (MIT) Compression/KrakenDecoder.cs.
//
// Notes on the port:
//  * C# masks shift counts to 5 bits; C++ does not (shifting by 32 is
//    undefined). The decoder relies on `x >> 32 == x` in places, so every
//    variable shift goes through shr/shl below, which mask the same way.
//  * Out-of-range indexing throws in C#, which the caller treats as a failed
//    decode. Here every data-dependent index is checked explicitly instead.
#include "alembic/kraken.hpp"

#include <cstdint>
#include <cstring>
#include <vector>

namespace alembic::kraken {

namespace {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using Buffer = std::vector<u8>;

constexpr int kSeedSize = 8;
constexpr int kMalformed = -1;
constexpr int kUnsupported = -2;

inline u32 shr(u32 x, int n) { return x >> (n & 31); }
inline u32 shl(u32 x, int n) { return x << (n & 31); }
inline int ishl(int x, int n) { return static_cast<int>(static_cast<u32>(x) << (n & 31)); }
inline u32 rotl(u32 x, int n) {
    n &= 31;
    return n == 0 ? x : (x << n) | (x >> (32 - n));
}

int msb(u32 x) {
    if (x == 0) return 0;
    int r = 31;
    while (!(x & 0x80000000u)) {
        x <<= 1;
        --r;
    }
    return r;
}
int lsb(u32 x) {
    if (x == 0) return 0;
    int r = 0;
    while (!(x & 1)) {
        x >>= 1;
        ++r;
    }
    return r;
}
int clz(u32 x) { return x == 0 ? 31 : 31 - msb(x); } // .NET LeadingZeroCount, but 31 for zero

// Reads that fall outside the array yield zero, as the C# helpers do.
inline u8 at(const Buffer& b, long i) { return i >= 0 && static_cast<std::size_t>(i) < b.size() ? b[i] : 0; }
u32 read_le32(const Buffer& b, long off) {
    u32 r = 0;
    for (int i = 0; i < 4; ++i) r |= u32(at(b, off + i)) << (8 * i);
    return r;
}
u64 read_le64(const Buffer& b, long off) {
    u64 r = 0;
    for (int i = 0; i < 8; ++i) r |= u64(at(b, off + i)) << (8 * i);
    return r;
}
void write_le32(Buffer& b, long off, u32 v) {
    for (int i = 0; i < 4; ++i)
        if (off + i >= 0 && static_cast<std::size_t>(off + i) < b.size()) b[off + i] = static_cast<u8>(v >> (8 * i));
}
void write_le64(Buffer& b, long off, u64 v) {
    for (int i = 0; i < 8; ++i)
        if (off + i >= 0 && static_cast<std::size_t>(off + i) < b.size()) b[off + i] = static_cast<u8>(v >> (8 * i));
}
u32 swap32(u32 v) { return (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24); }
u64 swap64(u64 v) { return (u64(swap32(static_cast<u32>(v))) << 32) | swap32(static_cast<u32>(v >> 32)); }

// ---------------------------------------------------------------- bit reader

// MSB-first reader with a 24-bit refill window, forward or backward.
struct BitReader {
    const Buffer* data = nullptr;
    long p = 0;
    long bound = 0; // forward: exclusive high bound; backward: inclusive low bound
    u32 bits = 0;
    int bitpos = 24;
    bool backward = false;

    static BitReader forward(const Buffer& d, long start, long end) {
        BitReader r;
        r.data = &d;
        r.p = start;
        r.bound = end;
        r.refill_forward();
        return r;
    }
    static BitReader reverse(const Buffer& d, long low, long high) {
        BitReader r;
        r.data = &d;
        r.p = high;
        r.bound = low;
        r.backward = true;
        r.refill_backward();
        return r;
    }

    void refill() { backward ? refill_backward() : refill_forward(); }
    void refill_forward() {
        while (bitpos > 0) {
            bits |= shl(p < bound ? at(*data, p) : 0, bitpos);
            bitpos -= 8;
            ++p;
        }
    }
    void refill_backward() {
        while (bitpos > 0) {
            --p;
            bits |= shl(p >= bound ? at(*data, p) : 0, bitpos);
            bitpos -= 8;
        }
    }
    int read_bit() {
        refill();
        int r = static_cast<int>(bits >> 31);
        bits <<= 1;
        bitpos += 1;
        return r;
    }
    int read_bit_no_refill() {
        int r = static_cast<int>(bits >> 31);
        bits <<= 1;
        bitpos += 1;
        return r;
    }
    u32 read_bits_no_refill(int n) {
        u32 r = shr(bits, 32 - n);
        bits = shl(bits, n);
        bitpos += n;
        return r;
    }
    u32 read_bits_no_refill_zero(int n) {
        u32 r = shr(bits >> 1, 31 - n);
        bits = shl(bits, n);
        bitpos += n;
        return r;
    }
    int read_fluff(int num_symbols) {
        if (num_symbols == 256) return 0;
        int x = 257 - num_symbols;
        if (x > num_symbols) x = num_symbols;
        x *= 2;
        int y = msb(static_cast<u32>(x - 1)) + 1;
        u32 v = shr(bits, 32 - y);
        u32 z = static_cast<u32>(ishl(1, y) - x);
        if ((v >> 1) >= z) {
            bits = shl(bits, y);
            bitpos += y;
            return static_cast<int>(v - z);
        }
        bits = shl(bits, y - 1);
        bitpos += y - 1;
        return static_cast<int>(v >> 1);
    }
    u32 read_extra(int n) {
        u32 rv;
        if (n <= 24) {
            rv = read_bits_no_refill_zero(n);
        } else {
            rv = shl(read_bits_no_refill(24), n - 24);
            refill();
            rv += read_bits_no_refill(n - 24);
        }
        refill();
        return rv;
    }
    u32 read_distance(u32 v) {
        u32 w, m, rv;
        int n;
        if (v < 0xF0) {
            n = static_cast<int>(v >> 4) + 4;
            w = rotl(bits | 1, n);
            bitpos += n;
            m = shl(2u, n) - 1;
            bits = w & ~m;
            rv = ((w & m) << 4) + (v & 0xF) - 248;
        } else {
            n = static_cast<int>(v - 0xF0) + 4;
            w = rotl(bits | 1, n);
            bitpos += n;
            m = shl(2u, n) - 1;
            bits = w & ~m;
            rv = 8322816 + ((w & m) << 12);
            refill();
            rv += bits >> 20;
            bitpos += 12;
            bits <<= 12;
        }
        refill();
        return rv;
    }
    bool read_length(u32& v) {
        int n = 31 - msb(bits);
        if (n > 12) return false;
        bitpos += n;
        bits = shl(bits, n);
        refill();
        n += 7;
        bitpos += n;
        v = shr(bits, 32 - n) - 64;
        bits = shl(bits, n);
        refill();
        return true;
    }
};

// ---------------------------------------------------------------- Huffman

constexpr u32 kCodePrefixBase[12] = {0x0, 0x0, 0x2, 0x6, 0xE, 0x1E, 0x3E, 0x7E, 0xFE, 0x1FE, 0x2FE, 0x3FE};
constexpr std::size_t kSymsSize = 1280;

struct HuffLookup {
    u8 bits_to_len[2048 + 16] = {};
    u8 bits_to_sym[2048 + 16] = {};
};

struct HuffRange {
    int symbol;
    int num;
};

struct GolombReader {
    const Buffer* data;
    long p;
    long end;
    int bitpos;
};

constexpr u32 kRiceValue[256] = {
    0x80000000, 0x00000007, 0x10000006, 0x00000006, 0x20000005, 0x00000105, 0x10000005, 0x00000005,
    0x30000004, 0x00000204, 0x10000104, 0x00000104, 0x20000004, 0x00010004, 0x10000004, 0x00000004,
    0x40000003, 0x00000303, 0x10000203, 0x00000203, 0x20000103, 0x00010103, 0x10000103, 0x00000103,
    0x30000003, 0x00020003, 0x10010003, 0x00010003, 0x20000003, 0x01000003, 0x10000003, 0x00000003,
    0x50000002, 0x00000402, 0x10000302, 0x00000302, 0x20000202, 0x00010202, 0x10000202, 0x00000202,
    0x30000102, 0x00020102, 0x10010102, 0x00010102, 0x20000102, 0x01000102, 0x10000102, 0x00000102,
    0x40000002, 0x00030002, 0x10020002, 0x00020002, 0x20010002, 0x01010002, 0x10010002, 0x00010002,
    0x30000002, 0x02000002, 0x11000002, 0x01000002, 0x20000002, 0x00000012, 0x10000002, 0x00000002,
    0x60000001, 0x00000501, 0x10000401, 0x00000401, 0x20000301, 0x00010301, 0x10000301, 0x00000301,
    0x30000201, 0x00020201, 0x10010201, 0x00010201, 0x20000201, 0x01000201, 0x10000201, 0x00000201,
    0x40000101, 0x00030101, 0x10020101, 0x00020101, 0x20010101, 0x01010101, 0x10010101, 0x00010101,
    0x30000101, 0x02000101, 0x11000101, 0x01000101, 0x20000101, 0x00000111, 0x10000101, 0x00000101,
    0x50000001, 0x00040001, 0x10030001, 0x00030001, 0x20020001, 0x01020001, 0x10020001, 0x00020001,
    0x30010001, 0x02010001, 0x11010001, 0x01010001, 0x20010001, 0x00010011, 0x10010001, 0x00010001,
    0x40000001, 0x03000001, 0x12000001, 0x02000001, 0x21000001, 0x01000011, 0x11000001, 0x01000001,
    0x30000001, 0x00000021, 0x10000011, 0x00000011, 0x20000001, 0x00001001, 0x10000001, 0x00000001,
    0x70000000, 0x00000600, 0x10000500, 0x00000500, 0x20000400, 0x00010400, 0x10000400, 0x00000400,
    0x30000300, 0x00020300, 0x10010300, 0x00010300, 0x20000300, 0x01000300, 0x10000300, 0x00000300,
    0x40000200, 0x00030200, 0x10020200, 0x00020200, 0x20010200, 0x01010200, 0x10010200, 0x00010200,
    0x30000200, 0x02000200, 0x11000200, 0x01000200, 0x20000200, 0x00000210, 0x10000200, 0x00000200,
    0x50000100, 0x00040100, 0x10030100, 0x00030100, 0x20020100, 0x01020100, 0x10020100, 0x00020100,
    0x30010100, 0x02010100, 0x11010100, 0x01010100, 0x20010100, 0x00010110, 0x10010100, 0x00010100,
    0x40000100, 0x03000100, 0x12000100, 0x02000100, 0x21000100, 0x01000110, 0x11000100, 0x01000100,
    0x30000100, 0x00000120, 0x10000110, 0x00000110, 0x20000100, 0x00001100, 0x10000100, 0x00000100,
    0x60000000, 0x00050000, 0x10040000, 0x00040000, 0x20030000, 0x01030000, 0x10030000, 0x00030000,
    0x30020000, 0x02020000, 0x11020000, 0x01020000, 0x20020000, 0x00020010, 0x10020000, 0x00020000,
    0x40010000, 0x03010000, 0x12010000, 0x02010000, 0x21010000, 0x01010010, 0x11010000, 0x01010000,
    0x30010000, 0x00010020, 0x10010010, 0x00010010, 0x20010000, 0x00011000, 0x10010000, 0x00010000,
    0x50000000, 0x04000000, 0x13000000, 0x03000000, 0x22000000, 0x02000010, 0x12000000, 0x02000000,
    0x31000000, 0x01000020, 0x11000010, 0x01000010, 0x21000000, 0x01001000, 0x11000000, 0x01000000,
    0x40000000, 0x00000030, 0x10000020, 0x00000020, 0x20000010, 0x00001010, 0x10000010, 0x00000010,
    0x30000000, 0x00002000, 0x10001000, 0x00001000, 0x20000000, 0x00100000, 0x10000000, 0x00000000,
};

constexpr u8 kRiceLength[256] = {
    0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4, 1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5,
    1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5, 2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
    1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5, 2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
    2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6, 3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7,
    1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5, 2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
    2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6, 3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7,
    2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6, 3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7,
    3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7, 4, 5, 5, 6, 5, 6, 6, 7, 5, 6, 6, 7, 6, 7, 7, 8,
};

bool put_sym(u8* syms, u32* code_prefix, int codelen, u8 sym) {
    if (codelen < 0 || codelen > 11 || code_prefix[codelen] >= kSymsSize) return false;
    syms[code_prefix[codelen]++] = sym;
    return true;
}

int huff_read_code_lengths_old(BitReader& bits, u8* syms, u32* code_prefix) {
    if (bits.read_bit_no_refill() != 0) {
        int sym = 0, codelen, num_symbols = 0;
        int avg_bits_x4 = 32;
        int forced_bits = static_cast<int>(bits.read_bits_no_refill(2));
        u32 thres = shl(1u, 31 - static_cast<int>(20u >> forced_bits));
        int n;
        bool skip = bits.read_bit() != 0;
        for (;;) {
            if (!skip) {
                if ((bits.bits & 0xFF000000) == 0) return kMalformed;
                sym += static_cast<int>(bits.read_bits_no_refill(2 * (clz(bits.bits) + 1))) - 2 + 1;
                if (sym >= 256) break;
            }
            skip = false;
            bits.refill();
            if ((bits.bits & 0xFF000000) == 0) return kMalformed;
            n = static_cast<int>(bits.read_bits_no_refill(2 * (clz(bits.bits) + 1))) - 2 + 1;
            if (sym + n > 256) return kMalformed;
            bits.refill();
            num_symbols += n;
            do {
                if (bits.bits < thres) return kMalformed;
                int lz = clz(bits.bits);
                int v = static_cast<int>(bits.read_bits_no_refill(lz + forced_bits + 1)) + ishl(lz - 1, forced_bits);
                codelen = (-(v & 1) ^ (v >> 1)) + ((avg_bits_x4 + 2) >> 2);
                if (codelen < 1 || codelen > 11) return kMalformed;
                avg_bits_x4 = codelen + ((3 * avg_bits_x4 + 2) >> 2);
                bits.refill();
                if (!put_sym(syms, code_prefix, codelen, static_cast<u8>(sym++))) return kMalformed;
            } while (--n != 0);
            if (sym == 256) break;
        }
        return (sym == 256 && num_symbols >= 2) ? num_symbols : kMalformed;
    }
    int num_symbols = static_cast<int>(bits.read_bits_no_refill(8));
    if (num_symbols == 0) return kMalformed;
    if (num_symbols == 1) {
        syms[0] = static_cast<u8>(bits.read_bits_no_refill(8));
    } else {
        int codelen_bits = static_cast<int>(bits.read_bits_no_refill(3));
        if (codelen_bits > 4) return kMalformed;
        for (int i = 0; i < num_symbols; ++i) {
            bits.refill();
            int sym = static_cast<int>(bits.read_bits_no_refill(8));
            int codelen = static_cast<int>(bits.read_bits_no_refill_zero(codelen_bits)) + 1;
            if (codelen > 11) return kMalformed;
            if (!put_sym(syms, code_prefix, codelen, static_cast<u8>(sym))) return kMalformed;
        }
    }
    return num_symbols;
}

bool decode_golomb_rice_lengths(Buffer& dst, int size, GolombReader& br) {
    long p = br.p, p_end = br.end;
    long dst_pos = 0, dst_end = size;
    if (p >= p_end) return false;
    int count = -br.bitpos;
    u32 v = static_cast<u32>(at(*br.data, p++) & (255 >> br.bitpos));
    for (;;) {
        if (v == 0) {
            count += 8;
        } else {
            u32 x = kRiceValue[v];
            u32 lo = static_cast<u32>(count) + (x & 0x0F0F0F0F);
            write_le32(dst, dst_pos, lo);
            write_le32(dst, dst_pos + 4, (x >> 4) & 0x0F0F0F0F);
            dst_pos += kRiceLength[v];
            if (dst_pos >= dst_end) break;
            count = static_cast<int>(x >> 28);
        }
        if (p >= p_end) return false;
        v = at(*br.data, p++);
    }
    if (dst_pos > dst_end) {
        long nn = dst_pos - dst_end;
        do {
            v &= v - 1;
        } while (--nn != 0);
    }
    int bitpos = 0;
    if ((v & 1) == 0) {
        --p;
        bitpos = 8 - lsb(v);
    }
    br.p = p;
    br.bitpos = bitpos;
    return true;
}

bool decode_golomb_rice_bits(Buffer& dst, int size, int bitcount, GolombReader& br) {
    if (bitcount == 0) return true;
    long dst_pos = 0, dst_end = size;
    long p = br.p;
    int bitpos = br.bitpos;
    long bits_required = bitpos + static_cast<long>(bitcount) * size;
    long bytes_required = (bits_required + 7) >> 3;
    if (bytes_required > br.end - p) return false;
    br.p = p + (bits_required >> 3);
    br.bitpos = static_cast<int>(bits_required & 7);
    u64 bak = read_le64(dst, dst_end);
    const Buffer& data = *br.data;
    if (bitcount == 1) {
        do {
            u64 bits = static_cast<u8>(shr(swap32(read_le32(data, p)), 24 - bitpos));
            p += 1;
            bits = (bits | (bits << 28)) & 0xF0000000Full;
            bits = (bits | (bits << 14)) & 0x3000300030003ull;
            bits = (bits | (bits << 7)) & 0x0101010101010101ull;
            write_le64(dst, dst_pos, read_le64(dst, dst_pos) * 2 + swap64(bits));
            dst_pos += 8;
        } while (dst_pos < dst_end);
    } else if (bitcount == 2) {
        do {
            u64 bits = static_cast<std::uint16_t>(shr(swap32(read_le32(data, p)), 16 - bitpos));
            p += 2;
            bits = (bits | (bits << 24)) & 0xFF000000FFull;
            bits = (bits | (bits << 12)) & 0xF000F000F000Full;
            bits = (bits | (bits << 6)) & 0x0303030303030303ull;
            write_le64(dst, dst_pos, read_le64(dst, dst_pos) * 4 + swap64(bits));
            dst_pos += 8;
        } while (dst_pos < dst_end);
    } else {
        do {
            u64 bits = static_cast<u64>(shr(swap32(read_le32(data, p)), 8 - bitpos) & 0xFFFFFF);
            p += 3;
            bits = (bits | (bits << 20)) & 0xFFF00000FFFull;
            bits = (bits | (bits << 10)) & 0x3F003F003F003Full;
            bits = (bits | (bits << 5)) & 0x0707070707070707ull;
            write_le64(dst, dst_pos, read_le64(dst, dst_pos) * 8 + swap64(bits));
            dst_pos += 8;
        } while (dst_pos < dst_end);
    }
    write_le64(dst, dst_end, bak);
    return true;
}

int huff_convert_to_ranges(HuffRange* range, int num_symbols, int p, const Buffer& symlen, int symlen_off,
                           BitReader& bits) {
    int num_ranges = p >> 1, v, sym_idx = 0;
    if (num_ranges + 1 > 128) return kMalformed;
    auto len_at = [&](int i) { return i >= 0 && static_cast<std::size_t>(i) < symlen.size() ? int(symlen[i]) : 255; };
    if ((p & 1) != 0) {
        bits.refill();
        v = len_at(symlen_off++);
        if (v >= 8) return kMalformed;
        sym_idx = static_cast<int>(bits.read_bits_no_refill(v + 1)) + ishl(1, v + 1) - 1;
    }
    int syms_used = 0;
    for (int i = 0; i < num_ranges; ++i) {
        bits.refill();
        v = len_at(symlen_off);
        if (v >= 9) return kMalformed;
        int num = static_cast<int>(bits.read_bits_no_refill_zero(v)) + ishl(1, v);
        v = len_at(symlen_off + 1);
        if (v >= 8) return kMalformed;
        int space = static_cast<int>(bits.read_bits_no_refill(v + 1)) + ishl(1, v + 1) - 1;
        range[i].symbol = sym_idx;
        range[i].num = num;
        syms_used += num;
        sym_idx += num + space;
        symlen_off += 2;
    }
    if (sym_idx >= 256 || syms_used >= num_symbols || sym_idx + num_symbols - syms_used > 256) return kMalformed;
    range[num_ranges].symbol = sym_idx;
    range[num_ranges].num = num_symbols - syms_used;
    return num_ranges + 1;
}

int huff_read_code_lengths_new(BitReader& bits, u8* syms, u32* code_prefix) {
    int forced_bits = static_cast<int>(bits.read_bits_no_refill(2));
    int num_symbols = static_cast<int>(bits.read_bits_no_refill(8)) + 1;
    int fluff = bits.read_fluff(num_symbols);

    Buffer code_len(512 + 16, 0);
    GolombReader br2{bits.data, bits.p - ((24 - bits.bitpos + 7) >> 3), bits.bound, (bits.bitpos - 24) & 7};
    if (num_symbols + fluff + 16 > static_cast<int>(code_len.size())) return kMalformed;
    if (!decode_golomb_rice_lengths(code_len, num_symbols + fluff, br2)) return kMalformed;
    for (int z = 0; z < 16; ++z) code_len[num_symbols + fluff + z] = 0;
    if (!decode_golomb_rice_bits(code_len, num_symbols, forced_bits, br2)) return kMalformed;

    bits.bitpos = 24;
    bits.p = br2.p;
    bits.bits = 0;
    bits.refill();
    bits.bits = shl(bits.bits, br2.bitpos);
    bits.bitpos += br2.bitpos;

    u32 running_sum = 0x1e;
    for (int i = 0; i < num_symbols; ++i) {
        int v = code_len[i];
        v = -(v & 1) ^ (v >> 1);
        int cl = v + static_cast<int>(running_sum >> 2) + 1;
        if (cl < 1 || cl > 11) return kMalformed;
        code_len[i] = static_cast<u8>(cl);
        running_sum = static_cast<u32>(static_cast<int>(running_sum) + v);
    }

    HuffRange range[128];
    int ranges = huff_convert_to_ranges(range, num_symbols, fluff, code_len, num_symbols, bits);
    if (ranges <= 0) return kMalformed;
    int cp = 0;
    for (int i = 0; i < ranges; ++i) {
        int sym = range[i].symbol;
        int nn = range[i].num;
        if (nn <= 0) return kMalformed;
        do {
            if (cp >= num_symbols) return kMalformed;
            if (!put_sym(syms, code_prefix, code_len[cp++], static_cast<u8>(sym++))) return kMalformed;
        } while (--nn != 0);
    }
    return num_symbols;
}

bool huff_build_lookup(const u32* prefix_cur, HuffLookup& lut, const u8* syms) {
    u32 currslot = 0;
    for (u32 i = 1; i < 11; ++i) {
        u32 start = kCodePrefixBase[i];
        u32 count = prefix_cur[i] - start;
        if (count == 0) continue;
        u32 stepsize = 1u << (11 - i);
        u32 num_to_set = count << (11 - i);
        if (currslot + num_to_set > 2048 || start + count > kSymsSize) return false;
        std::memset(lut.bits_to_len + currslot, static_cast<int>(i), num_to_set);
        u32 p = currslot;
        for (u32 j = 0; j != count; ++j, p += stepsize) std::memset(lut.bits_to_sym + p, syms[start + j], stepsize);
        currslot += num_to_set;
    }
    if (prefix_cur[11] - kCodePrefixBase[11] != 0) {
        u32 num_to_set = prefix_cur[11] - kCodePrefixBase[11];
        if (currslot + num_to_set > 2048 || kCodePrefixBase[11] + num_to_set > kSymsSize) return false;
        std::memset(lut.bits_to_len + currslot, 11, num_to_set);
        std::memcpy(lut.bits_to_sym + currslot, syms + kCodePrefixBase[11], num_to_set);
        currslot += num_to_set;
    }
    return currslot == 2048;
}

int reverse11(int v) {
    int r = 0;
    for (int k = 0; k < 11; ++k) {
        r = (r << 1) | (v & 1);
        v >>= 1;
    }
    return r;
}

struct HuffStream {
    long src = 0, src_end = 0, src_mid = 0, src_mid_original = 0;
    u32 src_bits = 0, src_mid_bits = 0, src_end_bits = 0;
    int src_bitpos = 0, src_mid_bitpos = 0, src_end_bitpos = 0;
    long out_off = 0, out_end = 0;
};

// The three-stream scalar Huffman core.
bool decode_huffman_core(const Buffer& data, Buffer& out, HuffStream hr, const HuffLookup& lut) {
    long src = hr.src, src_mid = hr.src_mid, src_end = hr.src_end;
    u32 src_bits = hr.src_bits, src_mid_bits = hr.src_mid_bits, src_end_bits = hr.src_end_bits;
    int src_bitpos = hr.src_bitpos, src_mid_bitpos = hr.src_mid_bitpos, src_end_bitpos = hr.src_end_bitpos;
    long dst = hr.out_off, dst_end = hr.out_end;
    if (src > src_mid || dst_end > static_cast<long>(out.size()) || src_end > static_cast<long>(data.size()) || src < 0)
        return false;
    for (;;) {
        if (dst >= dst_end) break;
        if (src_mid - src <= 1) {
            if (src_mid - src == 1) src_bits |= shl(data[src], src_bitpos);
        } else {
            src_bits |= shl(u32(data[src]) | (u32(data[src + 1]) << 8), src_bitpos);
        }
        u32 k = src_bits & 0x7FF;
        int n = lut.bits_to_len[k];
        src_bitpos -= n;
        src_bits = shr(src_bits, n);
        out[dst++] = lut.bits_to_sym[k];
        src += (7 - src_bitpos) >> 3;
        src_bitpos &= 7;
        if (dst < dst_end) {
            if (src_end - src_mid <= 1) {
                if (src_end - src_mid == 1) {
                    src_end_bits |= shl(data[src_mid], src_end_bitpos);
                    src_mid_bits |= shl(data[src_mid], src_mid_bitpos);
                }
            } else {
                u32 vv = u32(data[src_end - 2]) | (u32(data[src_end - 1]) << 8);
                src_end_bits |= shl(((vv >> 8) | (vv << 8)) & 0xFFFF, src_end_bitpos);
                src_mid_bits |= shl(u32(data[src_mid]) | (u32(data[src_mid + 1]) << 8), src_mid_bitpos);
            }
            k = src_end_bits & 0x7FF;
            n = lut.bits_to_len[k];
            out[dst++] = lut.bits_to_sym[k];
            src_end_bitpos -= n;
            src_end_bits = shr(src_end_bits, n);
            src_end -= (7 - src_end_bitpos) >> 3;
            src_end_bitpos &= 7;
            if (dst < dst_end) {
                k = src_mid_bits & 0x7FF;
                n = lut.bits_to_len[k];
                out[dst++] = lut.bits_to_sym[k];
                src_mid_bitpos -= n;
                src_mid_bits = shr(src_mid_bits, n);
                src_mid += (7 - src_mid_bitpos) >> 3;
                src_mid_bitpos &= 7;
            }
        }
        if (src > src_mid || src_mid > src_end) return false;
    }
    return src == hr.src_mid_original && src_end == src_mid;
}

int decode_huffman(const Buffer& src, long src_start, int src_size, Buffer& output, int output_size, int type) {
    long src_end = src_start + src_size;
    BitReader bits = BitReader::forward(src, src_start, src_end);
    u32 code_prefix[12];
    std::memcpy(code_prefix, kCodePrefixBase, sizeof code_prefix);
    u8 syms[kSymsSize] = {};
    int num_syms;
    if (bits.read_bit_no_refill() == 0) num_syms = huff_read_code_lengths_old(bits, syms, code_prefix);
    else if (bits.read_bit_no_refill() == 0) num_syms = huff_read_code_lengths_new(bits, syms, code_prefix);
    else return kMalformed;
    if (num_syms < 1) return kMalformed;

    long sp = bits.p - ((24 - bits.bitpos) / 8);
    if (num_syms == 1) {
        std::memset(output.data(), syms[0], static_cast<std::size_t>(output_size));
        return src_size;
    }

    HuffLookup lut;
    if (!huff_build_lookup(code_prefix, lut, syms)) return kMalformed;
    HuffLookup rev;
    for (int i = 0; i < 2048; ++i) {
        rev.bits_to_len[i] = lut.bits_to_len[reverse11(i)];
        rev.bits_to_sym[i] = lut.bits_to_sym[reverse11(i)];
    }

    if (type == 1) {
        if (sp + 3 > src_end) return kMalformed;
        int split_mid = at(src, sp) | (at(src, sp + 1) << 8);
        sp += 2;
        HuffStream hr;
        hr.out_off = 0;
        hr.out_end = output_size;
        hr.src = sp;
        hr.src_end = src_end;
        hr.src_mid = hr.src_mid_original = sp + split_mid;
        if (!decode_huffman_core(src, output, hr, rev)) return kMalformed;
    } else {
        if (sp + 6 > src_end) return kMalformed;
        int half_out = (output_size + 1) >> 1;
        int split_mid = at(src, sp) | (at(src, sp + 1) << 8) | (at(src, sp + 2) << 16);
        sp += 3;
        if (split_mid > src_end - sp) return kMalformed;
        long src_mid = sp + split_mid;
        int split_left = at(src, sp) | (at(src, sp + 1) << 8);
        sp += 2;
        if (src_mid - sp < split_left + 2 || src_end - src_mid < 3) return kMalformed;
        int split_right = at(src, src_mid) | (at(src, src_mid + 1) << 8);
        if (src_end - (src_mid + 2) < split_right + 2) return kMalformed;
        HuffStream h1;
        h1.out_off = 0;
        h1.out_end = half_out;
        h1.src = sp;
        h1.src_end = src_mid;
        h1.src_mid = h1.src_mid_original = sp + split_left;
        if (!decode_huffman_core(src, output, h1, rev)) return kMalformed;
        HuffStream h2;
        h2.out_off = half_out;
        h2.out_end = output_size;
        h2.src = src_mid + 2;
        h2.src_end = src_end;
        h2.src_mid = h2.src_mid_original = src_mid + 2 + split_right;
        if (!decode_huffman_core(src, output, h2, rev)) return kMalformed;
    }
    return src_size;
}

// Decodes one entropy-coded array. Returns the bytes consumed, or kMalformed/kUnsupported.
int decode_bytes(const Buffer& src, long sp, long src_end, int output_cap, Buffer& output, int& decoded_size) {
    output.clear();
    decoded_size = 0;
    long src_org = sp;
    if (src_end - sp < 2) return kMalformed;
    int chunk_type = (at(src, sp) >> 4) & 0x7;
    int src_size, dst_size;
    if (chunk_type == 0) {
        if (at(src, sp) >= 0x80) {
            src_size = ((at(src, sp) << 8) | at(src, sp + 1)) & 0xFFF;
            sp += 2;
        } else {
            if (src_end - sp < 3) return kMalformed;
            src_size = (at(src, sp) << 16) | (at(src, sp + 1) << 8) | at(src, sp + 2);
            if ((src_size & ~0x3FFFF) != 0) return kMalformed;
            sp += 3;
        }
        if (src_size > output_cap || src_end - sp < src_size) return kMalformed;
        output.assign(src.begin() + sp, src.begin() + sp + src_size);
        decoded_size = src_size;
        return static_cast<int>(sp + src_size - src_org);
    }
    if (at(src, sp) >= 0x80) {
        if (src_end - sp < 3) return kMalformed;
        u32 bits = (u32(at(src, sp)) << 16) | (u32(at(src, sp + 1)) << 8) | at(src, sp + 2);
        src_size = static_cast<int>(bits & 0x3FF);
        dst_size = static_cast<int>(src_size + ((bits >> 10) & 0x3FF) + 1);
        sp += 3;
    } else {
        if (src_end - sp < 5) return kMalformed;
        u32 bits = (u32(at(src, sp + 1)) << 24) | (u32(at(src, sp + 2)) << 16) | (u32(at(src, sp + 3)) << 8) |
                   at(src, sp + 4);
        src_size = static_cast<int>(bits & 0x3FFFF);
        dst_size = static_cast<int>((((bits >> 18) | (u32(at(src, sp)) << 14)) & 0x3FFFF) + 1);
        if (src_size >= dst_size) return kMalformed;
        sp += 5;
    }
    if (src_end - sp < src_size || dst_size > output_cap) return kMalformed;
    output.assign(static_cast<std::size_t>(dst_size), 0);
    int used;
    switch (chunk_type) {
    case 2:
    case 4: used = decode_huffman(src, sp, src_size, output, dst_size, chunk_type >> 1); break;
    default: return kUnsupported; // TANS (1), RLE (3), recursive (5): not produced by these packages
    }
    if (used != src_size) return kMalformed;
    decoded_size = dst_size;
    return static_cast<int>(sp + src_size - src_org);
}

// ---------------------------------------------------------------- LZ

struct LzStreams {
    Buffer literals;
    int literal_count = 0;
    Buffer commands;
    int command_count = 0;
    std::vector<int> distances;
    int distance_count = 0;
    std::vector<int> lengths;
    int length_count = 0;
};

bool unpack_offsets(const Buffer& src, long bs_begin, long bs_end, bool excess_flag, int excess_count,
                    const Buffer& packed_offs, int offs_count, int offs_scaling, const Buffer& packed_offs_extra,
                    const Buffer& packed_lit_len, int len_count, std::vector<int>& offs_stream,
                    std::vector<int>& len_stream) {
    BitReader fwd = BitReader::forward(src, bs_begin, bs_end);
    BitReader bwd = BitReader::reverse(src, bs_begin, bs_end);

    int u32_len_stream_size = 0;
    if (!excess_flag) {
        if (bwd.bits < 0x2000) return false;
        int nn = 31 - msb(bwd.bits);
        bwd.bitpos += nn;
        bwd.bits = shl(bwd.bits, nn);
        bwd.refill_backward();
        ++nn;
        u32_len_stream_size = static_cast<int>(shr(bwd.bits, 32 - nn) - 1);
        bwd.bitpos += nn;
        bwd.bits = shl(bwd.bits, nn);
        bwd.refill_backward();
    } else {
        for (int q = 0; q < len_count; ++q)
            if (packed_lit_len[q] == 255) ++u32_len_stream_size;
    }

    if (offs_scaling == 0) {
        int i = 0;
        while (i < offs_count) {
            offs_stream[i] = -static_cast<int>(fwd.read_distance(packed_offs[i]));
            ++i;
            if (i >= offs_count) break;
            offs_stream[i] = -static_cast<int>(bwd.read_distance(packed_offs[i]));
            ++i;
        }
    } else {
        int i = 0;
        while (i < offs_count) {
            u32 cmd = packed_offs[i];
            if ((cmd >> 3) > 26) return false;
            u32 off = shl(8u + (cmd & 7), static_cast<int>(cmd >> 3)) | fwd.read_extra(static_cast<int>(cmd >> 3));
            offs_stream[i] = 8 - static_cast<int>(off);
            ++i;
            if (i >= offs_count) break;
            cmd = packed_offs[i];
            if ((cmd >> 3) > 26) return false;
            off = shl(8u + (cmd & 7), static_cast<int>(cmd >> 3)) | bwd.read_extra(static_cast<int>(cmd >> 3));
            offs_stream[i] = 8 - static_cast<int>(off);
            ++i;
        }
        if (offs_scaling != 1) {
            if (static_cast<int>(packed_offs_extra.size()) < offs_count) return false;
            for (int k = 0; k < offs_count; ++k)
                offs_stream[k] = static_cast<std::int8_t>(packed_offs_extra[k]) - offs_stream[k] * offs_scaling;
        }
    }

    if (u32_len_stream_size < 0 || u32_len_stream_size > 512) return false;
    std::vector<u32> u32s(static_cast<std::size_t>(u32_len_stream_size));
    BitReader& a = fwd;
    BitReader& b = bwd;
    BitReader ea, eb;
    BitReader* ra = &a;
    BitReader* rb = &b;
    if (excess_flag) {
        ea = BitReader::forward(src, bs_end, bs_end + excess_count);
        eb = BitReader::reverse(src, bs_end, bs_end + excess_count);
        ra = &ea;
        rb = &eb;
    }
    int i = 0;
    for (; i + 1 < u32_len_stream_size; i += 2) {
        if (!ra->read_length(u32s[i])) return false;
        if (!rb->read_length(u32s[i + 1])) return false;
    }
    if (i < u32_len_stream_size && !ra->read_length(u32s[i])) return false;

    long a_seam = fwd.p - ((24 - fwd.bitpos) >> 3);
    long b_seam = bwd.p + ((24 - bwd.bitpos) >> 3);
    if (a_seam != b_seam && !excess_flag) return false;

    int u = 0;
    for (int k = 0; k < len_count; ++k) {
        u32 v = packed_lit_len[k];
        if (v == 255) {
            if (u >= u32_len_stream_size) return false;
            v = u32s[u++] + 255;
        }
        len_stream[k] = static_cast<int>(v) + 3;
    }
    return u == u32_len_stream_size;
}

// Returns the source position after the table, or kMalformed/kUnsupported.
long read_lz_table(const Buffer& src, long sp, long src_end, Buffer& dst, long dst_start, int dst_size, long offset,
                   LzStreams& lzt) {
    if (offset == 0) {
        if (src_end - sp < kSeedSize) return kMalformed;
        std::memcpy(dst.data() + dst_start, src.data() + sp, kSeedSize);
        sp += kSeedSize;
    }
    bool excess_flag = false;
    int excess_count = 0;
    if (sp < src_end && (src[sp] & 0x80) != 0) {
        u8 flag = src[sp++];
        if ((flag & 0xC0) != 0x80) return kMalformed;
        excess_flag = true;
        excess_count = flag & 0x3F;
        if (excess_count > 0x1F) {
            if (sp >= src_end) return kMalformed;
            excess_count += src[sp++] * 0x20;
        }
        src_end -= excess_count;
        if (src_end < sp) return kMalformed;
    }

    int n = decode_bytes(src, sp, src_end, dst_size, lzt.literals, lzt.literal_count);
    if (n < 0) return n;
    sp += n;
    n = decode_bytes(src, sp, src_end, dst_size, lzt.commands, lzt.command_count);
    if (n < 0) return n;
    sp += n;
    if (src_end - sp < 3) return kMalformed;

    int offs_scaling = 0;
    Buffer packed_offs_extra, packed_offs;
    int offs_count = 0;
    if ((src[sp] & 0x80) != 0) {
        offs_scaling = src[sp] - 127;
        ++sp;
        n = decode_bytes(src, sp, src_end, lzt.command_count, packed_offs, offs_count);
        if (n < 0) return n;
        sp += n;
        if (offs_scaling != 1) {
            int extra_count = 0;
            n = decode_bytes(src, sp, src_end, offs_count, packed_offs_extra, extra_count);
            if (n < 0) return n;
            if (extra_count != offs_count) return kMalformed;
            sp += n;
        }
    } else {
        n = decode_bytes(src, sp, src_end, lzt.command_count, packed_offs, offs_count);
        if (n < 0) return n;
        sp += n;
    }
    lzt.distance_count = offs_count;

    Buffer packed_lit_len;
    int len_count = 0;
    n = decode_bytes(src, sp, src_end, dst_size >> 2, packed_lit_len, len_count);
    if (n < 0) return n;
    sp += n;
    lzt.length_count = len_count;
    lzt.distances.assign(static_cast<std::size_t>(offs_count), 0);
    lzt.lengths.assign(static_cast<std::size_t>(len_count), 0);

    return unpack_offsets(src, sp, src_end, excess_flag, excess_count, packed_offs, offs_count, offs_scaling,
                          packed_offs_extra, packed_lit_len, len_count, lzt.distances, lzt.lengths)
        ? sp
        : kMalformed;
}

// Rebuilds a sub-chunk from its streams. `sub` selects delta literals.
bool process_lz_runs(bool sub, const LzStreams& lzt, Buffer& dst, long dst_pos, long dst_end, long window_start) {
    long cmd_pos = 0, len_pos = 0, lit_pos = 0, offs_pos = 0;
    const long cmd_end = lzt.command_count, len_end = lzt.length_count, lit_end = lzt.literal_count,
               offs_end = lzt.distance_count;
    int recent[7] = {0, 0, 0, -8, -8, -8, 0};
    long last_offset = -8;
    const long size = static_cast<long>(dst.size());

    while (cmd_pos < cmd_end) {
        u32 f = lzt.commands[cmd_pos++];
        long litlen = f & 3;
        int offs_index = static_cast<int>(f >> 6);
        u32 matchlen = (f >> 2) & 0xF;
        if (litlen == 3) {
            if (len_pos >= len_end) return false;
            litlen = lzt.lengths[len_pos++];
        }
        recent[6] = offs_pos < offs_end ? lzt.distances[offs_pos] : 0;
        if (litlen != 0) {
            if (litlen < 0 || lit_pos + litlen > lit_end || dst_pos + litlen > dst_end) return false;
            if (sub) {
                if (dst_pos + last_offset < 0 || dst_pos + last_offset + litlen > size) return false;
                for (long c = 0; c < litlen; ++c)
                    dst[dst_pos + c] = static_cast<u8>(lzt.literals[lit_pos + c] + dst[dst_pos + c + last_offset]);
            } else {
                std::memcpy(dst.data() + dst_pos, lzt.literals.data() + lit_pos, static_cast<std::size_t>(litlen));
            }
            dst_pos += litlen;
            lit_pos += litlen;
        }
        int offset = recent[offs_index + 3];
        recent[offs_index + 3] = recent[offs_index + 2];
        recent[offs_index + 2] = recent[offs_index + 1];
        recent[offs_index + 1] = recent[offs_index + 0];
        recent[3] = offset;
        last_offset = offset;
        offs_pos += ((offs_index + 1) & 4) >> 2;

        if (dst_pos + offset < window_start) return false;
        long copy_from = dst_pos + offset;
        long actual;
        if (matchlen != 15) {
            actual = matchlen + 2;
        } else {
            if (len_pos >= len_end) return false;
            actual = 14 + static_cast<long>(lzt.lengths[len_pos++]);
        }
        if (actual < 0 || dst_pos + actual > dst_end || copy_from < 0 || copy_from + actual > size) return false;
        for (long c = 0; c < actual; ++c) dst[dst_pos + c] = dst[copy_from + c]; // may overlap: byte by byte
        dst_pos += actual;
    }
    if (offs_pos != offs_end || len_pos != len_end) return false;
    long final_len = dst_end - dst_pos;
    if (final_len != lit_end - lit_pos) return false;
    if (sub) {
        if (dst_pos + last_offset < 0) return false;
        for (long c = 0; c < final_len; ++c)
            dst[dst_pos + c] = static_cast<u8>(lzt.literals[lit_pos + c] + dst[dst_pos + c + last_offset]);
    } else if (final_len > 0) {
        std::memcpy(dst.data() + dst_pos, lzt.literals.data() + lit_pos, static_cast<std::size_t>(final_len));
    }
    return true;
}

DecodeStatus decode_chunk(const Buffer& src, long src_start, long src_len, Buffer& dst, long dst_start, int dst_size,
                          bool with_seed, bool sub_literals) {
    if (dst_size <= 0 || dst_start + dst_size > static_cast<long>(dst.size()) || src_len < 0 ||
        src_start + src_len > static_cast<long>(src.size()))
        return DecodeStatus::malformed;
    long offset = with_seed ? 0 : dst_start;
    LzStreams streams;
    long rc = read_lz_table(src, src_start, src_start + src_len, dst, dst_start, dst_size, offset, streams);
    if (rc < 0) return rc == kUnsupported ? DecodeStatus::unsupported_entropy : DecodeStatus::malformed;
    long start = dst_start + (offset == 0 ? kSeedSize : 0);
    long window_start = dst_start - offset;
    return process_lz_runs(sub_literals, streams, dst, start, dst_start + dst_size, window_start)
        ? DecodeStatus::success
        : DecodeStatus::malformed;
}

DecodeStatus decode_bare_entropy(const Buffer& src, long src_start, long src_len, Buffer& out, long dst_start,
                                 int dst_len) {
    Buffer decoded;
    int count = 0;
    int used = decode_bytes(src, src_start, src_start + src_len, dst_len, decoded, count);
    if (used < 0) return used == kUnsupported ? DecodeStatus::unsupported_entropy : DecodeStatus::malformed;
    if (used != src_len || count != dst_len) return DecodeStatus::malformed;
    std::memcpy(out.data() + dst_start, decoded.data(), static_cast<std::size_t>(dst_len));
    return DecodeStatus::success;
}

DecodeStatus decode_sub_chunk(const Buffer& src, long src_start, long src_len, Buffer& out, long dst_start,
                              int dst_len, bool new_lz, bool restart, bool sub_literals) {
    if (src_len < 0 || src_start + src_len > static_cast<long>(src.size()) || dst_len < 0 ||
        dst_start + dst_len > static_cast<long>(out.size()))
        return DecodeStatus::malformed;
    if (src_len == dst_len) {
        std::memcpy(out.data() + dst_start, src.data() + src_start, static_cast<std::size_t>(dst_len));
        return DecodeStatus::success;
    }
    return new_lz ? decode_chunk(src, src_start, src_len, out, dst_start, dst_len, restart, sub_literals)
                  : decode_bare_entropy(src, src_start, src_len, out, dst_start, dst_len);
}

} // namespace

DecodeStatus decode_block(Bytes payload, int flags, std::size_t first_chunk_size, MutableBytes dst) {
    if (dst.empty()) return payload.empty() ? DecodeStatus::success : DecodeStatus::malformed;
    if (dst.size() > kMaxBlockSize) return DecodeStatus::malformed;
    Buffer src(payload.begin(), payload.end());
    Buffer out(dst.size(), 0);
    const long out_size = static_cast<long>(dst.size());
    bool chunk0_new_lz = (flags & kChunk0NewLz) != 0;
    bool chunk0_sub = (flags & kChunk0SubLiterals) != 0;

    DecodeStatus status;
    if (dst.size() <= kChunkSize) {
        status = decode_sub_chunk(src, 0, static_cast<long>(src.size()), out, 0, static_cast<int>(out_size),
                                  chunk0_new_lz, true, chunk0_sub);
    } else {
        if (first_chunk_size == 0 || first_chunk_size > src.size()) return DecodeStatus::malformed;
        long first = static_cast<long>(first_chunk_size);
        status = decode_sub_chunk(src, 0, first, out, 0, static_cast<int>(kChunkSize), chunk0_new_lz, true, chunk0_sub);
        if (status != DecodeStatus::success) return status;
        status = decode_sub_chunk(src, first, static_cast<long>(src.size()) - first, out, static_cast<long>(kChunkSize),
                                  static_cast<int>(out_size - static_cast<long>(kChunkSize)),
                                  (flags & kChunk1NewLz) != 0, (flags & kChunk1Restart) != 0,
                                  (flags & kChunk1SubLiterals) != 0);
    }
    if (status == DecodeStatus::success) std::memcpy(dst.data(), out.data(), dst.size());
    return status;
}

} // namespace alembic::kraken
