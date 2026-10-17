// SPDX-License-Identifier: GPL-3.0-or-later
#include "alembic/tree.hpp"

#include "alembic/unicode.hpp"

#include <algorithm>
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

namespace alembic::tree {

namespace {

constexpr std::string_view kJunkNames[] = {
    ".ds_store", ".spotlight-v100", ".trashes", ".fseventsd", ".temporaryitems",
    ".documentrevisions-v100", ".apdisk", "__macosx", ".volumeicon.icns",
    "thumbs.db", "ehthumbs.db", "desktop.ini", "$recycle.bin", "system volume information",
};

class FolderSource final : public Source {
public:
    const Node& root() const override { return root_; }
    const std::string& label() const override { return label_; }

    Result<std::shared_ptr<const RandomAccess>> open(const Node& file) const override {
        if (file.is_directory || file.id >= paths_.size())
            return make_error(Errc::invalid_argument, "not a file of this folder");
        ALEMBIC_ASSIGN(File handle, File::open(paths_[file.id], File::Mode::read));
        ALEMBIC_ASSIGN(std::uint64_t size, handle.size());
        if (size != file.size)
            return make_error(Errc::io, "the source file changed size during conversion: " + paths_[file.id]);
        return std::shared_ptr<const RandomAccess>(std::make_shared<File>(std::move(handle)));
    }

    Status scan(const std::string& path, Node& parent) {
        DIR* dir = ::opendir(path.c_str());
        if (!dir) return make_error(Errc::io, "cannot open folder '" + path + "': " + os_error_message(errno));
        Status status;
        for (;;) {
            errno = 0;
            dirent* entry = ::readdir(dir);
            if (!entry) {
                if (errno != 0) status = make_error(Errc::io, "cannot read folder '" + path + "'");
                break;
            }
            std::string name = entry->d_name;
            if (name == "." || name == ".." || is_os_junk(name)) continue;
            std::string full = path + "/" + name;
            struct stat st {};
            if (::lstat(full.c_str(), &st) != 0) {
                status = make_error(Errc::io, "cannot stat '" + full + "': " + os_error_message(errno));
                break;
            }
            if (S_ISLNK(st.st_mode)) continue; // PS5PKGTool skips reparse points
            if (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode)) {
                status = make_error(Errc::unsupported, "not a regular file or folder: " + full);
                break;
            }
            if (!unicode::utf8_to_utf16(name).ok()) {
                status = make_error(Errc::invalid_data, "the file name is not valid UTF-8: " + full);
                break;
            }
            Node node;
            node.name = name;
            node.is_directory = S_ISDIR(st.st_mode);
            node.size = node.is_directory ? 0 : static_cast<std::uint64_t>(st.st_size);
            node.mtime = static_cast<std::int64_t>(st.st_mtime);
            node.id = paths_.size();
            paths_.push_back(full);
            parent.children.push_back(std::move(node));
        }
        ::closedir(dir);
        if (!status.ok()) return status;
        for (Node& child : parent.children) {
            if (!child.is_directory) continue;
            std::string child_path = paths_[child.id]; // copy: paths_ grows while recursing
            ALEMBIC_TRY(scan(child_path, child));
        }
        return {};
    }

