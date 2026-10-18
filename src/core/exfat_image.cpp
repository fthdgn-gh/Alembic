// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from PS5PKGTool PS5PKGTool.Ffpfsc/ExfatImage.cs (GPL-3.0).
//
// Image layout:
//   sectors 0-11   main boot region      sectors 12-23  backup boot region
//   sector 128     FAT (every allocated cluster gets an explicit chain)
//   cluster heap   allocation bitmap, up-case table, then every directory and
//                  file in pre-order walk order, each in contiguous clusters.
#include "alembic/exfat.hpp"

#include "alembic/ampr.hpp"
#include "alembic/bytes.hpp"
#include "alembic/tree.hpp"
#include "alembic/unicode.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

namespace alembic::exfat {

namespace {

constexpr std::uint32_t kSectorSize = 512;
constexpr std::uint32_t kFatOffsetSectors = 128;
constexpr std::uint32_t kFatEnd = 0xFFFFFFFF;
constexpr std::uint32_t kFirstCluster = 2;
constexpr std::size_t kChunkSize = 1024 * 1024;
constexpr std::uint32_t kVolumeSerial = 0x4D6B5046; // "MkPF", as MkPFS deterministic images
// 2024-01-01 00:00:00 in exFAT timestamp format, used for every entry.
constexpr std::uint32_t kFixedTimestamp = ((2024u - 1980u) << 25) | (1u << 21) | (1u << 16);

constexpr std::uint8_t kUpcaseTable[] = {
#include "data/exfat_upcase_table.inc"
};

struct Node {
    std::string name;          // UTF-8, as on disk
    std::u16string name16;     // UTF-16, as stored in exFAT
    std::u16string sort_key;   // lower-cased name16
    std::string relative_path; // UTF-8, '/' separated
    const tree::Node* source = nullptr; // null for the root and virtual files
    bool is_directory = false;
    bool is_virtual = false;
    std::uint64_t size = 0;
    std::int64_t mtime = 0;
    std::vector<std::uint8_t> virtual_content;
    std::vector<Node> children;
    std::uint32_t first_cluster = 0;
    std::uint32_t cluster_count = 0;
};

void sort_children(std::vector<Node>& children) {
    std::stable_sort(children.begin(), children.end(),
                     [](const Node& a, const Node& b) { return a.sort_key < b.sort_key; });
}

void walk(Node& node, std::vector<Node*>& out) {
    out.push_back(&node);
    for (Node& child : node.children) walk(child, out);
}

Status validate_name(const std::u16string& name, const std::string& utf8) {
    bool bad = name.empty() || name.size() > 255 || name == u"." || name == u"..";
    for (char16_t c : name)
        if (c < 0x20 || std::u16string_view(u"\"*/:<>?\\|").find(c) != std::u16string_view::npos) bad = true;
    if (bad) return make_error(Errc::invalid_data, "the file name cannot be represented in exFAT: " + utf8);
    return {};
}

// Copies a source tree into the writer's own nodes: drops OS junk files,
// checks names fit exFAT, and sorts children the way PS5PKGTool does
// (.NET ToLowerInvariant, then UTF-16 ordinal; stable over source order).
Status convert_tree(const tree::Node& source, Node& parent) {
    std::vector<Node> children;
    for (const tree::Node& child : source.children) {
        if (tree::is_os_junk(child.name)) continue;
        auto name16 = unicode::utf8_to_utf16(child.name);
        if (!name16.ok())
            return make_error(Errc::invalid_data, "the file name is not valid UTF-8: " + parent.relative_path + "/" + child.name);
        Node node;
        node.name = child.name;
        node.name16 = std::move(name16.value());
        node.sort_key = unicode::to_lower_invariant(node.name16);
        node.relative_path = parent.relative_path.empty() ? child.name : parent.relative_path + "/" + child.name;
        node.source = &child;
        node.is_directory = child.is_directory;
        node.size = child.is_directory ? 0 : child.size;
        node.mtime = child.mtime;
        children.push_back(std::move(node));
    }
    sort_children(children);
    // exFAT ignores case; a case-sensitive source (UFS2, Linux) can hold names it cannot.
    for (std::size_t i = 1; i < children.size(); ++i)
        if (children[i].sort_key == children[i - 1].sort_key)
            return make_error(Errc::unsupported, "exFAT cannot hold both '" + children[i - 1].relative_path +
                                                     "' and '" + children[i].relative_path +
                                                     "': names differ only by case");
    for (Node& child : children) {
        ALEMBIC_TRY(validate_name(child.name16, child.relative_path));
        if (child.is_directory) ALEMBIC_TRY(convert_tree(*child.source, child));
    }
    parent.children = std::move(children);
    return {};
}

bool is_stale_index(const Node& n) {
    return !n.is_directory &&
           (unicode::ascii_iequals(n.name, "ampr_emu.index") || unicode::ascii_iequals(n.name, "ampr_emu.index.tmp"));
}

Result<std::vector<std::uint8_t>> index_for_tree(Node& root, ampr::Scheme scheme) {
    std::vector<Node*> nodes;
    walk(root, nodes);
    std::vector<ampr::FileRecord> records;
    for (const Node* n : nodes)
        if (!n->is_directory) records.push_back({n->relative_path, n->size, n->mtime});
    return ampr::build(records, scheme);
}

Status add_virtual_ampr_index(Node& root, ampr::Scheme scheme) {
    std::vector<Node*> nodes;
    walk(root, nodes);
    bool has_marker = std::any_of(nodes.begin(), nodes.end(), [](const Node* n) {
        return !n->is_directory && unicode::ascii_iequals(n->relative_path, "fakelib/libSceAmpr.sprx");
    });
    if (!has_marker) return {};
    // An LZ4-packed tree must keep the index it was packed against: every
    // pack record is tied to a file ID of that index.
    bool packed = std::any_of(root.children.begin(), root.children.end(), [](const Node& n) {
        return !n.is_directory && unicode::ascii_iequals(n.name, "ampr_assets.index");
    });
    if (packed) return {};

    std::erase_if(root.children, is_stale_index);
    ALEMBIC_ASSIGN(std::vector<std::uint8_t> index, index_for_tree(root, scheme));
    if (index.empty()) return {};

    Node node;
    node.name = "ampr_emu.index";
    node.name16 = u"ampr_emu.index";
    node.sort_key = node.name16;
    node.relative_path = node.name;
    node.is_virtual = true;
    node.size = index.size();
    node.virtual_content = std::move(index);
    root.children.push_back(std::move(node));
    sort_children(root.children);
    return {};
}

std::uint32_t table_checksum(const std::uint8_t* data, std::size_t size) {
    std::uint32_t checksum = 0;
    for (std::size_t i = 0; i < size; ++i) checksum = ((checksum >> 1) | (checksum << 31)) + data[i];
    return checksum;
}

Result<std::vector<std::uint16_t>> build_upcase_map() {
    std::vector<std::uint16_t> map(0x10000, 0);
    std::size_t offset = 0;
    std::uint32_t code_point = 0;
    const std::size_t length = sizeof kUpcaseTable;
    while (offset < length && code_point < map.size()) {
        std::uint16_t value = get_u16(kUpcaseTable + offset);
        offset += 2;
        if (value != 0xFFFF) {
            map[code_point++] = value;
            continue;
        }
        if (offset == length && code_point == 0xFFFF) {
            map[code_point++] = 0xFFFF;
            continue;
        }
        if (offset + 2 > length) return make_error(Errc::invalid_data, "up-case table ends inside an identity run");
        std::uint32_t count = get_u16(kUpcaseTable + offset);
        offset += 2;
        if (count == 0 || count > map.size() - code_point)
            return make_error(Errc::invalid_data, "up-case table has an invalid identity run");
        for (std::uint32_t i = 0; i < count; ++i, ++code_point) map[code_point] = static_cast<std::uint16_t>(code_point);
    }
    if (offset != length || code_point != map.size())
        return make_error(Errc::invalid_data, "up-case table does not cover the Unicode BMP");
    return map;
}

std::uint16_t name_hash(const std::u16string& name, const std::vector<std::uint16_t>& upcase) {
    std::uint16_t hash = 0;
    for (char16_t c : name) {
        std::uint16_t upper = upcase[c];
        hash = static_cast<std::uint16_t>(((hash >> 1) | (hash << 15)) + (upper & 0xFF));
        hash = static_cast<std::uint16_t>(((hash >> 1) | (hash << 15)) + (upper >> 8));
    }
    return hash;
}

std::uint16_t entry_set_checksum(const std::vector<std::uint8_t>& entries) {
    std::uint16_t checksum = 0;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (i == 2 || i == 3) continue;
        checksum = static_cast<std::uint16_t>(((checksum >> 1) | (checksum << 15)) + entries[i]);
    }
    return checksum;
}

std::uint32_t name_entry_count(const Node& node) {
    return static_cast<std::uint32_t>(div_round_up(node.name16.size(), 15));
}

std::uint64_t directory_entry_count(const Node& node, bool is_root) {
    std::uint64_t count = is_root ? 3 : 0;
    for (const Node& child : node.children) count += 2 + name_entry_count(child);
    return count;
}

int log2_exact(std::uint32_t value) {
    int result = 0;
    while (value > 1) {
        value >>= 1;
        ++result;
    }
    return result;
}

// Coalesces small writes, reports progress and counts bytes.
class BufferedSink {
public:
    BufferedSink(ByteSink& sink, std::uint64_t total, const ProgressFn& progress)
        : sink_(sink), total_(total), progress_(progress) {
        buffer_.reserve(kChunkSize);
    }

