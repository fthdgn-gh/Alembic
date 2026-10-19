// SPDX-License-Identifier: GPL-3.0-or-later
#include "fs_ops.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#if defined(__linux__)
#include <sys/vfs.h> // statfs
#else
#include <sys/mount.h> // statfs (macOS, FreeBSD, PS5)
#endif
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <vector>

namespace alembic::engine {

namespace {

Error os_error(const std::string& what, const std::string& path) {
    return Error{Errc::io, what + " " + path + ": " + std::strerror(errno)};
}

std::vector<std::string> children(const std::string& dir) {
    std::vector<std::string> out;
    if (DIR* d = ::opendir(dir.c_str())) {
        while (dirent* e = ::readdir(d)) {
            std::string n = e->d_name;
            if (n != "." && n != "..") out.push_back(n);
        }
        ::closedir(d);
    }
    return out;
}

bool plain_name(const std::string& name) {
    return !name.empty() && name != "." && name != ".." && name.find('/') == std::string::npos &&
           name.find('\0') == std::string::npos;
}

void keep_times(const std::string& path, const struct stat& st) {
    timeval times[2];
    times[0].tv_sec = st.st_atime;
    times[0].tv_usec = 0;
    times[1].tv_sec = st.st_mtime;
    times[1].tv_usec = 0;
    ::utimes(path.c_str(), times);
}

struct Copier {
    explicit Copier(const ProgressFn& p) : progress(p) {}
    const ProgressFn& progress;
    std::uint64_t total = 0;
    std::uint64_t done = 0;
    std::vector<std::string> created; // to undo on failure, in creation order
    std::vector<std::uint8_t> buffer = std::vector<std::uint8_t>(8 << 20);

    Status file(const std::string& from, const std::string& to, const struct stat& st, bool overwrite) {
        if (!overwrite && path_exists(to)) return make_error(Errc::invalid_argument, "already exists: " + to);
        std::string partial = to + ".alembic-partial";
        int in = ::open(from.c_str(), O_RDONLY);
        if (in < 0) return os_error("cannot open", from);
        int out = ::open(partial.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (out < 0) {
            ::close(in);
            return os_error("cannot create", partial);
        }
        created.push_back(partial);
        Status status;
        for (;;) {
            ssize_t n = ::read(in, buffer.data(), buffer.size());
            if (n < 0 && errno == EINTR) continue;
            if (n < 0) {
                status = os_error("cannot read", from);
                break;
            }
            if (n == 0) break;
            for (ssize_t off = 0; off < n;) {
                ssize_t w = ::write(out, buffer.data() + off, static_cast<std::size_t>(n - off));
                if (w < 0 && errno == EINTR) continue;
                if (w <= 0) {
                    status = os_error("cannot write", partial);
                    break;
                }
                off += w;
            }
            if (!status.ok()) break;
            done += static_cast<std::uint64_t>(n);
            if (progress && !progress({"Copying", done, total})) {
                status = make_error(Errc::cancelled, "cancelled");
                break;
            }
        }
        ::close(in);
        if (::close(out) != 0 && status.ok()) status = os_error("cannot write", partial);
        if (!status.ok()) return status;
        if (::rename(partial.c_str(), to.c_str()) != 0) return os_error("cannot rename", partial);
        created.back() = to;
        keep_times(to, st);
        return {};
    }

    Status any(const std::string& from, const std::string& to, bool overwrite) {
        struct stat st{};
        if (::stat(from.c_str(), &st) != 0) return os_error("cannot read", from);
        if (!S_ISDIR(st.st_mode)) return file(from, to, st, overwrite);
        if (path_exists(to)) {
            if (!is_directory(to)) return make_error(Errc::invalid_argument, "a file is in the way: " + to);
            if (!overwrite) return make_error(Errc::invalid_argument, "already exists: " + to);
        } else {
            if (::mkdir(to.c_str(), 0777) != 0) return os_error("cannot create", to);
            created.push_back(to);
        }
        for (const std::string& name : children(from)) ALEMBIC_TRY(any(from + "/" + name, to + "/" + name, overwrite));
        keep_times(to, st);
        return {};
    }

    void undo() {
        for (auto it = created.rbegin(); it != created.rend(); ++it) {
            if (is_directory(*it)) ::rmdir(it->c_str());
            else ::unlink(it->c_str());
        }
    }
};

} // namespace

bool is_within(const std::string& inner, const std::string& outer) {
    if (inner == outer) return true;
    std::string o = outer;
    if (o.empty() || o.back() != '/') o += '/';
    return inner.compare(0, o.size(), o) == 0;
}

Result<SpaceInfo> space(const std::string& path) {
    struct statfs st{};
    if (::statfs(path.c_str(), &st) != 0) return os_error("cannot read the free space of", path);
    SpaceInfo s;
    s.total = static_cast<std::uint64_t>(st.f_blocks) * static_cast<std::uint64_t>(st.f_bsize);
    s.free = static_cast<std::uint64_t>(st.f_bavail) * static_cast<std::uint64_t>(st.f_bsize);
    return s;
}

Status remove_tree(const std::string& path) {
    struct stat st{};
    if (::lstat(path.c_str(), &st) != 0) return os_error("cannot delete", path);
    if (S_ISDIR(st.st_mode)) {
        for (const std::string& name : children(path)) ALEMBIC_TRY(remove_tree(path + "/" + name));
        if (::rmdir(path.c_str()) != 0) return os_error("cannot delete", path);
        return {};
    }
    if (::unlink(path.c_str()) != 0) return os_error("cannot delete", path);
    return {};
}

Status make_folder(const std::string& path) {
    if (path_exists(path)) return make_error(Errc::invalid_argument, "already exists: " + path);
    if (::mkdir(path.c_str(), 0777) != 0) return os_error("cannot create", path);
    return {};
}

Status rename_entry(const std::string& path, const std::string& new_name) {
    if (!plain_name(new_name)) return make_error(Errc::invalid_argument, "a name cannot contain '/'");
    std::size_t slash = path.rfind('/');
    std::string target = path.substr(0, slash + 1) + new_name;
    if (target == path) return {};
    if (path_exists(target)) return make_error(Errc::invalid_argument, "already exists: " + target);
    if (::rename(path.c_str(), target.c_str()) != 0) return os_error("cannot rename", path);
    return {};
}

Result<std::uint64_t> tree_size(const std::string& path) {
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) return os_error("cannot read", path);
    if (!S_ISDIR(st.st_mode)) return static_cast<std::uint64_t>(st.st_size);
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
    if (!path_exists(destination) && ::rename(source.c_str(), destination.c_str()) == 0) return {};
    if (errno != EXDEV && !path_exists(destination)) return os_error("cannot move", source);
    // Another drive (or merging into an existing folder): copy, then delete.
    ALEMBIC_TRY(copy_tree(source, destination, overwrite, progress));
    if (progress) progress({"Deleting the source", 0, 0});
    return remove_tree(source);
}

} // namespace alembic::engine