    Node root_;
    std::string label_;
    std::vector<std::string> paths_;
};

Status write_file(const Source& source, const Node& file, const std::string& path, bool overwrite,
                  std::uint64_t& done, std::uint64_t total, const ProgressFn& progress) {
    if (path_exists(path) && !overwrite) return make_error(Errc::invalid_argument, "the output file already exists: " + path);
    if (path_exists(path) && ::unlink(path.c_str()) != 0)
        return make_error(Errc::io, "cannot replace '" + path + "': " + os_error_message(errno));
    ALEMBIC_ASSIGN(File out, File::open(path, File::Mode::create_new));
    if (file.size > 0) {
        ALEMBIC_ASSIGN(std::shared_ptr<const RandomAccess> in, source.open(file));
        std::vector<std::uint8_t> buffer(1 << 20);
        for (std::uint64_t at = 0; at < file.size;) {
            auto n = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), file.size - at));
            ALEMBIC_TRY(in->read_at(at, MutableBytes(buffer.data(), n)));
            ALEMBIC_TRY(out.write_at(at, Bytes(buffer.data(), n)));
            at += n;
            done += n;
            if (progress && !progress({"Extracting", done, total})) return make_error(Errc::cancelled, "cancelled");
        }
    }
    ALEMBIC_TRY(out.close());
    timeval times[2] = {{static_cast<time_t>(file.mtime), 0}, {static_cast<time_t>(file.mtime), 0}};
    ::utimes(path.c_str(), times); // best effort
    return {};
}

Status extract_into(const Source& source, const Node& directory, const std::string& folder, bool overwrite,
                    std::uint64_t& done, std::uint64_t total, const ProgressFn& progress) {
    if (!is_directory(folder) && ::mkdir(folder.c_str(), 0755) != 0)
        return make_error(Errc::io, "cannot create folder '" + folder + "': " + os_error_message(errno));
    for (const Node& child : directory.children) {
        // Names come from untrusted images: never let one leave the folder.
        if (child.name.empty() || child.name == "." || child.name == ".." ||
            child.name.find('/') != std::string::npos || child.name.find('\0') != std::string::npos)
            return make_error(Errc::invalid_data, "unsafe name in the image: " + child.name);
        std::string path = folder + "/" + child.name;
        if (child.is_directory) ALEMBIC_TRY(extract_into(source, child, path, overwrite, done, total, progress));
        else ALEMBIC_TRY(write_file(source, child, path, overwrite, done, total, progress));
    }
    return {};
}

} // namespace

bool is_os_junk(std::string_view name) {
    if (name.size() >= 2 && name[0] == '.' && name[1] == '_') return true;
    for (std::string_view junk : kJunkNames)
        if (unicode::ascii_iequals(name, junk)) return true;
    return false;
}

Result<std::unique_ptr<Source>> open_folder(const std::string& path) {
    if (!is_directory(path)) return make_error(Errc::invalid_argument, "not a folder: " + path);
    auto source = std::make_unique<FolderSource>();
    std::string root = path;
    while (root.size() > 1 && root.back() == '/') root.pop_back();
    source->root_.is_directory = true;
    source->label_ = base_name(root);
    ALEMBIC_TRY(source->scan(root, source->root_));
    return std::unique_ptr<Source>(std::move(source));
}

const Node* find(const Node& root, std::string_view path) {
    const Node* node = &root;
    while (!path.empty()) {
        std::size_t slash = path.find('/');
        std::string_view part = path.substr(0, slash);
        path = slash == std::string_view::npos ? std::string_view() : path.substr(slash + 1);
        if (part.empty()) continue;
        const Node* next = nullptr;
        for (const Node& child : node->children)
            if (child.name == part) next = &child;
        if (!next) return nullptr;
        node = next;
    }
    return node;
}

const Node* find_child_icase(const Node& directory, std::string_view name) {
    for (const Node& child : directory.children)
        if (unicode::ascii_iequals(child.name, name)) return &child;
    return nullptr;
}

Totals totals(const Node& node) {
    Totals t;
    for (const Node& child : node.children) {
        if (child.is_directory) {
            Totals sub = totals(child);
            t.files += sub.files;
            t.directories += sub.directories + 1;
            t.bytes += sub.bytes;
        } else {
            ++t.files;
            t.bytes += child.size;
        }
    }
    return t;
}

Status extract(const Source& source, const Node& directory, const std::string& output_folder, bool overwrite,
               const ProgressFn& progress) {
    ALEMBIC_TRY(make_parent_directories(output_folder));
    std::uint64_t done = 0;
    return extract_into(source, directory, output_folder, overwrite, done, totals(directory).bytes, progress);
}

} // namespace alembic::tree