    Status write(Bytes data) {
        if (buffer_.size() + data.size() > kChunkSize) ALEMBIC_TRY(flush());
        if (data.size() >= kChunkSize) {
            ALEMBIC_TRY(sink_.write(data));
            return advance(data.size());
        }
        buffer_.insert(buffer_.end(), data.begin(), data.end());
        return {};
    }

    Status zeros(std::uint64_t count) {
        static const std::vector<std::uint8_t> kZeros(kChunkSize, 0);
        while (count > 0) {
            std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(count, kChunkSize));
            ALEMBIC_TRY(write(Bytes(kZeros.data(), n)));
            count -= n;
        }
        return {};
    }

    Status flush() {
        if (buffer_.empty()) return {};
        ALEMBIC_TRY(sink_.write(buffer_));
        std::size_t n = buffer_.size();
        buffer_.clear();
        return advance(n);
    }

    std::uint64_t written() const { return written_ + buffer_.size(); }

private:
    Status advance(std::size_t n) {
        written_ += n;
        if (progress_ && !progress_({"Building exFAT", written_, total_})) return make_error(Errc::cancelled, "cancelled");
        return {};
    }

    ByteSink& sink_;
    std::uint64_t total_;
    const ProgressFn& progress_;
    std::vector<std::uint8_t> buffer_;
    std::uint64_t written_ = 0;
};

} // namespace

