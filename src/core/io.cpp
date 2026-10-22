// SPDX-License-Identifier: GPL-3.0-or-later
#include "alembic/io.hpp"

#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <random>

#ifndef _WIN32
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/vfs.h> // statfs
#else
#include <sys/mount.h> // statfs (macOS, FreeBSD, the PS5)
#endif
#endif

namespace alembic {

std::string os_error_message(int error_number) {
    return std::strerror(error_number);
}

// ------------------------------------------------------------------ shared

Status Slice::read_at(std::uint64_t offset, MutableBytes out) const {
    if (offset > length_ || out.size() > length_ - offset)
        return make_error(Errc::invalid_data, "read past the end of a slice");
    return base_->read_at(offset_ + offset, out);
}

Status FileSink::write(Bytes data) {
    ALEMBIC_TRY(file_.write_at(position_, data));
    position_ += data.size();
    return {};
}

bool path_exists(const std::string& path) {
    return stat_path(path, false).kind != PathKind::missing;
}

bool is_directory(const std::string& path) {
    return stat_path(path).is_directory();
}

bool is_regular_file(const std::string& path) {
    return stat_path(path).is_file();
}

Status make_parent_directories(const std::string& path) {
    std::size_t first = 1;
#ifdef _WIN32
    // Never create a drive ("C:") or a share ("//server/share").
    if (path.size() >= 2 && path[1] == ':') first = 3;
    else if (path.rfind("//", 0) == 0) {
        std::size_t server_end = path.find('/', 2);
        first = server_end == std::string::npos ? path.size() : path.find('/', server_end + 1);
        if (first == std::string::npos) return {};
        ++first;
    }
#endif
    for (std::size_t slash = path.find('/', first); slash != std::string::npos; slash = path.find('/', slash + 1)) {
        std::string dir = path.substr(0, slash);
        if (dir.empty() || is_directory(dir)) continue;
        Status made = make_directory(dir);
        if (!made.ok() && !is_directory(dir)) return made; // another thread may have made it
    }
    return {};
}

bool safe_file_name(std::string_view name) {
    if (name.empty() || name == "." || name == "..") return false;
    for (char c : name) {
        if (c == '/' || c == '\0') return false;
#ifdef _WIN32
        if (static_cast<unsigned char>(c) < 0x20 || std::strchr("\\:*?\"<>|", c)) return false;
#endif
    }
    return true;
}

std::string base_name(const std::string& path) {
    std::size_t end = path.find_last_not_of('/');
    if (end == std::string::npos) return path.empty() ? "" : "/";
    std::size_t start = path.rfind('/', end);
    return path.substr(start == std::string::npos ? 0 : start + 1, end - (start == std::string::npos ? 0 : start + 1) + 1);
}

std::string without_trailing_slashes(std::string path) {
    while (path.size() > 1 && path.back() == '/' && !(path.size() == 3 && path[1] == ':')) path.pop_back();
    return path;
}

std::string join_path(const std::string& dir, const std::string& name) {
    if (dir.empty()) return name;
    return dir.back() == '/' ? dir + name : dir + "/" + name;
}

Status touch_file(const std::string& path) {
    auto now = static_cast<std::int64_t>(std::time(nullptr));
    return set_file_times(path, now, now);
}

AtomicOutput::~AtomicOutput() {
    if (keep_ && !committed_) {
        (void)file_.close();
        return;
    }
    if (!committed_ && !temp_path_.empty()) {
        (void)file_.close();
        (void)remove_file(temp_path_);
    }
}

AtomicOutput::AtomicOutput(AtomicOutput&& other) noexcept
    : file_(std::move(other.file_)),
      final_path_(std::move(other.final_path_)),
      temp_path_(std::move(other.temp_path_)),
      committed_(other.committed_),
      overwrite_(other.overwrite_),
      keep_(other.keep_) {
    other.temp_path_.clear();
    other.committed_ = true;
}

Result<AtomicOutput> AtomicOutput::create(const std::string& path, bool overwrite) {
    if (!overwrite && path_exists(path))
        return make_error(Errc::invalid_argument, "the output file already exists: " + path);
    if (overwrite && is_directory(path)) return make_error(Errc::invalid_argument, "the output is a folder: " + path);
    ALEMBIC_TRY(make_parent_directories(path));
    std::random_device device;
    static const char hex[] = "0123456789abcdef";
    std::string suffix;
    for (int i = 0; i < 16; ++i) suffix += hex[device() & 15];
    AtomicOutput output;
    output.overwrite_ = overwrite;
    output.final_path_ = path;
    output.temp_path_ = path + "." + suffix + ".tmp";
    ALEMBIC_ASSIGN(output.file_, File::open(output.temp_path_, File::Mode::create_new));
    return output;
}

Result<AtomicOutput> AtomicOutput::create_partial(const std::string& path, const std::string& partial_path, bool reuse,
                                                  bool overwrite) {
    if (!overwrite && path_exists(path))
        return make_error(Errc::invalid_argument, "the output file already exists: " + path);
    if (overwrite && is_directory(path)) return make_error(Errc::invalid_argument, "the output is a folder: " + path);
    if (partial_path.empty() || partial_path == path) return make_error(Errc::invalid_argument, "bad partial file path");
    ALEMBIC_TRY(make_parent_directories(partial_path));
    AtomicOutput output;
    output.overwrite_ = overwrite;
    output.final_path_ = path;
    output.temp_path_ = partial_path;
    output.keep_ = true;
    if (reuse) {
        if (!is_regular_file(partial_path)) return make_error(Errc::invalid_argument, "the partial file is gone: " + partial_path);
        ALEMBIC_ASSIGN(output.file_, File::open(partial_path, File::Mode::read_write));
    } else {
        (void)remove_file(partial_path);
        ALEMBIC_ASSIGN(output.file_, File::open(partial_path, File::Mode::create_new));
    }
    return output;
}

Status AtomicOutput::commit() {
    ALEMBIC_TRY(file_.sync());
    ALEMBIC_TRY(file_.close());
    if (!overwrite_ && path_exists(final_path_)) return make_error(Errc::invalid_argument, "the output file already exists: " + final_path_);
    ALEMBIC_TRY(rename_path(temp_path_, final_path_));
    committed_ = true;
    return {};
}

#ifndef _WIN32
// ------------------------------------------------------------------ POSIX

std::string last_os_error() { return os_error_message(errno); }

static Error io_error(const std::string& what, const std::string& path) {
    return make_error(Errc::io, what + " '" + path + "': " + os_error_message(errno));
}

File::~File() {
    if (fd_ >= 0) ::close(static_cast<int>(fd_));
}

File::File(File&& other) noexcept : fd_(other.fd_), path_(std::move(other.path_)) {
    other.fd_ = -1;
}

File& File::operator=(File&& other) noexcept {
    if (this != &other) {
        if (fd_ >= 0) ::close(static_cast<int>(fd_));
        fd_ = other.fd_;
        path_ = std::move(other.path_);
        other.fd_ = -1;
    }
    return *this;
}

Result<File> File::open(const std::string& path, Mode mode) {
    int flags = 0;
    switch (mode) {
    case Mode::read: flags = O_RDONLY; break;
    case Mode::create_new: flags = O_RDWR | O_CREAT | O_EXCL; break;
    case Mode::read_write: flags = O_RDWR; break;
    }
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    int fd = ::open(path.c_str(), flags, 0644);
    if (fd < 0) return io_error("cannot open", path);
    File file;
    file.fd_ = fd;
    file.path_ = path;
    return file;
}

Result<std::uint64_t> File::size() const {
    struct stat st {};
    if (::fstat(static_cast<int>(fd_), &st) != 0) return io_error("cannot stat", path_);
    return static_cast<std::uint64_t>(st.st_size);
}

Status File::read_at(std::uint64_t offset, MutableBytes out) const {
    std::size_t done = 0;
    while (done < out.size()) {
        ssize_t n = ::pread(static_cast<int>(fd_), out.data() + done, out.size() - done, static_cast<off_t>(offset + done));
        if (n < 0) {
            if (errno == EINTR) continue;
            return io_error("cannot read", path_);
        }
        if (n == 0) return make_error(Errc::invalid_data, "unexpected end of file in '" + path_ + "'");
        done += static_cast<std::size_t>(n);
    }
    return {};
}

Status File::write_at(std::uint64_t offset, Bytes data) {
    std::size_t done = 0;
    while (done < data.size()) {
        ssize_t n = ::pwrite(static_cast<int>(fd_), data.data() + done, data.size() - done, static_cast<off_t>(offset + done));
        if (n < 0) {
            if (errno == EINTR) continue;
            return io_error("cannot write", path_);
        }
        done += static_cast<std::size_t>(n);
    }
    return {};
}

Status File::truncate(std::uint64_t length) {
    if (::ftruncate(static_cast<int>(fd_), static_cast<off_t>(length)) != 0) return io_error("cannot resize", path_);
    return {};
}

Status File::sync() {
    if (::fsync(static_cast<int>(fd_)) != 0) return io_error("cannot flush", path_);
    return {};
}

Status File::close() {
    if (fd_ < 0) return {};
    int fd = static_cast<int>(fd_);
    fd_ = -1;
    if (::close(fd) != 0) return io_error("cannot close", path_);
    return {};
}

PathStat stat_path(const std::string& path, bool follow_links, std::string* error) {
    struct stat st {};
    PathStat out;
    if ((follow_links ? ::stat(path.c_str(), &st) : ::lstat(path.c_str(), &st)) != 0) {
        if (error) *error = os_error_message(errno);
        return out;
    }
    out.kind = S_ISREG(st.st_mode)   ? PathKind::file
               : S_ISDIR(st.st_mode) ? PathKind::directory
               : S_ISLNK(st.st_mode) ? PathKind::link
                                     : PathKind::other;
    out.size = static_cast<std::uint64_t>(st.st_size);
    out.atime = static_cast<std::int64_t>(st.st_atime);
    out.mtime = static_cast<std::int64_t>(st.st_mtime);
    return out;
}

Result<std::vector<std::string>> list_directory(const std::string& path) {
    DIR* dir = ::opendir(path.c_str());
    if (!dir) return io_error("cannot open folder", path);
    std::vector<std::string> names;
    for (;;) {
        errno = 0;
        dirent* entry = ::readdir(dir);
        if (!entry) {
            int e = errno;
            ::closedir(dir);
            if (e != 0) return make_error(Errc::io, "cannot read folder '" + path + "': " + os_error_message(e));
            return names;
        }
        std::string name = entry->d_name;
        if (name != "." && name != "..") names.push_back(std::move(name));
    }
}

Status make_directory(const std::string& path) {
    if (::mkdir(path.c_str(), 0755) != 0) return io_error("cannot create folder", path);
    return {};
}

Status remove_file(const std::string& path) {
    if (::unlink(path.c_str()) != 0) return io_error("cannot delete", path);
    return {};
}

Status remove_directory(const std::string& path) {
    if (::rmdir(path.c_str()) != 0) return io_error("cannot delete", path);
    return {};
}

Status rename_path(const std::string& from, const std::string& to) {
    if (::rename(from.c_str(), to.c_str()) == 0) return {};
    Error e = io_error("cannot rename", from);
    if (errno == EXDEV) e.code = Errc::unsupported;
    e.message += " (to '" + to + "')";
    return e;
}

Status set_file_times(const std::string& path, std::int64_t atime, std::int64_t mtime) {
    timeval times[2] = {};
    times[0].tv_sec = static_cast<time_t>(atime);
    times[1].tv_sec = static_cast<time_t>(mtime);
    if (::utimes(path.c_str(), times) != 0) return io_error("cannot set the times of", path);
    return {};
}

void flush_file_system() { ::sync(); }

Result<DiskSpace> disk_space(const std::string& path) {
    struct statfs st {};
    if (::statfs(path.c_str(), &st) != 0) return io_error("cannot read the free space of", path);
    DiskSpace s;
    s.total = static_cast<std::uint64_t>(st.f_blocks) * static_cast<std::uint64_t>(st.f_bsize);
    s.free = static_cast<std::uint64_t>(st.f_bavail) * static_cast<std::uint64_t>(st.f_bsize);
    return s;
}

int process_id() { return static_cast<int>(::getpid()); }

bool is_absolute_path(const std::string& path) { return !path.empty() && path[0] == '/'; }

std::string portable_path(std::string path) { return path; }

std::string environment(const char* name) {
    const char* v = std::getenv(name);
    return v ? v : "";
}

std::vector<std::string> program_arguments(int argc, char** argv) { return std::vector<std::string>(argv, argv + argc); }

void use_utf8_console() {}

bool stderr_is_terminal() { return ::isatty(STDERR_FILENO); }

std::FILE* open_stdio(const std::string& path, const char* mode) { return std::fopen(path.c_str(), mode); }

Result<std::string> absolute_path(const std::string& path) {
    std::string rest;
    std::string head = path.empty() ? "." : path;
    for (;;) {
        char resolved[PATH_MAX];
        if (::realpath(head.c_str(), resolved)) {
            std::string out = resolved;
            if (!rest.empty()) out += (out == "/" ? "" : "/") + rest;
            return out;
        }
        if (errno != ENOENT) return io_error("cannot resolve", path);
        while (head.size() > 1 && head.back() == '/') head.pop_back();
        std::size_t slash = head.rfind('/');
        std::string leaf = slash == std::string::npos ? head : head.substr(slash + 1);
        rest = rest.empty() ? leaf : leaf + "/" + rest;
        if (slash == std::string::npos) head = ".";
        else head = slash == 0 ? "/" : head.substr(0, slash);
    }
}
#endif // !_WIN32

} // namespace alembic
