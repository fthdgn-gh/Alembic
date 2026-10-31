// SPDX-License-Identifier: GPL-3.0-or-later
// Kraken ("newLZ") encoder for PS5 package blocks. Ported from
// pearlxcore/ProsperoPkgTool (MIT) Compression/KrakenLzEncoder.cs and
// Compression/KrakenHuffmanArrayEncoder.cs, byte for byte: an LZ77 parse with
// a hash-chain match finder and lazy matching, raw or Huffman-coded arrays,
// and the decoder's "excess" framing for long lengths.
//
// Every working buffer lives in the caller's EncoderScratch and is reused
// from block to block: on the console, threads that allocate per block
// spend their time waiting for the allocator (2026-10-31, /api/bench).
#include "alembic/kraken.hpp"

#include <algorithm>
#include <bit>
#include <cstring>

namespace alembic::kraken {

namespace {

constexpr int kAlphabet = 256;
constexpr int kMaxBits = 11;

// MSB-first bits into a reused vector, padded with zeros to a byte.
class MsbBits {
public:
    explicit MsbBits(std::vector<std::uint8_t>& bytes) : bytes_(bytes) { bytes_.clear(); }
    void bit(unsigned b) {
        cur_ = (cur_ << 1) | (b & 1);
        if (++bits_ == 8) {
            bytes_.push_back(static_cast<std::uint8_t>(cur_));
            cur_ = 0;
            bits_ = 0;
        }
    }
    void bits(std::uint32_t value, int count) {
        for (int i = count - 1; i >= 0; --i) bit((value >> i) & 1);
    }
    // Pads the last byte; the bytes are in the vector given.
    void finish() {
        if (bits_ > 0) bytes_.push_back(static_cast<std::uint8_t>(cur_ << (8 - bits_)));
        cur_ = 0;
        bits_ = 0;
    }

private:
    std::vector<std::uint8_t>& bytes_;
    std::uint32_t cur_ = 0;
    int bits_ = 0;
};

// LSB-first bits into a reused vector, the last byte padded.
class LsbBits {
public:
    explicit LsbBits(std::vector<std::uint8_t>& bytes) : bytes_(bytes) { bytes_.clear(); }
    void bits(std::uint32_t value, int count) {
        if (count == 0) return;
        acc_ |= (value & ((1u << count) - 1)) << bits_;
        bits_ += count;
        while (bits_ >= 8) {
            bytes_.push_back(static_cast<std::uint8_t>(acc_));
            acc_ >>= 8;
            bits_ -= 8;
        }
    }
    void finish() {
        if (bits_ > 0) bytes_.push_back(static_cast<std::uint8_t>(acc_));
        acc_ = 0;
        bits_ = 0;
    }

private:
    std::vector<std::uint8_t>& bytes_;
    std::uint32_t acc_ = 0;
    int bits_ = 0;
};

} // namespace

// The reusable buffers (EncoderScratch::buffers).
struct EncoderScratch::Buffers {
    // The parse: the arrays and the bit regions of one chunk.
    std::vector<std::uint8_t> literal, command, offs, lengths;
    std::vector<std::uint32_t> escapes;
    std::vector<std::uint8_t> forward, backward, excess_forward, excess_backward, region, excess;
    // Huffman arrays.
    std::vector<std::uint8_t> code_lengths, table, stream[3], huffman;
    std::vector<int> present, left, right, leaf_of, leaves, current, packages, merged, stack;
    std::vector<std::int64_t> weight;
};

EncoderScratch::EncoderScratch() : buffers(std::make_unique<Buffers>()) {}
EncoderScratch::~EncoderScratch() = default;
EncoderScratch::EncoderScratch(EncoderScratch&&) noexcept = default;
EncoderScratch& EncoderScratch::operator=(EncoderScratch&&) noexcept = default;

namespace {

using Buffers = EncoderScratch::Buffers;

// ---------------------------------------------------------------------------
// Huffman-coded arrays (DecodeBytes chunk type 2, one 3-stream split).

// Package-merge code lengths limited to kMaxBits into b.code_lengths; false
// when the code is not complete.
bool limited_lengths(const int* freq, Buffers& b) {
    std::vector<int>& present = b.present;
    present.clear();
    for (int s = 0; s < kAlphabet; ++s)
        if (freq[s] > 0) present.push_back(s);
    const int n = static_cast<int>(present.size());
    if (n == 0) return false;
    std::vector<std::uint8_t>& lengths = b.code_lengths;
    lengths.assign(kAlphabet, 0);
    if (n == 1) {
        lengths[present[0]] = 1;
        return true;
    }
    std::sort(present.begin(), present.end(), [&](int x, int y) { return freq[x] != freq[y] ? freq[x] < freq[y] : x < y; });

    std::vector<std::int64_t>& weight = b.weight;
    std::vector<int>&left = b.left, &right = b.right, &leaf_of = b.leaf_of, &leaves = b.leaves;
    weight.clear();
    left.clear();
    right.clear();
    leaf_of.clear();
    leaves.clear();
    for (int i = 0; i < n; ++i) {
        weight.push_back(freq[present[i]]);
        left.push_back(-1);
        right.push_back(-1);
        leaf_of.push_back(present[i]);
        leaves.push_back(static_cast<int>(weight.size()) - 1);
    }
    std::vector<int>& current = b.current;
    current = leaves;
    for (int level = 1; level < kMaxBits; ++level) {
        std::vector<int>& packages = b.packages;
        packages.clear();
        for (std::size_t i = 0; i + 1 < current.size(); i += 2) {
            int x = current[i], y = current[i + 1];
            weight.push_back(weight[x] + weight[y]);
            left.push_back(x);
            right.push_back(y);
            leaf_of.push_back(-1);
            packages.push_back(static_cast<int>(weight.size()) - 1);
        }
        // Merge by weight, leaves first on ties.
        std::vector<int>& merged = b.merged;
        merged.clear();
        std::size_t i = 0, j = 0;
        while (i < leaves.size() && j < packages.size())
            merged.push_back(weight[leaves[i]] <= weight[packages[j]] ? leaves[i++] : packages[j++]);
        while (i < leaves.size()) merged.push_back(leaves[i++]);
        while (j < packages.size()) merged.push_back(packages[j++]);
        current.swap(merged);
    }

    int counts[kAlphabet] = {};
    const std::size_t select = static_cast<std::size_t>(2 * n - 2);
    std::vector<int>& stack = b.stack;
    stack.clear();
    for (std::size_t i = 0; i < select && i < current.size(); ++i) {
        stack.push_back(current[i]);
        while (!stack.empty()) {
            int c = stack.back();
            stack.pop_back();
            if (leaf_of[c] >= 0) {
                ++counts[leaf_of[c]];
            } else {
                stack.push_back(left[c]);
                stack.push_back(right[c]);
            }
        }
    }
    std::int64_t kraft = 0;
    for (int s = 0; s < kAlphabet; ++s) {
        int length = counts[s];
        if (freq[s] > 0 && (length < 1 || length > kMaxBits)) return false;
        lengths[s] = static_cast<std::uint8_t>(length);
        if (freq[s] > 0) kraft += std::int64_t(1) << (kMaxBits - length);
    }
    return kraft == (std::int64_t(1) << kMaxBits);
}

// A DecodeBytes type-2 record into b.huffman; false when it is not
// representable or not smaller.
bool huffman_array(Bytes data, Buffers& b) {
    const std::size_t d = data.size();
    if (d < 2 || d > 0x3FFFF) return false;
    int freq[kAlphabet] = {};
    for (std::uint8_t c : data) ++freq[c];
    int distinct = 0;
    for (int s = 0; s < kAlphabet; ++s)
        if (freq[s]) ++distinct;
    if (distinct == 1) {
        // The decoder rejects a one-symbol array: add a phantom second symbol.
        int real = 0;
        while (!freq[real]) ++real;
        freq[real == 0 ? 1 : 0] = 1;
        distinct = 2;
    }
    if (!limited_lengths(freq, b)) return false;
    const std::vector<std::uint8_t>& lengths = b.code_lengths;

    // Canonical codes in (length, symbol) order from a running kMaxBits slot, bit-reversed.
    std::uint16_t reversed[kAlphabet] = {};
    {
        std::uint32_t slot = 0;
        for (int length = 1; length <= kMaxBits; ++length)
            for (int s = 0; s < kAlphabet; ++s) {
                if (lengths[s] != length) continue;
                std::uint32_t code = slot >> (kMaxBits - length), r = 0;
                for (int i = 0; i < length; ++i) {
                    r = (r << 1) | (code & 1);
                    code >>= 1;
                }
                reversed[s] = static_cast<std::uint16_t>(r);
                slot += 1u << (kMaxBits - length);
            }
    }

    // The simple code-length list: [0][0][count:8][width:3], then {symbol:8, length-1:width}.
    if (distinct < 2 || distinct >= kAlphabet) return false;
    int max_length = 0;
    for (int s = 0; s < kAlphabet; ++s) max_length = std::max<int>(max_length, lengths[s]);
    const int width = std::bit_width(static_cast<unsigned>(max_length - 1));
    if (width > 4) return false;
    {
        MsbBits header(b.table);
        header.bits(0, 1);
        header.bits(0, 1);
        header.bits(static_cast<std::uint32_t>(distinct), 8);
        header.bits(static_cast<std::uint32_t>(width), 3);
        for (int s = 0; s < kAlphabet; ++s) {
            if (!lengths[s]) continue;
            header.bits(static_cast<std::uint32_t>(s), 8);
            if (width > 0) header.bits(lengths[s] - 1u, width);
        }
        header.finish();
    }
    const std::vector<std::uint8_t>& table = b.table;

    {
        LsbBits s0(b.stream[0]), s1(b.stream[1]), s2(b.stream[2]);
        LsbBits* streams[3] = {&s0, &s1, &s2};
        for (std::size_t i = 0; i < d; ++i) streams[i % 3]->bits(reversed[data[i]], lengths[data[i]]);
        s0.finish();
        s1.finish();
        s2.finish();
    }
    const std::vector<std::uint8_t>&a = b.stream[0], &bs = b.stream[1], &c = b.stream[2];
    if (a.size() > 0xFFFF) return false; // the split field is 16 bits

    const std::size_t body = table.size() + 2 + a.size() + c.size() + bs.size();
    if (body >= d) return false;
    std::vector<std::uint8_t>& out = b.huffman;
    out.resize(5 + body);
    const auto dm1 = static_cast<std::uint32_t>(d - 1);
    out[0] = static_cast<std::uint8_t>((2 << 4) | ((dm1 >> 14) & 0xF));
    const std::uint32_t sizes = static_cast<std::uint32_t>(body) | ((dm1 & 0x3FFF) << 18);
    out[1] = static_cast<std::uint8_t>(sizes >> 24);
    out[2] = static_cast<std::uint8_t>(sizes >> 16);
    out[3] = static_cast<std::uint8_t>(sizes >> 8);
    out[4] = static_cast<std::uint8_t>(sizes);
    std::size_t p = 5;
    std::memcpy(&out[p], table.data(), table.size());
    p += table.size();
    out[p++] = static_cast<std::uint8_t>(a.size());
    out[p++] = static_cast<std::uint8_t>(a.size() >> 8);
    if (!a.empty()) std::memcpy(&out[p], a.data(), a.size());
    p += a.size();
    if (!c.empty()) std::memcpy(&out[p], c.data(), c.size());
    p += c.size();
    for (std::size_t i = 0; i < bs.size(); ++i) out[p + i] = bs[bs.size() - 1 - i];
    return true;
}

// An entropy array: Huffman when smaller, else raw (a 3-byte big-endian length and the bytes).
void append_array(std::vector<std::uint8_t>& out, const std::vector<std::uint8_t>& data, Buffers& b) {
    const std::size_t raw_size = 3 + data.size();
    if (data.size() >= 2 && huffman_array(Bytes(data.data(), data.size()), b) && b.huffman.size() < raw_size) {
        out.insert(out.end(), b.huffman.begin(), b.huffman.end());
        return;
    }
    out.push_back(static_cast<std::uint8_t>(data.size() >> 16));
    out.push_back(static_cast<std::uint8_t>(data.size() >> 8));
    out.push_back(static_cast<std::uint8_t>(data.size()));
    out.insert(out.end(), data.begin(), data.end());
}

// ---------------------------------------------------------------------------
// The newLZ parse.

constexpr int kSeed = 8;           // literal bytes before the first match
constexpr int kMinMatch = 4;
constexpr int kNoMatchZone = 16;   // no match may start in a chunk's last 16 bytes
constexpr int kLiteralTail = 8;    // nor cover its last 8
constexpr std::size_t kMaxEscapes = 512;
constexpr int kHashBits = 18;
constexpr int kMaxChain = 128;

struct Finder {
    const std::uint8_t* data;
    int n;
    std::vector<int>& head;
    std::vector<int>& prev;
    int next_insert = 0;

