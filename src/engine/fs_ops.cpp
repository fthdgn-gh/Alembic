// SPDX-License-Identifier: GPL-3.0-or-later
#include "fs_ops.hpp"

#include <cctype>
#include <cstring>
#include <vector>

namespace alembic::engine {

namespace {

std::vector<std::string> children(const std::string& dir) {
    auto names = list_directory(dir);
    return names.ok() ? std::move(*names) : std::vector<std::string>{};
}

void keep_times(const std::string& path, const PathStat& st) { (void)set_file_times(path, st.atime, st.mtime); }

struct Copier {
    explicit Copier(const ProgressFn& p) : progress(p) {}
    const ProgressFn& progress;
    std::uint64_t total = 0;
    std::uint64_t done = 0;
    std::vector<std::string> created; // to undo on failure, in creation order
    std::vector<std::uint8_t> buffer = std::vector<std::uint8_t>(8 << 20);

    Status file(const std::string& from, const std::string& to, const PathStat& st, bool overwrite) {
        if (!overwrite && path_exists(to)) return make_error(Errc::invalid_argument, "already exists: " + to);
        std::string partial = to + ".alembic-partial";
        ALEMBIC_ASSIGN(File in, File::open(from, File::Mode::read));
        if (path_exists(partial)) ALEMBIC_TRY(remove_file(partial));
        ALEMBIC_ASSIGN(File out, File::open(partial, File::Mode::create_new));
        created.push_back(partial);
        ALEMBIC_ASSIGN(std::uint64_t size, in.size());
        for (std::uint64_t at = 0; at < size;) {
            auto n = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), size - at));
            ALEMBIC_TRY(in.read_at(at, MutableBytes(buffer.data(), n)));
            ALEMBIC_TRY(out.write_at(at, Bytes(buffer.data(), n)));
            at += n;
            done += n;
            if (progress && !progress({"Copying", done, total})) return make_error(Errc::cancelled, "cancelled");
        }
        ALEMBIC_TRY(out.close());
        (void)in.close();
        ALEMBIC_TRY(rename_path(partial, to));
        created.back() = to;
        keep_times(to, st);
        return {};
    }

    Status any(const std::string& from, const std::string& to, bool overwrite) {
        std::string why;
        PathStat st = stat_path(from, true, &why);
        if (st.kind == PathKind::missing) return make_error(Errc::io, "cannot read " + from + ": " + why);
        if (!st.is_directory()) return file(from, to, st, overwrite);
        if (path_exists(to)) {
            if (!is_directory(to)) return make_error(Errc::invalid_argument, "a file is in the way: " + to);
            if (!overwrite) return make_error(Errc::invalid_argument, "already exists: " + to);
        } else {
            ALEMBIC_TRY(make_directory(to));
            created.push_back(to);
        }
        for (const std::string& name : children(from)) ALEMBIC_TRY(any(from + "/" + name, to + "/" + name, overwrite));
        keep_times(to, st);
        return {};
    }

    void undo() {
        for (auto it = created.rbegin(); it != created.rend(); ++it) {
            if (is_directory(*it)) (void)remove_directory(*it);
            else (void)remove_file(*it);
        }
    }
};

} // namespace

namespace {
// Paths compare without case on Windows (ASCII only: drive letters, most names).
bool same_text(const std::string& a, std::size_t from, std::size_t count, const std::string& b) {
#ifdef _WIN32
    if (a.size() - from < count || b.size() != count) return false;
    for (std::size_t i = 0; i < count; ++i)
        if (std::tolower(static_cast<unsigned char>(a[from + i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
#else
    return a.compare(from, count, b) == 0;
#endif
}
} // namespace

bool is_within(const std::string& inner, const std::string& outer) {
    if (inner.size() == outer.size() && same_text(inner, 0, inner.size(), outer)) return true;
    std::string o = outer;
    if (o.empty() || o.back() != '/') o += '/';
    return inner.size() >= o.size() && same_text(inner, 0, o.size(), o);
}

Result<SpaceInfo> space(const std::string& path) {
    ALEMBIC_ASSIGN(DiskSpace d, disk_space(path));
    return SpaceInfo{d.total, d.free};
}

Status remove_tree(const std::string& path) {
    PathStat st = stat_path(path, false);
    if (st.kind == PathKind::missing) return make_error(Errc::io, "cannot delete " + path + ": it is not there");
    if (st.is_directory()) {
        for (const std::string& name : children(path)) ALEMBIC_TRY(remove_tree(path + "/" + name));
        return remove_directory(path);
    }
#ifdef _WIN32
    // A link to a folder is removed like a folder (its target stays).
    if (st.kind == PathKind::link && stat_path(path).is_directory()) return remove_directory(path);
#endif
    return remove_file(path);
}

Status make_folder(const std::string& path) {
    if (path_exists(path)) return make_error(Errc::invalid_argument, "already exists: " + path);
    return make_directory(path);
}

Status rename_entry(const std::string& path, const std::string& new_name) {
    if (!safe_file_name(new_name)) return make_error(Errc::invalid_argument, "not a valid name: " + new_name);
    std::size_t slash = path.rfind('/');
    std::string target = path.substr(0, slash + 1) + new_name;
    if (target == path) return {};
    // Windows: a change of case only is the same file, so let it through.
    if (path_exists(target) && !is_within(target, path)) return make_error(Errc::invalid_argument, "already exists: " + target);
    return rename_path(path, target);
}

Result<std::uint64_t> tree_size(const std::string& path) {
    std::string why;
    PathStat st = stat_path(path, true, &why);
    if (st.kind == PathKind::missing) return make_error(Errc::io, "cannot read " + path + ": " + why);
    if (!st.is_directory()) return st.size;
    std::uint64_t total = 0;
    for (const std::string& name : children(path)) {
        ALEMBIC_ASSIGN(std::uint64_t n, tree_size(path + "/" + name));
        total += n;
    }
    return total;
}

Status copy_tree(const std::string& source, const std::string& destination, bool overwrite, const ProgressFn& progress) {
    if (is_within(destination, source)) return make_error(Errc::invalid_argument, "cannot copy a folder into itself");
    Copier c(progress);
    if (progress && !progress({"Measuring", 0, 0})) return make_error(Errc::cancelled, "cancelled");
    ALEMBIC_ASSIGN(c.total, tree_size(source));
    ALEMBIC_TRY(make_parent_directories(destination));
    Status s = c.any(source, destination, overwrite);
    if (!s.ok()) c.undo();
    return s;
}

Status move_tree(const std::string& source, const std::string& destination, bool overwrite, const ProgressFn& progress) {
    if (is_within(destination, source)) return make_error(Errc::invalid_argument, "cannot move a folder into itself");
    if (path_exists(destination) && !overwrite) return make_error(Errc::invalid_argument, "already exists: " + destination);
    ALEMBIC_TRY(make_parent_directories(destination));
    if (!path_exists(destination)) {
        Status moved = rename_path(source, destination);
        if (moved.ok()) return {};
        if (moved.error().code != Errc::unsupported) return moved; // not "another drive"
    }
    // Another drive (or merging into an existing folder): copy, then delete.
    ALEMBIC_TRY(copy_tree(source, destination, overwrite, progress));
    if (progress) progress({"Deleting the source", 0, 0});
    return remove_tree(source);
}

} // namespace alembic::engine