struct ImageBuilder::Impl {
    std::unique_ptr<tree::Source> owned_source; // set by scan()
    const tree::Source* source = nullptr;
    Node root;
    std::vector<Node*> nodes; // pre-order walk, root first
    std::vector<std::uint16_t> upcase_map;
    std::uint32_t upcase_checksum = 0;
    std::uint32_t cluster_size = 0;
    std::uint32_t bitmap_clusters = 0;
    std::uint32_t upcase_clusters = 0;
    std::uint32_t cluster_count = 0;
    std::uint32_t fat_length_sectors = 0;
    std::uint32_t heap_offset_sectors = 0;
    std::uint64_t image_length = 0;
    std::uint32_t files = 0;
    std::uint32_t directories = 0;

    Status plan();
    Status emit(BufferedSink& out) const;
    std::vector<std::uint8_t> boot_region() const;
    Status emit_fat(BufferedSink& out) const;
    Status emit_bitmap(BufferedSink& out) const;
    Status emit_directory(BufferedSink& out, const Node& directory) const;
    Status emit_file(BufferedSink& out, const Node& file) const;
    std::vector<std::uint8_t> file_entry_set(const Node& child) const;
};

Status ImageBuilder::Impl::plan() {
    const std::uint64_t kIntMax = INT32_MAX;
    std::uint64_t upcase = div_round_up(sizeof kUpcaseTable, cluster_size);
    std::uint64_t content = upcase;
    for (Node* node : nodes) {
        std::uint64_t clusters = node->is_directory
            ? std::max<std::uint64_t>(1, div_round_up(directory_entry_count(*node, node == &root) * 32, cluster_size))
            : div_round_up(node->size, cluster_size);
        if (clusters > kIntMax) return make_error(Errc::too_large, "a file is too large for this exFAT layout");
        node->cluster_count = static_cast<std::uint32_t>(clusters);
        content += clusters;
        if (content > kIntMax) return make_error(Errc::too_large, "the dump is too large for this exFAT layout");
    }

    std::uint32_t sectors_per_cluster = cluster_size / kSectorSize;
    std::uint64_t bitmap = 1;
    for (;;) {
        std::uint64_t needed = div_round_up(div_round_up(content + bitmap, 8), cluster_size);
        if (needed == bitmap) break;
        bitmap = needed;
    }
    std::uint64_t clusters = content + bitmap;
    if (clusters > kIntMax) return make_error(Errc::too_large, "the dump is too large for this exFAT layout");
    std::uint64_t fat_sectors = align_up(div_round_up((clusters + 2) * 4, kSectorSize), sectors_per_cluster);
    std::uint64_t heap_offset = align_up(kFatOffsetSectors + fat_sectors, sectors_per_cluster);
    if (fat_sectors > kIntMax || heap_offset > kIntMax)
        return make_error(Errc::too_large, "the dump is too large for this exFAT layout");
    std::uint64_t volume_sectors = heap_offset + clusters * sectors_per_cluster;
    if (volume_sectors > static_cast<std::uint64_t>(UINT32_MAX) * sectors_per_cluster)
        return make_error(Errc::too_large, "the dump is too large for this exFAT layout");

    std::uint64_t next = kFirstCluster + bitmap + upcase;
    for (Node* node : nodes) {
        if (node->cluster_count == 0) continue;
        node->first_cluster = static_cast<std::uint32_t>(next);
        next += node->cluster_count;
    }
    if (next != kFirstCluster + clusters) return make_error(Errc::invalid_data, "exFAT layout accounting failed");

    bitmap_clusters = static_cast<std::uint32_t>(bitmap);
    upcase_clusters = static_cast<std::uint32_t>(upcase);
    cluster_count = static_cast<std::uint32_t>(clusters);
    fat_length_sectors = static_cast<std::uint32_t>(fat_sectors);
    heap_offset_sectors = static_cast<std::uint32_t>(heap_offset);
    image_length = volume_sectors * kSectorSize;
    return {};
}