    int hash(int p) const {
        std::uint32_t v = data[p] | (std::uint32_t(data[p + 1]) << 8) | (std::uint32_t(data[p + 2]) << 16) |
                          (std::uint32_t(data[p + 3]) << 24);
        return static_cast<int>((v * 2654435761u) >> (32 - kHashBits));
    }
    void insert_upto(int upto) {
        while (next_insert < upto && next_insert + 3 < n) {
            int h = hash(next_insert);
            prev[next_insert] = head[h];
            head[h] = next_insert;
            ++next_insert;
        }
        if (next_insert < upto) next_insert = upto;
    }
    void find(int pos, int match_end_limit, int& best_length, int& best_distance) const {
        best_length = 0;
        best_distance = 0;
        const int max_length = match_end_limit - pos;
        if (max_length < kMinMatch) return;
        int candidate = head[hash(pos)];
        for (int chain = 0; candidate >= 0 && chain < kMaxChain; ++chain) {
            const int distance = pos - candidate;
            if (distance >= kSeed && data[candidate] == data[pos] && data[candidate + 1] == data[pos + 1] &&
                data[candidate + 2] == data[pos + 2] && data[candidate + 3] == data[pos + 3]) {
                int length = kMinMatch;
                while (length < max_length && data[candidate + length] == data[pos + length]) ++length;
                if (length > best_length) {
                    best_length = length;
                    best_distance = distance;
                    if (length >= max_length) break;
                }
            }
            candidate = prev[candidate];
        }
    }
};

// Inverts the decoder's forward/backward length reader; false outside its range.
bool write_length(MsbBits& w, std::uint32_t value) {
    const std::uint32_t code = value + 64;
    const int zeros = std::bit_width(code) - 1 - 6;
    if (zeros < 0 || zeros > 12) return false;
    for (int i = 0; i < zeros; ++i) w.bit(0);
    w.bit(1);
    const int width = zeros + 6;
    w.bits(code - (1u << width), width);
    return true;
}

// Forward bits, then the backward bits mirrored from the end, into `out`.
void region(const std::vector<std::uint8_t>& f, const std::vector<std::uint8_t>& b, std::vector<std::uint8_t>& out) {
    out.resize(f.size() + b.size());
    std::copy(f.begin(), f.end(), out.begin());
    for (std::size_t i = 0; i < b.size(); ++i) out[out.size() - 1 - i] = b[i];
}

bool encode_chunk(Finder& finder, int chunk_start, int chunk_end, bool with_seed, std::vector<std::uint8_t>& out, Buffers& b) {
    const std::uint8_t* data = finder.data;
    const int parse_start = with_seed ? kSeed : chunk_start;
    const int match_start_limit = chunk_end - kNoMatchZone;
    const int match_end_limit = chunk_end - kLiteralTail;

    std::vector<std::uint8_t>&literal = b.literal, &command = b.command, &offs = b.offs, &lengths = b.lengths;
    literal.clear();
    command.clear();
    offs.clear();
    lengths.clear();
    std::vector<std::uint32_t>& escapes = b.escapes;
    escapes.clear();
    MsbBits forward(b.forward), backward(b.backward);
    int offset_index = 0;
    int literal_start = parse_start;
    int pos = parse_start;

    while (pos < match_start_limit) {
        finder.insert_upto(pos);
        int match_length, match_distance;
        finder.find(pos, match_end_limit, match_length, match_distance);
        if (match_length < kMinMatch) {
            ++pos;
            continue;
        }
        // Lazy matching: a noticeably longer match one byte later wins.
        if (pos + 1 < match_start_limit) {
            finder.insert_upto(pos + 1);
            int lazy_length, lazy_distance;
            finder.find(pos + 1, match_end_limit, lazy_length, lazy_distance);
            if (lazy_length > match_length + 1) {
                ++pos;
                continue;
            }
        }

        const int lit_length = pos - literal_start;
        literal.insert(literal.end(), data + literal_start, data + pos);
        const int lit_field = lit_length <= 2 ? lit_length : 3;
        const int match_field = match_length <= 16 ? match_length - 2 : 15;
        command.push_back(static_cast<std::uint8_t>(0xC0 | (match_field << 2) | lit_field));

        // Offset: a 5-bit exponent and 3-bit mantissa byte, plus extra bits.
        const int off = match_distance + kSeed;
        int e = 0;
        while ((off >> e) >= 16) ++e;
        const int base = (off >> e) - 8;
        const auto extra = static_cast<std::uint32_t>(off - ((8 + base) << e));
        offs.push_back(static_cast<std::uint8_t>((e << 3) | base));
        if (e > 0) ((offset_index & 1) == 0 ? forward : backward).bits(extra, e);
        ++offset_index;

        if (lit_length >= 3) {
            if (lit_length - 3 < 255) {
                lengths.push_back(static_cast<std::uint8_t>(lit_length - 3));
            } else {
                lengths.push_back(255);
                escapes.push_back(static_cast<std::uint32_t>(lit_length - 258));
            }
        }
        if (match_length > 16) {
            if (match_length - 17 < 255) {
                lengths.push_back(static_cast<std::uint8_t>(match_length - 17));
            } else {
                lengths.push_back(255);
                escapes.push_back(static_cast<std::uint32_t>(match_length - 272));
            }
        }
        if (escapes.size() > kMaxEscapes) return false;
        pos += match_length;
        literal_start = pos;
    }
    finder.insert_upto(chunk_end);
    literal.insert(literal.end(), data + literal_start, data + chunk_end);

    // Escaped lengths: alternately forward and backward.
    {
        MsbBits ef(b.excess_forward), eb(b.excess_backward);
        for (std::size_t i = 0; i < escapes.size(); ++i)
            if (!write_length((i & 1) == 0 ? ef : eb, escapes[i])) return false;
        ef.finish();
        eb.finish();
    }
    region(b.excess_forward, b.excess_backward, b.excess);
    forward.finish();
    backward.finish();
    region(b.forward, b.backward, b.region);

    if (with_seed) out.insert(out.end(), data, data + kSeed);
    const std::size_t count = b.excess.size();
    if (count <= 31) {
        out.push_back(static_cast<std::uint8_t>(0x80 | count));
    } else {
        const std::size_t high = (count - 32) / 32;
        out.push_back(static_cast<std::uint8_t>(0x80 | (count - high * 32)));
        out.push_back(static_cast<std::uint8_t>(high));
    }
    append_array(out, literal, b);
    append_array(out, command, b);
    out.push_back(0x80); // offsets: one table, scaling 1
    append_array(out, offs, b);
    append_array(out, lengths, b);
    out.insert(out.end(), b.region.begin(), b.region.end());
    out.insert(out.end(), b.excess.begin(), b.excess.end());
    return true;
}

} // namespace

std::optional<EncodedBlock> encode_block(Bytes input) {
    EncoderScratch scratch;
    return encode_block(input, scratch);
}

std::optional<EncodedBlock> encode_block(Bytes input, EncoderScratch& scratch) {
    if (input.size() < static_cast<std::size_t>(kSeed) || input.size() > kMaxBlockSize) return std::nullopt;
    // Every call starts the head table afresh; `prev` is only read where written.
    std::vector<int>& head = scratch.head;
    std::vector<int>& prev = scratch.prev;
    head.assign(std::size_t(1) << kHashBits, -1);
    if (prev.size() < input.size()) prev.resize(input.size());
    const int n = static_cast<int>(input.size());
    Finder finder{input.data(), n, head, prev};
    Buffers& b = *scratch.buffers;

    EncodedBlock result;
    result.payload.reserve(input.size());
    const int first_end = std::min<int>(n, static_cast<int>(kChunkSize));
    if (!encode_chunk(finder, 0, first_end, true, result.payload, b)) return std::nullopt;
    if (n <= static_cast<int>(kChunkSize)) {
        if (result.payload.size() >= input.size()) return std::nullopt;
        result.flags = kChunk0NewLz;
        return result;
    }
    result.first_chunk_size = result.payload.size();
    if (!encode_chunk(finder, static_cast<int>(kChunkSize), n, false, result.payload, b)) return std::nullopt;
    if (result.payload.size() >= input.size()) return std::nullopt;
    result.flags = kChunk0NewLz | kChunk1NewLz;
    return result;
}

} // namespace alembic::kraken
