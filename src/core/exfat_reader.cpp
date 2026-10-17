// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from PS5PKGTool PS5PKGTool.Ffpfsc/ExfatVolume.cs (GPL-3.0).
#include "alembic/exfat_reader.hpp"

#include "alembic/bytes.hpp"
#include "alembic/unicode.hpp"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <vector>

namespace alembic::exfat {

namespace {

constexpr std::uint32_t kFatEndMinimum = 0xFFFFFFF8;
constexpr std::size_t kMaxEntries = 2'000'000;
constexpr int kMaxDepth = 256;
constexpr std::uint64_t kMaxDirectoryBytes = 512ull * 1024 * 1024;

Error invalid(const std::string& message) {
    return make_error(Errc::invalid_data, message);
}

// A run of consecutive clusters.
struct Run {
    std::uint64_t file_offset; // where the run starts within the file
    std::uint32_t cluster;
    std::uint32_t count;
};

struct Extent {
    std::uint32_t first_cluster = 0;
    bool contiguous = false; // NoFatChain: clusters follow each other, FAT unused
    std::uint64_t size = 0;
};

// Days since 1970-01-01 for a proleptic Gregorian date (Howard Hinnant's algorithm).
std::int64_t days_from_civil(std::int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    auto yoe = static_cast<unsigned>(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

// exFAT timestamp (+10 ms increment and UTC offset) to Unix seconds.
std::int64_t exfat_time(std::uint32_t ts, std::uint8_t utc_offset) {
    unsigned second = (ts & 0x1F) * 2, minute = (ts >> 5) & 0x3F, hour = (ts >> 11) & 0x1F;
    unsigned day = (ts >> 16) & 0x1F, month = (ts >> 21) & 0x0F, year = 1980 + (ts >> 25);
    if (day == 0 || month == 0 || month > 12) return 0;
    std::int64_t t = days_from_civil(year, month, day) * 86400 + hour * 3600 + minute * 60 + second;
    if (utc_offset & 0x80) {
        int quarters = (utc_offset & 0x40) ? static_cast<int>(utc_offset & 0x7F) - 128 : utc_offset & 0x7F;
        t -= static_cast<std::int64_t>(quarters) * 15 * 60;
    }
    return t;
}

class ExfatFile final : public RandomAccess {
public:
    ExfatFile(std::shared_ptr<const RandomAccess> image, std::uint64_t heap, std::uint32_t cluster_size,
              std::vector<Run> runs, std::uint64_t size)
        : image_(std::move(image)), heap_(heap), cluster_size_(cluster_size), runs_(std::move(runs)), size_(size) {}

    Result<std::uint64_t> size() const override { return size_; }

    Status read_at(std::uint64_t offset, MutableBytes out) const override {
        if (offset > size_ || out.size() > size_ - offset) return invalid("read past the end of an exFAT file");
        std::size_t done = 0;
        while (done < out.size()) {
            std::uint64_t at = offset + done;
            auto it = std::upper_bound(runs_.begin(), runs_.end(), at,
                                       [](std::uint64_t v, const Run& r) { return v < r.file_offset; });
            const Run& run = *(it - 1);
            std::uint64_t within = at - run.file_offset;
            std::uint64_t run_bytes = static_cast<std::uint64_t>(run.count) * cluster_size_;
            std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(out.size() - done, run_bytes - within));
            std::uint64_t image_offset = heap_ + static_cast<std::uint64_t>(run.cluster - 2) * cluster_size_ + within;
            ALEMBIC_TRY(image_->read_at(image_offset, out.subspan(done, n)));
            done += n;
        }
        return {};
    }

private:
    std::shared_ptr<const RandomAccess> image_;
    std::uint64_t heap_;
    std::uint32_t cluster_size_;
    std::vector<Run> runs_;
    std::uint64_t size_;
};

class ImageSource final : public tree::Source {
public:
    const tree::Node& root() const override { return root_; }
    const std::string& label() const override { return label_; }

    Result<std::shared_ptr<const RandomAccess>> open(const tree::Node& file) const override {
        if (file.is_directory || file.id >= extents_.size())
            return make_error(Errc::invalid_argument, "not a file of this exFAT image");
        const Extent& e = extents_[file.id];
        std::vector<Run> runs;
        if (e.size > 0) {
            ALEMBIC_ASSIGN(runs, clusters(e.first_cluster, e.size, e.contiguous));
        }
        return std::shared_ptr<const RandomAccess>(
            std::make_shared<ExfatFile>(image_, geometry_.heap_offset, geometry_.cluster_size, std::move(runs), e.size));
    }

    Status load(std::shared_ptr<const RandomAccess> image, std::string label) {
        image_ = std::move(image);
        label_ = std::move(label);
        ALEMBIC_ASSIGN(image_size_, image_->size());
        if (image_size_ < 512) return invalid("the image is not a supported exFAT filesystem");
        std::uint8_t boot[512];
        ALEMBIC_TRY(image_->read_at(0, MutableBytes(boot, sizeof boot)));
        if (std::memcmp(boot + 3, "EXFAT   ", 8) != 0 || get_u16(boot + 0x1FE) != 0xAA55)
            return invalid("the image is not a supported exFAT filesystem");
        unsigned sector_shift = boot[0x6C], cluster_shift = boot[0x6D];
        if (sector_shift < 9 || sector_shift > 12 || cluster_shift > 25)
            return invalid("the exFAT sector or cluster shift is invalid");
        geometry_.sector_size = 1u << sector_shift;
        geometry_.cluster_size = geometry_.sector_size << cluster_shift;
        if (sector_shift + cluster_shift > 31) return invalid("the exFAT cluster size is too large");
        std::uint64_t fat_sectors = get_u32(boot + 0x54);
        geometry_.fat_offset = static_cast<std::uint64_t>(get_u32(boot + 0x50)) * geometry_.sector_size;
        geometry_.heap_offset = static_cast<std::uint64_t>(get_u32(boot + 0x58)) * geometry_.sector_size;
        geometry_.cluster_count = get_u32(boot + 0x5C);
        geometry_.root_cluster = get_u32(boot + 0x60);
        std::uint64_t fat_bytes = fat_sectors * geometry_.sector_size;
        std::uint64_t entries = static_cast<std::uint64_t>(geometry_.cluster_count) + 2;
        if (geometry_.cluster_count == 0 || geometry_.root_cluster < 2 || geometry_.root_cluster >= entries ||
            fat_bytes < entries * 4 || geometry_.heap_offset >= image_size_ || entries > INT32_MAX)
            return invalid("the exFAT volume geometry is invalid");

        std::vector<std::uint8_t> fat_data(static_cast<std::size_t>(entries * 4));
        ALEMBIC_TRY(image_->read_at(geometry_.fat_offset, fat_data));
        fat_.resize(static_cast<std::size_t>(entries));
        for (std::size_t i = 0; i < fat_.size(); ++i) fat_[i] = get_u32(fat_data.data() + i * 4);

        root_.is_directory = true;
        std::set<std::uint32_t> visited;
        return read_directory(root_, u"", geometry_.root_cluster, std::nullopt, false, visited, 0);
    }

    Geometry geometry_;

private:
    Result<std::vector<Run>> clusters(std::uint32_t first, std::optional<std::uint64_t> length, bool contiguous) const {
        if (first < 2 || first >= fat_.size()) return invalid("an exFAT entry has an invalid first cluster");
        std::uint64_t expected = length ? div_round_up(*length, geometry_.cluster_size) : 0;
        std::vector<Run> runs;
        if (length && expected == 0) return runs;
        if (contiguous && length) {
            if (static_cast<std::uint64_t>(first) + expected > fat_.size())
                return invalid("a contiguous exFAT extent leaves the cluster heap");
            runs.push_back({0, first, static_cast<std::uint32_t>(expected)});
            return runs;
        }
        // Follow the FAT chain, merging consecutive clusters into runs. Earlier
        // runs go into an interval map so a revisited cluster (a cycle) is caught.
        std::map<std::uint32_t, std::uint32_t> earlier; // first cluster -> one past last
        auto seen = [&](std::uint32_t c) {
            auto it = earlier.upper_bound(c);
            if (it != earlier.begin() && c < std::prev(it)->second) return true;
            const Run& last = runs.back();
            return c >= last.cluster && c < last.cluster + last.count;
        };
        std::uint64_t count = 0;
        std::uint32_t cluster = first;
        while (cluster < kFatEndMinimum) {
            if (cluster < 2 || cluster >= fat_.size() || (!runs.empty() && seen(cluster)))
                return invalid("an exFAT FAT chain is invalid or cyclic");
            if (!runs.empty() && runs.back().cluster + runs.back().count == cluster) {
                ++runs.back().count;
            } else {
                if (!runs.empty()) earlier[runs.back().cluster] = runs.back().cluster + runs.back().count;
                runs.push_back({count * geometry_.cluster_size, cluster, 1});
            }
            ++count;
            if (length && count == expected) break;
            if (count > geometry_.cluster_count) return invalid("an exFAT FAT chain is unreasonably long");
            cluster = fat_[cluster];
        }
        if (length && count != expected) return invalid("an exFAT FAT chain is shorter than its declared length");
        return runs;
    }

    Status read_directory(tree::Node& node, const std::u16string& parent_path, std::uint32_t first_cluster,
                          std::optional<std::uint64_t> data_length, bool contiguous, std::set<std::uint32_t>& visited,
                          int depth) {
        if (depth > kMaxDepth) return invalid("the exFAT directory depth exceeds the safety limit");
        if (!visited.insert(first_cluster).second) return invalid("the exFAT directory graph contains a cycle");
        ALEMBIC_ASSIGN(std::vector<Run> runs, clusters(first_cluster, data_length, contiguous));
        std::uint64_t length = data_length.value_or(0);
        if (!data_length)
            for (const Run& r : runs) length += static_cast<std::uint64_t>(r.count) * geometry_.cluster_size;
        if (length > kMaxDirectoryBytes) return invalid("an exFAT directory is unreasonably large");
        std::vector<std::uint8_t> data(static_cast<std::size_t>(length));
        ExfatFile reader(image_, geometry_.heap_offset, geometry_.cluster_size, std::move(runs), length);
        ALEMBIC_TRY(reader.read_at(0, data));

        for (std::size_t offset = 0; offset + 32 <= data.size();) {
            std::uint8_t type = data[offset];
            if (type == 0) break;
            if (type != 0x85) {
                offset += 32;
                continue;
            }
            std::size_t secondary = data[offset + 1];
            std::size_t set_length = (secondary + 1) * 32;
            if (secondary < 2 || offset + set_length > data.size()) return invalid("an exFAT file entry set is truncated");
            const std::uint8_t* primary = &data[offset];
            const std::uint8_t* stream = &data[offset + 32];
            if (stream[0] != 0xC0) return invalid("an exFAT file entry has no stream extension");
            std::size_t name_length = stream[3];
            std::size_t name_entries = (name_length + 14) / 15;
            if (name_entries > secondary - 1) return invalid("an exFAT file name entry set is incomplete");
            std::u16string name16;
            for (std::size_t i = 0; i < name_entries; ++i) {
                const std::uint8_t* entry = &data[offset + (i + 2) * 32];
                if (entry[0] != 0xC1) return invalid("an exFAT file name extension is missing");
                for (std::size_t k = 0; k < 15 && name16.size() < name_length; ++k)
                    name16.push_back(static_cast<char16_t>(get_u16(entry + 2 + k * 2)));
            }
            bool blank = std::all_of(name16.begin(), name16.end(), [](char16_t c) {
                return c == u' ' || (c >= u'\t' && c <= u'\r') || c == 0x85 || c == 0xA0 || c == 0x3000;
            });
            if (blank || name16 == u"." || name16 == u".." || name16.find_first_of(u"/\\") != std::u16string::npos)
                return invalid("an exFAT entry contains an unsafe file name");

            std::uint16_t attributes = get_u16(primary + 4);
            bool is_directory = (attributes & 0x10) != 0;
            bool no_fat_chain = (stream[1] & 0x02) != 0;
            std::uint32_t child_cluster = get_u32(stream + 0x14);
            std::uint64_t size = get_u64(stream + 0x18);
            if (size > INT64_MAX) return invalid("an exFAT entry is too large");

            std::u16string path16 = parent_path.empty() ? name16 : parent_path + u"/" + name16;
            if (!seen_paths_.insert(unicode::to_lower_invariant(path16)).second)
                return invalid("duplicate exFAT path: " + unicode::utf16_to_utf8(path16));
            if (++entry_count_ > kMaxEntries) return invalid("the exFAT entry count exceeds the safety limit");

            tree::Node child;
            child.name = unicode::utf16_to_utf8(name16);
            child.is_directory = is_directory;
            child.size = is_directory ? 0 : size;
            child.mtime = exfat_time(get_u32(primary + 0x0C), primary[0x17]);
            child.id = extents_.size();
            extents_.push_back({child_cluster, no_fat_chain, size});
            if (is_directory && child_cluster >= 2)
                ALEMBIC_TRY(read_directory(child, path16, child_cluster, size, no_fat_chain, visited, depth + 1));
            node.children.push_back(std::move(child));
            offset += set_length;
        }
        return {};
    }

    std::shared_ptr<const RandomAccess> image_;
    std::uint64_t image_size_ = 0;
    std::string label_;
    std::vector<std::uint32_t> fat_;
    std::vector<Extent> extents_;
    std::set<std::u16string> seen_paths_;
    std::size_t entry_count_ = 0;
    tree::Node root_;
};

} // namespace

bool looks_like_exfat(const RandomAccess& image) {
    std::uint8_t boot[512];
    auto size = image.size();
    if (!size.ok() || *size < sizeof boot || !image.read_at(0, MutableBytes(boot, sizeof boot)).ok()) return false;
    return std::memcmp(boot + 3, "EXFAT   ", 8) == 0 && get_u16(boot + 0x1FE) == 0xAA55;
}

Result<std::unique_ptr<tree::Source>> open_image(std::shared_ptr<const RandomAccess> image, std::string label,
                                                 Geometry* geometry) {
    auto source = std::make_unique<ImageSource>();
    ALEMBIC_TRY(source->load(std::move(image), std::move(label)));
    if (geometry) *geometry = source->geometry_;
    return std::unique_ptr<tree::Source>(std::move(source));
}

} // namespace alembic::exfat
