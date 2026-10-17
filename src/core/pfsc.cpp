// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from PS5PKGTool PS5PKGTool.Ffpfsc/PfscCodec.cs (GPL-3.0).
#include "alembic/pfsc.hpp"

#include "alembic/bytes.hpp"

#include <algorithm>
#include <cstring>
#include <zlib.h>

namespace alembic::pfsc {

namespace {

constexpr std::uint64_t kProgressStep = 8ull * 1024 * 1024;

Error invalid(const std::string& message) {
    return make_error(Errc::invalid_data, message);
}

} // namespace

std::uint64_t header_size_for(std::uint64_t source_length) {
    std::uint64_t blocks = div_round_up(source_length, kBlockSize);
    std::uint64_t table = (blocks + 1) * 8;
    std::uint64_t capacity = kInitialDataOffset - kOffsetTableOffset;
    std::uint64_t extra = table > capacity ? table - capacity : 0;
    return kInitialDataOffset + div_round_up(extra, kBlockSize) * kBlockSize;
}

struct Encoder::Zlib {
    z_stream stream{};
    bool ready = false;
    ~Zlib() {
        if (ready) deflateEnd(&stream);
    }
};

Encoder::Encoder(File& destination, std::uint64_t start, std::uint64_t source_length, CompressionOptions options,
                 ProgressFn progress)
    : destination_(destination),
      start_(start),
      source_length_(source_length),
      options_(options),
      progress_(std::move(progress)) {}

Encoder::~Encoder() = default;

Status Encoder::begin() {
    if (options_.level < 0 || options_.level > 9)
        return make_error(Errc::invalid_argument, "compression level must be between 0 and 9");
    if (options_.minimum_gain_percent < 0 || options_.minimum_gain_percent > 100)
        return make_error(Errc::invalid_argument, "minimum gain must be between 0 and 100 percent");
    std::uint64_t blocks = div_round_up(source_length_, kBlockSize);
    if (blocks > kMaxBlockCount) return make_error(Errc::too_large, "PFSC block count exceeds the safety limit");
    block_count_ = static_cast<std::uint32_t>(blocks);
    header_size_ = header_size_for(source_length_);

    zlib_ = std::make_unique<Zlib>();
    // Same parameters as .NET ZLibStream: zlib wrapper, 32 KiB window, memLevel 8.
    if (deflateInit2(&zlib_->stream, options_.level, Z_DEFLATED, 15, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        return make_error(Errc::io, "zlib deflateInit2 failed");
    zlib_->ready = true;

    block_.assign(kBlockSize, 0);
    compressed_.resize(deflateBound(&zlib_->stream, kBlockSize));
    offsets_.assign(static_cast<std::size_t>(block_count_) + 1, 0);
    offsets_[0] = header_size_;
    write_position_ = header_size_;
    ALEMBIC_TRY(destination_.truncate(start_ + header_size_));
    begun_ = true;
    return {};
}

Status Encoder::write(Bytes data) {
    if (!begun_) return make_error(Errc::invalid_argument, "PFSC encoder used before begin()");
    if (data.size() > source_length_ - received_)
        return make_error(Errc::invalid_argument, "more PFSC input than the declared length");
    received_ += data.size();
    while (!data.empty()) {
        std::size_t take = std::min<std::size_t>(data.size(), kBlockSize - block_fill_);
        std::memcpy(block_.data() + block_fill_, data.data(), take);
        block_fill_ += static_cast<std::uint32_t>(take);
        data = data.subspan(take);
        if (block_fill_ == kBlockSize) ALEMBIC_TRY(flush_block(kBlockSize));
    }
    return {};
}

Status Encoder::flush_block(std::uint32_t valid_bytes) {
    if (valid_bytes < kBlockSize) std::memset(block_.data() + valid_bytes, 0, kBlockSize - valid_bytes);
    block_fill_ = 0;

    z_stream& z = zlib_->stream;
    if (deflateReset(&z) != Z_OK) return make_error(Errc::io, "zlib deflateReset failed");
    z.next_in = block_.data();
    z.avail_in = kBlockSize;
    z.next_out = compressed_.data();
    z.avail_out = static_cast<uInt>(compressed_.size());
    if (deflate(&z, Z_FINISH) != Z_STREAM_END) return make_error(Errc::io, "zlib deflate failed");
    auto compressed_length = static_cast<std::uint32_t>(z.total_out);

    double gain = (static_cast<double>(kBlockSize) - compressed_length) * 100.0 / kBlockSize;
    bool keep = compressed_length < kBlockSize && gain >= options_.minimum_gain_percent;
    Bytes selected = keep ? Bytes(compressed_.data(), compressed_length) : Bytes(block_.data(), kBlockSize);
    ALEMBIC_TRY(destination_.write_at(start_ + write_position_, selected));
    write_position_ += selected.size();
    if (keep) ++compressed_blocks_;
    offsets_[++blocks_done_] = write_position_;

    std::uint64_t processed = std::min<std::uint64_t>(static_cast<std::uint64_t>(blocks_done_) * kBlockSize, source_length_);
    if (progress_ && (processed - last_progress_ >= kProgressStep || blocks_done_ == block_count_)) {
        last_progress_ = processed;
        if (!progress_({"Compressing PFSC", processed, source_length_}))
            return make_error(Errc::cancelled, "cancelled");
    }
    return {};
}

Result<WriteResult> Encoder::finish() {
    if (!begun_) return make_error(Errc::invalid_argument, "PFSC encoder used before begin()");
    if (received_ != source_length_)
        return make_error(Errc::invalid_argument, "the PFSC source ended before its declared length");
    if (block_fill_ > 0) ALEMBIC_TRY(flush_block(block_fill_));

    std::uint8_t header[kHeaderRecordSize] = {};
    put_u32(header + 0x00, kMagic);
    put_u32(header + 0x04, 0);
    put_u32(header + 0x08, kVersionField);
    put_u32(header + 0x0C, kBlockSize);
    put_u64(header + 0x10, kBlockSize);
    put_u64(header + 0x18, kOffsetTableOffset);
    put_u64(header + 0x20, header_size_);
    put_u64(header + 0x28, static_cast<std::uint64_t>(block_count_) * kBlockSize);
    ALEMBIC_TRY(destination_.write_at(start_, Bytes(header, sizeof header)));

    std::vector<std::uint8_t> table(offsets_.size() * 8);
    for (std::size_t i = 0; i < offsets_.size(); ++i) put_u64(table.data() + i * 8, offsets_[i]);
    ALEMBIC_TRY(destination_.write_at(start_ + kOffsetTableOffset, table));

    WriteResult result;
    result.source_length = source_length_;
    result.padded_logical_length = static_cast<std::uint64_t>(block_count_) * kBlockSize;
    result.stored_length = write_position_;
    result.header_size = header_size_;
    result.block_count = block_count_;
    result.compressed_block_count = compressed_blocks_;
    return result;
}

Result<WriteResult> encode_file(const File& source, File& destination, std::uint64_t start,
                                CompressionOptions options, const ProgressFn& progress) {
    ALEMBIC_ASSIGN(std::uint64_t length, source.size());
    Encoder encoder(destination, start, length, options, progress);
    ALEMBIC_TRY(encoder.begin());
    std::vector<std::uint8_t> buffer(1 << 20);
    for (std::uint64_t done = 0; done < length;) {
        std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), length - done));
        ALEMBIC_TRY(source.read_at(done, MutableBytes(buffer.data(), n)));
        ALEMBIC_TRY(encoder.write(Bytes(buffer.data(), n)));
        done += n;
    }
    return encoder.finish();
}