std::vector<std::uint8_t> ImageBuilder::Impl::boot_region() const {
    std::vector<std::uint8_t> region(12 * kSectorSize, 0);
    std::uint8_t* vbr = region.data();
    vbr[0] = 0xEB;
    vbr[1] = 0x76;
    vbr[2] = 0x90;
    std::memcpy(vbr + 3, "EXFAT   ", 8);
    put_u64(vbr + 0x48, image_length / kSectorSize);
    put_u32(vbr + 0x50, kFatOffsetSectors);
    put_u32(vbr + 0x54, fat_length_sectors);
    put_u32(vbr + 0x58, heap_offset_sectors);
    put_u32(vbr + 0x5C, cluster_count);
    put_u32(vbr + 0x60, root.first_cluster);
    put_u32(vbr + 0x64, kVolumeSerial);
    put_u16(vbr + 0x68, 0x0100);
    vbr[0x6C] = 9;
    vbr[0x6D] = static_cast<std::uint8_t>(log2_exact(cluster_size / kSectorSize));
    vbr[0x6E] = 1;
    vbr[0x6F] = 0x80;
    vbr[0x70] = 0xFF;
    put_u16(vbr + 0x1FE, 0xAA55);
    for (std::uint32_t sector = 1; sector <= 8; ++sector) put_u32(vbr + (sector + 1) * kSectorSize - 4, 0xAA550000);

    std::uint32_t checksum = 0;
    for (std::uint32_t i = 0; i < 11 * kSectorSize; ++i) {
        if (i == 106 || i == 107 || i == 112) continue;
        checksum = ((checksum >> 1) | (checksum << 31)) + region[i];
    }
    for (std::uint32_t i = 11 * kSectorSize; i < 12 * kSectorSize; i += 4) put_u32(vbr + i, checksum);
    return region;
}

