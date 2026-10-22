// SPDX-License-Identifier: GPL-3.0-or-later
// Mounted file systems (io.hpp): an in-memory one under "/mem/" stands in
// for another engine's files. Conversions into it must give the same bytes
// as conversions to disk, which shows every writer goes through io.hpp.
#include "alembic/convert.hpp"
#include "alembic/io.hpp"
#include "test.hpp"

#include <algorithm>
#include <map>
#include <mutex>
#include <set>

using namespace alembic;

namespace {

class MemoryFs final : public FileSystem {
public:
    struct Data {
        std::vector<std::uint8_t> bytes;
        std::int64_t mtime = 0;
    };
    std::mutex mutex;
    std::map<std::string, std::shared_ptr<Data>> files;
    std::set<std::string> dirs{"/mem"};
    int requests = 0;

    class Handle final : public FileHandle {
    public:
        Handle(MemoryFs& fs, std::shared_ptr<Data> d) : fs_(fs), d_(std::move(d)) {}
        Result<std::uint64_t> size() const override {
            std::lock_guard<std::mutex> lock(fs_.mutex);
            return static_cast<std::uint64_t>(d_->bytes.size());
        }
        Status read_at(std::uint64_t offset, MutableBytes out) const override {
            std::lock_guard<std::mutex> lock(fs_.mutex);
            if (offset + out.size() > d_->bytes.size()) return make_error(Errc::invalid_data, "unexpected end of file");
            std::copy_n(d_->bytes.begin() + static_cast<std::ptrdiff_t>(offset), out.size(), out.begin());
            return {};
        }
        Status write_at(std::uint64_t offset, Bytes data) override {
            std::lock_guard<std::mutex> lock(fs_.mutex);
            ++fs_.requests;
            if (offset + data.size() > d_->bytes.size()) d_->bytes.resize(static_cast<std::size_t>(offset + data.size()));
            std::copy(data.begin(), data.end(), d_->bytes.begin() + static_cast<std::ptrdiff_t>(offset));
            return {};
        }
        Status truncate(std::uint64_t length) override {
            std::lock_guard<std::mutex> lock(fs_.mutex);
            d_->bytes.resize(static_cast<std::size_t>(length));
            return {};
        }
        Status sync() override { return {}; }
        Status close() override { return {}; }

    private:
        MemoryFs& fs_;
        std::shared_ptr<Data> d_;
    };

    static std::string clean(std::string p) {
        while (p.size() > 1 && p.back() == '/') p.pop_back();
        return p;
    }
    static std::string parent(const std::string& p) { return p.substr(0, p.rfind('/')); }

    Result<std::unique_ptr<FileHandle>> open(const std::string& path, File::Mode mode) override {
        std::lock_guard<std::mutex> lock(mutex);
        std::string p = clean(path);
        auto it = files.find(p);
        if (mode == File::Mode::create_new) {
            if (it != files.end() || dirs.count(p)) return make_error(Errc::io, "exists: " + p);
            if (!dirs.count(parent(p))) return make_error(Errc::io, "no folder for " + p);
            it = files.emplace(p, std::make_shared<Data>()).first;
        } else if (it == files.end()) {
            return make_error(Errc::io, "no such file: " + p);
        }
        return std::unique_ptr<FileHandle>(new Handle(*this, it->second));
    }
    PathStat stat(const std::string& path, bool, std::string* error) override {
        std::lock_guard<std::mutex> lock(mutex);
        std::string p = clean(path);
        PathStat st;
        if (dirs.count(p)) st.kind = PathKind::directory;
        else if (auto it = files.find(p); it != files.end()) {
            st.kind = PathKind::file;
            st.size = it->second->bytes.size();
            st.mtime = it->second->mtime;
        } else if (error) *error = "not there";
        return st;
    }
    Result<std::vector<DirEntry>> list(const std::string& path, bool follow) override {
        std::string p = clean(path);
        std::vector<std::string> names;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!dirs.count(p)) return make_error(Errc::io, "no such folder: " + p);
            for (const auto& d : dirs)
                if (d != p && parent(d) == p) names.push_back(d.substr(p.size() + 1));
            for (const auto& [f, data] : files)
                if (parent(f) == p) names.push_back(f.substr(p.size() + 1));
        }
        std::vector<DirEntry> out;
        for (const std::string& n : names) out.push_back({n, stat(p + "/" + n, follow, nullptr)});
        return out;
    }
    Status make_directory(const std::string& path) override {
        std::lock_guard<std::mutex> lock(mutex);
        std::string p = clean(path);
        if (dirs.count(p) || files.count(p)) return make_error(Errc::io, "exists: " + p);
        if (!dirs.count(parent(p))) return make_error(Errc::io, "no folder for " + p);
        dirs.insert(p);
        return {};
    }
    Status remove_file(const std::string& path) override {
        std::lock_guard<std::mutex> lock(mutex);
        return files.erase(clean(path)) ? Status{} : make_error(Errc::io, "no such file");
    }
    Status remove_directory(const std::string& path) override {
        std::lock_guard<std::mutex> lock(mutex);
        std::string p = clean(path);
        for (const auto& [f, d] : files)
            if (parent(f) == p) return make_error(Errc::io, "not empty");
        return dirs.erase(p) ? Status{} : make_error(Errc::io, "no such folder");
    }
    Status rename(const std::string& from, const std::string& to) override {
        std::lock_guard<std::mutex> lock(mutex);
        std::string a = clean(from), b = clean(to);
        auto it = files.find(a);
        if (it == files.end()) return make_error(Errc::unsupported, "only files are renamed here");
        files[b] = it->second; // replaces a file, as rename(2) does
        files.erase(a);
        return {};
    }
    Status set_times(const std::string& path, std::int64_t, std::int64_t mtime) override {
        std::lock_guard<std::mutex> lock(mutex);
        if (auto it = files.find(clean(path)); it != files.end()) it->second->mtime = mtime;
        return {};
    }
    Result<DiskSpace> disk_space(const std::string&) override { return DiskSpace{1ull << 40, 1ull << 40}; }

    std::vector<std::uint8_t> bytes(const std::string& path) {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = files.find(path);
        return it == files.end() ? std::vector<std::uint8_t>{} : it->second->bytes;
    }
};