Result<Info> inspect(const File& source, std::uint64_t base_offset, std::optional<std::uint64_t> stored_length) {
    ALEMBIC_ASSIGN(std::uint64_t file_size, source.size());
    if (base_offset > file_size) return make_error(Errc::invalid_argument, "PFSC base offset is outside the file");
    std::uint64_t available = stored_length.value_or(file_size - base_offset);
    if (available < kHeaderRecordSize || available > file_size - base_offset)
        return invalid("the PFSC stored range is outside the source file");

    std::uint8_t h[kHeaderRecordSize];
    ALEMBIC_TRY(source.read_at(base_offset, MutableBytes(h, sizeof h)));
    std::uint64_t table_offset = get_u64(h + 0x18);
    std::uint64_t data_offset = get_u64(h + 0x20);
    std::uint64_t logical_length = get_u64(h + 0x28);

    if (get_u32(h + 0x00) != kMagic) return invalid("PFSC magic is missing");
    if (get_u32(h + 0x04) != 0 || get_u32(h + 0x08) != kVersionField) return invalid("unsupported PFSC header version");
    if (get_u32(h + 0x0C) != kBlockSize || get_u64(h + 0x10) != kBlockSize)
        return invalid("unsupported or mismatched PFSC block size");
    if (table_offset < kHeaderRecordSize || table_offset > available)
        return invalid("PFSC offset table is outside the stored payload");
    if (data_offset < kInitialDataOffset || data_offset > available || data_offset % kBlockSize != 0)
        return invalid("PFSC data offset is invalid");
    if (logical_length > INT64_MAX || logical_length % kBlockSize != 0) return invalid("PFSC logical length is invalid");

    std::uint64_t block_count = logical_length / kBlockSize;
    if (block_count > kMaxBlockCount) return invalid("PFSC block count exceeds the safety limit");
    std::uint64_t table_bytes = (block_count + 1) * 8;
    if (table_offset > data_offset || table_bytes > data_offset - table_offset)
        return invalid("PFSC offset table overlaps the data region");

    std::vector<std::uint8_t> table(static_cast<std::size_t>(table_bytes));
    ALEMBIC_TRY(source.read_at(base_offset + table_offset, table));
    Info info;
    info.offset_table_offset = table_offset;
    info.data_offset = data_offset;
    info.padded_logical_length = logical_length;
    info.stored_length = available;
    info.block_offsets.resize(static_cast<std::size_t>(block_count) + 1);
    for (std::size_t i = 0; i < info.block_offsets.size(); ++i) {
        std::uint64_t value = get_u64(table.data() + i * 8);
        if (value < data_offset || value > available) return invalid("PFSC block offset is outside the stored payload");
        if (i > 0 && value < info.block_offsets[i - 1]) return invalid("PFSC block offsets are not monotonic");
        info.block_offsets[i] = value;
    }
    if (info.block_offsets[0] != data_offset) return invalid("PFSC first block does not begin at the data offset");
    return info;
}