Status ImageBuilder::Impl::emit_fat(BufferedSink& out) const {
    // Last cluster of every chain, in increasing order: bitmap, up-case, then nodes.
    std::vector<std::uint32_t> chain_ends;
    chain_ends.push_back(kFirstCluster + bitmap_clusters - 1);
    chain_ends.push_back(kFirstCluster + bitmap_clusters + upcase_clusters - 1);
    for (const Node* node : nodes)
        if (node->cluster_count > 0) chain_ends.push_back(node->first_cluster + node->cluster_count - 1);
    std::sort(chain_ends.begin(), chain_ends.end());

    std::uint64_t entries = static_cast<std::uint64_t>(fat_length_sectors) * kSectorSize / 4;
    std::size_t next_end = 0;
    std::uint8_t value[4];
    for (std::uint64_t cluster = 0; cluster < entries; ++cluster) {
        std::uint32_t v;
        if (cluster == 0) v = 0xFFFFFFF8;
        else if (cluster == 1) v = kFatEnd;
        else if (cluster > static_cast<std::uint64_t>(cluster_count) + 1) v = 0;
        else {
            while (next_end < chain_ends.size() && chain_ends[next_end] < cluster) ++next_end;
            bool is_end = next_end < chain_ends.size() && chain_ends[next_end] == cluster;
            v = is_end ? kFatEnd : static_cast<std::uint32_t>(cluster + 1);
        }
        put_u32(value, v);
        ALEMBIC_TRY(out.write(Bytes(value, 4)));
    }
    return {};
}

Status ImageBuilder::Impl::emit_bitmap(BufferedSink& out) const {
    std::vector<std::uint8_t> bitmap(static_cast<std::size_t>(bitmap_clusters) * cluster_size, 0);
    std::size_t full = cluster_count / 8;
    std::memset(bitmap.data(), 0xFF, full);
    if (cluster_count % 8 != 0) bitmap[full] = static_cast<std::uint8_t>((1u << (cluster_count % 8)) - 1);
    return out.write(bitmap);
}

std::vector<std::uint8_t> ImageBuilder::Impl::file_entry_set(const Node& child) const {
    std::uint32_t name_entries = name_entry_count(child);
    std::uint32_t secondary = 1 + name_entries;
    std::vector<std::uint8_t> result((secondary + 1) * 32, 0);
    std::uint8_t* file = result.data();
    file[0] = 0x85;
    file[1] = static_cast<std::uint8_t>(secondary);
    put_u16(file + 0x04, child.is_directory ? 0x10 : 0x20);
    for (int offset = 0x08; offset <= 0x10; offset += 4) put_u32(file + offset, kFixedTimestamp);

    std::uint8_t* stream = result.data() + 32;
    stream[0] = 0xC0;
    bool allocated = child.first_cluster >= kFirstCluster;
    stream[1] = allocated ? 0x01 : 0x00;
    stream[3] = static_cast<std::uint8_t>(child.name16.size());
    put_u16(stream + 0x04, name_hash(child.name16, upcase_map));
    std::uint64_t data_length =
        child.is_directory ? static_cast<std::uint64_t>(child.cluster_count) * cluster_size : child.size;
    put_u64(stream + 0x08, data_length);
    put_u32(stream + 0x14, allocated ? child.first_cluster : 0);
    put_u64(stream + 0x18, data_length);

    for (std::uint32_t i = 0; i < name_entries; ++i) {
        std::uint8_t* entry = result.data() + (2 + i) * 32;
        entry[0] = 0xC1;
        std::size_t first = static_cast<std::size_t>(i) * 15;
        std::size_t count = std::min<std::size_t>(15, child.name16.size() - first);
        for (std::size_t k = 0; k < count; ++k) put_u16(entry + 2 + k * 2, child.name16[first + k]);
    }
    put_u16(file + 0x02, entry_set_checksum(result));
    return result;
}