std::vector<std::uint8_t> pattern(std::size_t size, std::uint8_t seed) {
    std::vector<std::uint8_t> data(size);
    for (std::size_t i = 0; i < size; ++i) data[i] = static_cast<std::uint8_t>(seed + i * 7 + (i >> 11));
    return data;
}

} // namespace

TEST_CASE(mounted_outputs_match_disk) {
    auto mem = std::make_shared<MemoryFs>();
    mount("/mem/", mem);
    std::string tmp = test::make_temp_dir();
    std::string game = tmp + "/game";
    for (const char* d : {"", "/sce_sys", "/Data", "/Data/Empty"}) test::make_dir(game + d);
    std::string param = R"({"titleId":"PPSA01234"})";
    test::write_file(game + "/sce_sys/param.json", std::vector<std::uint8_t>(param.begin(), param.end()));
    test::write_file(game + "/eboot.bin", pattern(300000, 1));
    test::write_file(game + "/Data/big.bin", pattern(3 * 1024 * 1024 + 17, 2));
    test::write_file(game + "/Data/empty.bin", {});

    convert::Options o;
    o.ffpfsc.build_timestamp = 1;
    o.ufs2.timestamp = 1;
    o.ufs2.process_id = 1;
    CHECK(is_directory("/mem") && is_absolute_path("/mem/x"));
    // Every image format: to disk and into the mount, the same bytes.
    for (auto [format, name] : {std::pair{convert::Format::exfat, "g.exfat"}, std::pair{convert::Format::ffpfsc, "g.ffpfsc"},
                                std::pair{convert::Format::ffpkg, "g.ffpkg"}, std::pair{convert::Format::ffpfs, "g.ffpfs"}}) {
        REQUIRE_OK(convert::run(game, tmp + "/" + name, format, o));
        REQUIRE_OK(convert::run(game, std::string("/mem/out/") + name, format, o)); // makes /mem/out
        CHECK(!test::read_file(tmp + "/" + name).empty());
        CHECK(mem->bytes(std::string("/mem/out/") + name) == test::read_file(tmp + "/" + name));
    }
    CHECK(!is_regular_file("/mem/out/g.exfat.partial")); // committed, nothing left behind
    // And back out of the mount: an image there as the input, a folder in it as the output.
    REQUIRE_OK(convert::run("/mem/out/g.ffpfsc", "/mem/back", convert::Format::folder, o));
    CHECK(mem->bytes("/mem/back/Data/big.bin") == test::read_file(game + "/Data/big.bin"));
    CHECK(is_directory("/mem/back/Data/Empty"));
    // A folder in the mount as the input.
    REQUIRE_OK(convert::run("/mem/back", tmp + "/from_mem.exfat", convert::Format::exfat, o));
    CHECK(test::read_file(tmp + "/from_mem.exfat") == test::read_file(tmp + "/g.exfat"));
    auto entries = list_directory_entries("/mem/back/Data");
    REQUIRE_OK(entries);
    CHECK_EQ(entries->size(), std::size_t(3));
    CHECK(!rename_path("/mem/out/g.exfat", tmp + "/moved.exfat").ok()); // across machines: copy instead
    unmount("/mem/");
    CHECK(!is_mounted("/mem/x"));
}