Status read_block(const File& source, const Info& info, std::uint32_t index, std::uint64_t base_offset,
                  MutableBytes out) {
    if (index >= info.block_count()) return make_error(Errc::invalid_argument, "PFSC block index out of range");
    if (out.size() < kBlockSize) return make_error(Errc::invalid_argument, "PFSC block buffer too small");
    std::uint64_t first = info.block_offsets[index];
    std::uint64_t stored_size = info.block_offsets[index + 1] - first;
    if (stored_size == 0 || stored_size > kBlockSize)
        return invalid("PFSC block " + std::to_string(index) + " has an invalid stored size");
    if (stored_size == kBlockSize) return source.read_at(base_offset + first, out.first(kBlockSize));

    std::vector<std::uint8_t> stored(static_cast<std::size_t>(stored_size));
    ALEMBIC_TRY(source.read_at(base_offset + first, stored));
    z_stream z{};
    if (inflateInit(&z) != Z_OK) return make_error(Errc::io, "zlib inflateInit failed");
    z.next_in = stored.data();
    z.avail_in = static_cast<uInt>(stored.size());
    z.next_out = out.data();
    z.avail_out = kBlockSize;
    int rc = inflate(&z, Z_FINISH);
    std::uint64_t produced = z.total_out;
    inflateEnd(&z);
    if (rc != Z_STREAM_END || produced != kBlockSize)
        return invalid("PFSC block " + std::to_string(index) + " did not decode to exactly 65536 bytes");
    return {};
}

Status decode(const File& source, ByteSink& destination, std::optional<std::uint64_t> content_length,
              std::uint64_t base_offset, std::optional<std::uint64_t> stored_length, const ProgressFn& progress) {
    ALEMBIC_ASSIGN(Info info, inspect(source, base_offset, stored_length));
    std::uint64_t output_length = content_length.value_or(info.padded_logical_length);
    if (output_length > info.padded_logical_length)
        return make_error(Errc::invalid_argument, "requested length exceeds the PFSC payload");

    std::vector<std::uint8_t> block(kBlockSize);
    std::uint64_t written = 0;
    std::uint64_t last_progress = 0;
    for (std::uint32_t i = 0; i < info.block_count() && written < output_length; ++i) {
        ALEMBIC_TRY(read_block(source, info, i, base_offset, block));
        std::size_t count = static_cast<std::size_t>(std::min<std::uint64_t>(kBlockSize, output_length - written));
        ALEMBIC_TRY(destination.write(Bytes(block.data(), count)));
        written += count;
        if (progress && (written - last_progress >= kProgressStep || written == output_length)) {
            last_progress = written;
            if (!progress({"Decoding PFSC", written, output_length})) return make_error(Errc::cancelled, "cancelled");
        }
    }
    if (written != output_length) return invalid("PFSC output length does not match the requested length");
    return {};
}

} // namespace alembic::pfsc