Status ImageBuilder::Impl::emit_directory(BufferedSink& out, const Node& directory) const {
    std::uint64_t emitted = 0;
    if (&directory == &root) {
        std::uint8_t entries[96] = {};
        entries[0] = 0x83; // empty volume label
        std::uint8_t* bitmap = entries + 32;
        bitmap[0] = 0x81;
        put_u32(bitmap + 0x14, kFirstCluster);
        put_u64(bitmap + 0x18, div_round_up(cluster_count, 8));
        std::uint8_t* upcase = entries + 64;
        upcase[0] = 0x82;
        put_u32(upcase + 0x04, upcase_checksum);
        put_u32(upcase + 0x14, kFirstCluster + bitmap_clusters);
        put_u64(upcase + 0x18, sizeof kUpcaseTable);
        ALEMBIC_TRY(out.write(Bytes(entries, sizeof entries)));
        emitted += sizeof entries;
    }
    for (const Node& child : directory.children) {
        std::vector<std::uint8_t> set = file_entry_set(child);
        ALEMBIC_TRY(out.write(set));
        emitted += set.size();
    }
    return out.zeros(static_cast<std::uint64_t>(directory.cluster_count) * cluster_size - emitted);
}

Status ImageBuilder::Impl::emit_file(BufferedSink& out, const Node& node) const {
    if (node.is_virtual) {
        ALEMBIC_TRY(out.write(node.virtual_content));
    } else {
        ALEMBIC_ASSIGN(std::shared_ptr<const RandomAccess> input, source->open(*node.source));
        std::vector<std::uint8_t> buffer(kChunkSize);
        for (std::uint64_t done = 0; done < node.size;) {
            std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(kChunkSize, node.size - done));
            Status read = input->read_at(done, MutableBytes(buffer.data(), n));
            if (!read.ok()) {
                if (read.error().code == Errc::invalid_data)
                    return make_error(Errc::io, "the source file shrank during conversion: " + node.relative_path);
                return read;
            }
            ALEMBIC_TRY(out.write(Bytes(buffer.data(), n)));
            done += n;
        }
        ALEMBIC_ASSIGN(std::uint64_t size_now, input->size());
        if (size_now != node.size)
            return make_error(Errc::io, "the source file changed size during conversion: " + node.relative_path);
    }
    return out.zeros(static_cast<std::uint64_t>(node.cluster_count) * cluster_size - node.size);
}

Status ImageBuilder::Impl::emit(BufferedSink& out) const {
    std::vector<std::uint8_t> boot = boot_region();
    ALEMBIC_TRY(out.write(boot));
    ALEMBIC_TRY(out.write(boot));
    ALEMBIC_TRY(out.zeros((kFatOffsetSectors - 24) * kSectorSize));
    ALEMBIC_TRY(emit_fat(out));
    ALEMBIC_TRY(out.zeros(static_cast<std::uint64_t>(heap_offset_sectors - (kFatOffsetSectors + fat_length_sectors)) *
                          kSectorSize));
    ALEMBIC_TRY(emit_bitmap(out));
    ALEMBIC_TRY(out.write(Bytes(kUpcaseTable, sizeof kUpcaseTable)));
    ALEMBIC_TRY(out.zeros(static_cast<std::uint64_t>(upcase_clusters) * cluster_size - sizeof kUpcaseTable));
    for (const Node* node : nodes) {
        if (node->is_directory) ALEMBIC_TRY(emit_directory(out, *node));
        else if (node->size > 0) ALEMBIC_TRY(emit_file(out, *node));
    }
    return out.flush();
}

