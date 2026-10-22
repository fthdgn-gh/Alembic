// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from PS5PKGTool PS5PKGTool.Ffpfsc/PfscCodec.cs (GPL-3.0).
#include "alembic/pfsc.hpp"

#include "alembic/bytes.hpp"
#include "alembic/thread_pool.hpp"

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
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

namespace {

// One zlib stream, reused for every block it compresses.
struct Deflater {
    z_stream stream{};
    bool ready = false;
    ~Deflater() {
        if (ready) deflateEnd(&stream);
    }
    bool init(int level) {
        // Same parameters as .NET ZLibStream: zlib wrapper, 32 KiB window, memLevel 8.
        ready = deflateInit2(&stream, level, Z_DEFLATED, 15, 8, Z_DEFAULT_STRATEGY) == Z_OK;
        return ready;
    }
    // Returns the compressed length, or 0 on failure.
    std::uint32_t compress(const std::uint8_t* in, std::uint8_t* out, std::size_t out_capacity) {
        if (deflateReset(&stream) != Z_OK) return 0;
        stream.next_in = const_cast<std::uint8_t*>(in);
        stream.avail_in = kBlockSize;
        stream.next_out = out;
        stream.avail_out = static_cast<uInt>(out_capacity);
        if (deflate(&stream, Z_FINISH) != Z_STREAM_END) return 0;
        return static_cast<std::uint32_t>(stream.total_out);
    }
};

} // namespace

// A ring of block slots. The producer fills a slot and queues it; workers
// compress queued slots; the producer drains slots in submission order before
// reusing them, so output order never depends on scheduling.
struct Encoder::Pipeline {
    struct Slot {
        std::vector<std::uint8_t> input;
        std::vector<std::uint8_t> output;
        std::uint32_t compressed = 0; // 0 = failed
        bool busy = false;            // submitted and not yet drained
        bool done = false;            // compressed, waiting to be drained
    };

    std::vector<Slot> slots;
    std::size_t fill_slot = 0;  // slot being filled by the producer
    std::size_t drain_slot = 0; // oldest submitted slot
    std::size_t in_flight = 0;
    std::uint32_t fill = 0;     // bytes in the fill slot
    int level = 9;

    // Single-threaded mode compresses inline with this stream.
    Deflater inline_deflater;

    // Worker mode.
    std::vector<std::thread> workers;
    std::deque<std::size_t> queue;
    std::mutex mutex;
    std::condition_variable work_ready;
    std::condition_variable slot_done;
    bool stopping = false;
    bool worker_init_failed = false;