ImageBuilder::ImageBuilder(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
ImageBuilder::ImageBuilder(ImageBuilder&&) noexcept = default;
ImageBuilder& ImageBuilder::operator=(ImageBuilder&&) noexcept = default;
ImageBuilder::~ImageBuilder() = default;

std::uint64_t ImageBuilder::image_length() const { return impl_->image_length; }
std::uint32_t ImageBuilder::cluster_size() const { return impl_->cluster_size; }
std::uint32_t ImageBuilder::file_count() const { return impl_->files; }
std::uint32_t ImageBuilder::directory_count() const { return impl_->directories; }

Result<ImageBuilder> ImageBuilder::from_source(const tree::Source& source, const tree::Node& root,
                                               const BuildOptions& options) {
    if (options.cluster_size) {
        std::uint32_t size = *options.cluster_size;
        if (size < 0x1000 || size > 0x2000000 || (size & (size - 1)) != 0)
            return make_error(Errc::invalid_argument, "the exFAT cluster size must be a power of two from 4 KiB to 32 MiB");
    }
    if (!root.is_directory) return make_error(Errc::invalid_argument, "the exFAT root must be a folder");

    auto impl = std::make_unique<Impl>();
    impl->source = &source;
    impl->root.is_directory = true;
    ALEMBIC_TRY(convert_tree(root, impl->root));
    if (options.generate_ampr_index) ALEMBIC_TRY(add_virtual_ampr_index(impl->root, options.ampr_scheme));
    walk(impl->root, impl->nodes);

    std::uint64_t total = 0;
    for (const Node* node : impl->nodes) {
        if (node->is_directory) {
            ++impl->directories;
        } else {
            ++impl->files;
            total += node->size;
        }
    }
    std::uint64_t average = impl->files == 0 ? 0 : total / impl->files;
    std::uint32_t automatic = options.ps5pkgtool_cluster_rule && average < 1024 * 1024 ? 0x8000 : 0x10000;
    impl->cluster_size = options.cluster_size.value_or(automatic);
    ALEMBIC_ASSIGN(impl->upcase_map, build_upcase_map());
    impl->upcase_checksum = table_checksum(kUpcaseTable, sizeof kUpcaseTable);
    ALEMBIC_TRY(impl->plan());
    return ImageBuilder(std::move(impl));
}

Result<ImageBuilder> ImageBuilder::scan(const std::string& source_directory, const BuildOptions& options) {
    ALEMBIC_ASSIGN(std::unique_ptr<tree::Source> folder, tree::open_folder(source_directory));
    const tree::Source& source = *folder;
    ALEMBIC_ASSIGN(ImageBuilder builder, from_source(source, source.root(), options));
    builder.impl_->owned_source = std::move(folder);
    return builder;
}

Status ImageBuilder::write(ByteSink& sink, const ProgressFn& progress) const {
    BufferedSink out(sink, impl_->image_length, progress);
    ALEMBIC_TRY(impl_->emit(out));
    if (out.written() != impl_->image_length)
        return make_error(Errc::invalid_data, "the exFAT writer did not produce its planned length");
    return {};
}

Result<std::vector<std::uint8_t>> build_ampr_index(const tree::Source&, const tree::Node& root, ampr::Scheme scheme) {
    Node converted;
    converted.is_directory = true;
    ALEMBIC_TRY(convert_tree(root, converted));
    std::erase_if(converted.children, is_stale_index);
    return index_for_tree(converted, scheme);
}

Result<std::vector<std::uint8_t>> build_ampr_index(const std::string& source_directory, ampr::Scheme scheme) {
    ALEMBIC_ASSIGN(std::unique_ptr<tree::Source> folder, tree::open_folder(source_directory));
    return build_ampr_index(*folder, folder->root(), scheme);
}

Status write_image(const tree::Source& source, const tree::Node& root, const std::string& output_path,
                   const BuildOptions& options, bool overwrite, const ProgressFn& progress) {
    ALEMBIC_ASSIGN(ImageBuilder builder, ImageBuilder::from_source(source, root, options));
    ALEMBIC_ASSIGN(AtomicOutput output, AtomicOutput::create(output_path, overwrite));
    FileSink sink(output.file(), 0);
    ALEMBIC_TRY(builder.write(sink, progress));
    return output.commit();
}

Status write_directory_image(const std::string& source_directory, const std::string& output_path,
                             const BuildOptions& options, const ProgressFn& progress) {
    ALEMBIC_ASSIGN(ImageBuilder builder, ImageBuilder::scan(source_directory, options));
    ALEMBIC_ASSIGN(AtomicOutput output, AtomicOutput::create(output_path));
    FileSink sink(output.file(), 0);
    ALEMBIC_TRY(builder.write(sink, progress));
    return output.commit();
}

} // namespace alembic::exfat