    ~Pipeline() { stop(); }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopping = true;
        }
        work_ready.notify_all();
        for (std::thread& t : workers) t.join();
        workers.clear();
    }

    void run_worker() {
        Deflater deflater;
        if (!deflater.init(level)) {
            std::lock_guard<std::mutex> lock(mutex);
            worker_init_failed = true;
        }
        for (;;) {
            std::size_t index;
            {
                std::unique_lock<std::mutex> lock(mutex);
                work_ready.wait(lock, [&] { return stopping || !queue.empty(); });
                if (queue.empty()) return; // stopping
                index = queue.front();
                queue.pop_front();
            }
            Slot& slot = slots[index];
            std::uint32_t length =
                deflater.ready ? deflater.compress(slot.input.data(), slot.output.data(), slot.output.size()) : 0;
            {
                std::lock_guard<std::mutex> lock(mutex);
                slot.compressed = length;
                slot.done = true;
            }
            slot_done.notify_all();
        }
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

void Encoder::set_checkpoints(std::uint64_t interval, std::function<Status(const Checkpoint&)> save) {
    checkpoint_interval_ = std::max<std::uint64_t>(interval, kBlockSize);
    save_checkpoint_ = std::move(save);
}

// Writes the table entries of the blocks finished since the last checkpoint,
// makes everything durable, then records the checkpoint.
Status Encoder::checkpoint() {
    std::uint32_t first = checkpoint_blocks_ + 1, last = blocks_done_;
    if (last >= first) {
        std::vector<std::uint8_t> entries(static_cast<std::size_t>(last - first + 1) * 8);
        for (std::uint32_t i = first; i <= last; ++i) put_u64(entries.data() + (i - first) * 8, offsets_[i]);
        ALEMBIC_TRY(destination_.write_at(start_ + kOffsetTableOffset + static_cast<std::uint64_t>(first) * 8, entries));
    }
    ALEMBIC_TRY(destination_.sync());
    checkpoint_blocks_ = blocks_done_;
    return save_checkpoint_(Checkpoint{blocks_done_, write_position_, compressed_blocks_});
}

Status Encoder::begin(const std::optional<Checkpoint>& from) {
    if (options_.level < 0 || options_.level > 9)
        return make_error(Errc::invalid_argument, "compression level must be between 0 and 9");
    if (options_.minimum_gain_percent < 0 || options_.minimum_gain_percent > 100)
        return make_error(Errc::invalid_argument, "minimum gain must be between 0 and 100 percent");
    std::uint64_t blocks = div_round_up(source_length_, kBlockSize);
    if (blocks > kMaxBlockCount) return make_error(Errc::too_large, "PFSC block count exceeds the safety limit");
    block_count_ = static_cast<std::uint32_t>(blocks);
    header_size_ = header_size_for(source_length_);

    unsigned threads = options_.threads;
    if (threads == 0) threads = std::max(1u, std::thread::hardware_concurrency());
    threads = std::min(threads, 64u);

    pipeline_ = std::make_unique<Pipeline>();
    Pipeline& p = *pipeline_;
    p.level = options_.level;
    // Enough slots to keep every worker busy while the producer fills more.
    std::size_t slot_count = threads == 1 ? 1 : std::size_t(threads) * 4;
    p.slots.resize(slot_count);
    std::size_t bound = compressBound(kBlockSize) + 64;
    for (Pipeline::Slot& slot : p.slots) {
        slot.input.assign(kBlockSize, 0);
        slot.output.resize(bound);
    }
    if (threads == 1) {
        if (!p.inline_deflater.init(options_.level)) return make_error(Errc::io, "zlib deflateInit2 failed");
    } else {
        p.workers.reserve(threads);
        for (unsigned i = 0; i < threads; ++i) p.workers.emplace_back([&p] { p.run_worker(); });
    }

    offsets_.assign(static_cast<std::size_t>(block_count_) + 1, 0);
    offsets_[0] = header_size_;
    write_position_ = header_size_;
    if (from) {
        // The table entries up to the checkpoint were written and synced
        // before it was recorded; anything after it is dropped.
        if (from->blocks > block_count_ || from->compressed > from->blocks)
            return make_error(Errc::invalid_data, "the checkpoint does not fit this PFSC payload");
        std::vector<std::uint8_t> entries((static_cast<std::size_t>(from->blocks) + 1) * 8);
        ALEMBIC_TRY(destination_.read_at(start_ + kOffsetTableOffset, entries));
        for (std::uint32_t i = 0; i <= from->blocks; ++i) {
            offsets_[i] = get_u64(entries.data() + static_cast<std::size_t>(i) * 8);
            std::uint64_t step = i ? offsets_[i] - offsets_[i - 1] : 0;
            if ((i == 0 && offsets_[0] != header_size_) || (i && (offsets_[i] <= offsets_[i - 1] || step > kBlockSize)))
                return make_error(Errc::invalid_data, "the partial file does not match its checkpoint");
        }
        if (offsets_[from->blocks] != from->written)
            return make_error(Errc::invalid_data, "the partial file does not match its checkpoint");
        blocks_done_ = checkpoint_blocks_ = from->blocks;
        compressed_blocks_ = from->compressed;
        write_position_ = from->written;
        received_ = std::min<std::uint64_t>(static_cast<std::uint64_t>(from->blocks) * kBlockSize, source_length_);
        last_progress_ = received_;
        ALEMBIC_TRY(destination_.truncate(start_ + write_position_));
    } else {
        ALEMBIC_TRY(destination_.truncate(start_ + header_size_));
        std::uint8_t first[8];
        put_u64(first, header_size_);
        ALEMBIC_TRY(destination_.write_at(start_ + kOffsetTableOffset, Bytes(first, 8)));
    }
    begun_ = true;
    return {};
}

Status Encoder::write(Bytes data) {
    if (!begun_) return make_error(Errc::invalid_argument, "PFSC encoder used before begin()");
    if (data.size() > source_length_ - received_)
        return make_error(Errc::invalid_argument, "more PFSC input than the declared length");
    received_ += data.size();
    Pipeline& p = *pipeline_;
    while (!data.empty()) {
        Pipeline::Slot& slot = p.slots[p.fill_slot];
        std::size_t take = std::min<std::size_t>(data.size(), kBlockSize - p.fill);
        std::memcpy(slot.input.data() + p.fill, data.data(), take);
        p.fill += static_cast<std::uint32_t>(take);
        data = data.subspan(take);
        if (p.fill == kBlockSize) ALEMBIC_TRY(submit_current(kBlockSize));
    }
    return {};
}

Status Encoder::submit_current(std::uint32_t valid_bytes) {
    Pipeline& p = *pipeline_;
    Pipeline::Slot& slot = p.slots[p.fill_slot];
    if (valid_bytes < kBlockSize) std::memset(slot.input.data() + valid_bytes, 0, kBlockSize - valid_bytes);
    p.fill = 0;
    if (p.workers.empty()) {
        slot.compressed = p.inline_deflater.compress(slot.input.data(), slot.output.data(), slot.output.size());
        slot.busy = slot.done = true;
    } else {
        {
            std::lock_guard<std::mutex> lock(p.mutex);
            slot.busy = true;
            slot.done = false;
            p.queue.push_back(p.fill_slot);
        }
        p.work_ready.notify_one();
    }
    ++p.in_flight;
    p.fill_slot = (p.fill_slot + 1) % p.slots.size();
    // The next slot to fill must be free: write out the oldest result first.
    while (p.slots[p.fill_slot].busy) ALEMBIC_TRY(drain_one());
    return {};
}

Status Encoder::drain_one() {
    Pipeline& p = *pipeline_;
    Pipeline::Slot& slot = p.slots[p.drain_slot];
    if (!p.workers.empty()) {
        std::unique_lock<std::mutex> lock(p.mutex);
        p.slot_done.wait(lock, [&] { return slot.done; });
        if (p.worker_init_failed) return make_error(Errc::io, "zlib deflateInit2 failed");
    }
    if (slot.compressed == 0) return make_error(Errc::io, "zlib deflate failed");
    std::uint32_t compressed_length = slot.compressed;
    double gain = (static_cast<double>(kBlockSize) - compressed_length) * 100.0 / kBlockSize;
    bool keep = compressed_length < kBlockSize && gain >= options_.minimum_gain_percent;
    Bytes selected = keep ? Bytes(slot.output.data(), compressed_length) : Bytes(slot.input.data(), kBlockSize);
    ALEMBIC_TRY(destination_.write_at(start_ + write_position_, selected));
    write_position_ += selected.size();
    if (keep) ++compressed_blocks_;
    offsets_[++blocks_done_] = write_position_;
    slot.busy = slot.done = false;
    --p.in_flight;
    p.drain_slot = (p.drain_slot + 1) % p.slots.size();
    if (save_checkpoint_ &&
        static_cast<std::uint64_t>(blocks_done_ - checkpoint_blocks_) * kBlockSize >= checkpoint_interval_)
        ALEMBIC_TRY(checkpoint());

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
    Pipeline& p = *pipeline_;
    if (p.fill > 0) ALEMBIC_TRY(submit_current(p.fill));
    while (p.in_flight > 0) ALEMBIC_TRY(drain_one());
    p.stop();

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

Result<Info> inspect(const RandomAccess& source, std::uint64_t base_offset, std::optional<std::uint64_t> stored_length) {
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

namespace {

// Turns one block's stored bytes into its 64 KiB.
Status decode_stored(Bytes stored, std::uint32_t index, MutableBytes out) {
    if (stored.size() == kBlockSize) {
        std::memcpy(out.data(), stored.data(), kBlockSize);
        return {};
    }
    z_stream z{};
    if (inflateInit(&z) != Z_OK) return make_error(Errc::io, "zlib inflateInit failed");
    z.next_in = const_cast<Bytef*>(stored.data());
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

Status check_stored_size(const Info& info, std::uint32_t index) {
    std::uint64_t stored_size = info.block_offsets[index + 1] - info.block_offsets[index];
    if (stored_size == 0 || stored_size > kBlockSize)
        return invalid("PFSC block " + std::to_string(index) + " has an invalid stored size");
    return {};
}

} // namespace

Status read_block(const RandomAccess& source, const Info& info, std::uint32_t index, std::uint64_t base_offset,
                  MutableBytes out) {
    if (index >= info.block_count()) return make_error(Errc::invalid_argument, "PFSC block index out of range");
    if (out.size() < kBlockSize) return make_error(Errc::invalid_argument, "PFSC block buffer too small");
    ALEMBIC_TRY(check_stored_size(info, index));
    std::uint64_t first = info.block_offsets[index];
    std::uint64_t stored_size = info.block_offsets[index + 1] - first;
    if (stored_size == kBlockSize) return source.read_at(base_offset + first, out.first(kBlockSize));
    std::vector<std::uint8_t> stored(static_cast<std::size_t>(stored_size));
    ALEMBIC_TRY(source.read_at(base_offset + first, stored));
    return decode_stored(stored, index, out);
}

Status decode(const RandomAccess& source, ByteSink& destination, std::optional<std::uint64_t> content_length,
              std::uint64_t base_offset, std::optional<std::uint64_t> stored_length, const ProgressFn& progress) {
    // Not owned: the reader lives only inside this call.
    std::shared_ptr<const RandomAccess> borrowed(std::shared_ptr<const RandomAccess>(), &source);
    ALEMBIC_ASSIGN(std::shared_ptr<Reader> reader, Reader::open(borrowed, base_offset, stored_length));
    std::uint64_t output_length = content_length.value_or(reader->info().padded_logical_length);
    if (output_length > reader->info().padded_logical_length)
        return make_error(Errc::invalid_argument, "requested length exceeds the PFSC payload");

    std::vector<std::uint8_t> block(kBlockSize);
    std::uint64_t written = 0;
    std::uint64_t last_progress = 0;
    while (written < output_length) {
        std::size_t count = static_cast<std::size_t>(std::min<std::uint64_t>(kBlockSize, output_length - written));
        ALEMBIC_TRY(reader->read_at(written, MutableBytes(block.data(), count)));
        ALEMBIC_TRY(destination.write(Bytes(block.data(), count)));
        written += count;
        if (progress && (written - last_progress >= kProgressStep || written == output_length)) {
            last_progress = written;
            if (!progress({"Decoding PFSC", written, output_length})) return make_error(Errc::cancelled, "cancelled");
        }
    }
    return {};
}

Result<std::shared_ptr<Reader>> Reader::open(std::shared_ptr<const RandomAccess> source, std::uint64_t base_offset,
                                             std::optional<std::uint64_t> stored_length,
                                             std::optional<std::uint64_t> content_length) {
    ALEMBIC_ASSIGN(Info info, inspect(*source, base_offset, stored_length));
    std::uint64_t length = content_length.value_or(info.padded_logical_length);
    if (length > info.padded_logical_length)
        return make_error(Errc::invalid_data, "the declared length exceeds the PFSC payload");
    std::shared_ptr<Reader> reader(new Reader());
    reader->source_ = std::move(source);
    reader->base_offset_ = base_offset;
    reader->length_ = length;
    reader->info_ = std::move(info);
    return reader;
}

namespace {

// Blocks per worker job (512 KiB) and jobs queued per worker.
constexpr std::uint32_t kRunBlocks = 8;
constexpr std::size_t kRunsPerWorker = 2;

// One pool for every Reader: an image can hold thousands of small
// compressed files, each with its own Reader.
Pool* shared_pool() {
    static unsigned workers = worker_count(0);
    if (workers <= 1) return nullptr;
    static Pool pool(workers);
    return &pool;
}

} // namespace

struct Reader::Run {
    std::uint32_t first = 0;
    std::uint32_t count = 0;
    std::vector<std::uint8_t> stored;  // the blocks' stored bytes, back to back
    std::vector<std::uint8_t> decoded; // count * kBlockSize
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    std::uint32_t good = 0; // blocks decoded before `error`
    Status error;
};

// Jobs own their Run, so they need nothing from the Reader.
Reader::~Reader() = default;

void Reader::fill_ahead(std::uint32_t from) const {
    Pool* pool = shared_pool();
    if (!pool) return;
    const std::size_t window = pool->size() * kRunsPerWorker;
    std::uint32_t next = std::max(queued_up_to_, from);
    const std::uint32_t total = info_.block_count();
    while (ahead_.size() < window && next < total) {
        auto run = std::make_shared<Run>();
        run->first = next;
        run->count = std::min(kRunBlocks, total - next);
        std::uint64_t begin = info_.block_offsets[next];
        std::uint64_t end = info_.block_offsets[next + run->count];
        run->stored.resize(static_cast<std::size_t>(end - begin));
        // A failed read stops here; the block is read again when it is wanted.
        if (!source_->read_at(base_offset_ + begin, run->stored).ok()) break;
        run->decoded.resize(static_cast<std::size_t>(run->count) * kBlockSize);
        std::vector<std::uint64_t> offsets(info_.block_offsets.begin() + next,
                                           info_.block_offsets.begin() + next + run->count + 1);
        pool->submit([run, offsets = std::move(offsets)](unsigned) {
            Status status;
            std::uint32_t good = 0;
            for (; good < run->count; ++good) {
                std::uint64_t size = offsets[good + 1] - offsets[good];
                std::uint32_t index = run->first + good;
                if (size == 0 || size > kBlockSize) {
                    status = invalid("PFSC block " + std::to_string(index) + " has an invalid stored size");
                    break;
                }
                status = decode_stored(Bytes(run->stored.data() + (offsets[good] - offsets[0]), size), index,
                                       MutableBytes(run->decoded.data() + std::size_t(good) * kBlockSize, kBlockSize));
                if (!status.ok()) break;
            }
            run->stored = {};
            std::lock_guard<std::mutex> lock(run->mutex);
            run->good = good;
            run->error = std::move(status);
            run->done = true;
            run->cv.notify_all();
        });
        ahead_.push_back(std::move(run));
        next += kRunBlocks;
    }
    queued_up_to_ = std::min(next, total);
}

Result<const std::uint8_t*> Reader::from_run(const Run& run, std::uint32_t index) const {
    std::uint32_t at = index - run.first;
    if (at >= run.good) return Error(run.error.error());
    return static_cast<const std::uint8_t*>(run.decoded.data() + std::size_t(at) * kBlockSize);
}

Result<const std::uint8_t*> Reader::block(std::uint32_t index) const {
    if (current_ && index >= current_->first && index - current_->first < current_->count) {
        last_ = index;
        return from_run(*current_, index);
    }
    constexpr std::size_t kCapacity = 16;
    for (CachedBlock& cached : cache_) {
        if (cached.index != index) continue;
        cached.last_used = ++clock_;
        return static_cast<const std::uint8_t*>(cached.data.data());
    }

    // Read ahead: the block may be on its way already.
    while (!ahead_.empty() && ahead_.front()->first + ahead_.front()->count <= index) ahead_.pop_front();
    if (!ahead_.empty() && ahead_.front()->first <= index) {
        current_ = ahead_.front();
        ahead_.pop_front();
        {
            std::unique_lock<std::mutex> lock(current_->mutex);
            current_->cv.wait(lock, [&] { return current_->done; });
        }
        last_ = index;
        fill_ahead(current_->first + current_->count);
        return from_run(*current_, index);
    }
    // Anything else is a jump: what was queued is of no use.
    bool next_in_line = last_ != UINT32_MAX && index == last_ + 1;
    ahead_.clear();
    queued_up_to_ = 0;

    CachedBlock* slot;
    if (cache_.size() < kCapacity) {
        cache_.push_back({index, 0, std::vector<std::uint8_t>(kBlockSize)});
        slot = &cache_.back();
    } else {
        slot = &*std::min_element(cache_.begin(), cache_.end(),
                                  [](const CachedBlock& a, const CachedBlock& b) { return a.last_used < b.last_used; });
    }
    slot->index = UINT32_MAX; // invalid until the decode succeeds
    ALEMBIC_TRY(read_block(*source_, info_, index, base_offset_, slot->data));
    slot->index = index;
    slot->last_used = ++clock_;
    last_ = index;
    // Two blocks in a row start the read-ahead; a payload with only a few
    // blocks left is not worth it.
    if (read_ahead_ && next_in_line && info_.block_count() - index > kRunBlocks) fill_ahead(index + 1);
    return static_cast<const std::uint8_t*>(slot->data.data());
}

Status Reader::read_at(std::uint64_t offset, MutableBytes out) const {
    if (offset > length_ || out.size() > length_ - offset)
        return make_error(Errc::invalid_data, "read past the end of the PFSC payload");
    std::size_t done = 0;
    while (done < out.size()) {
        std::uint64_t position = offset + done;
        auto index = static_cast<std::uint32_t>(position / kBlockSize);
        auto within = static_cast<std::size_t>(position % kBlockSize);
        std::size_t n = std::min<std::size_t>(out.size() - done, kBlockSize - within);
        ALEMBIC_ASSIGN(const std::uint8_t* data, block(index));
        std::memcpy(out.data() + done, data + within, n);
        done += n;
    }
    return {};
}

} // namespace alembic::pfsc
