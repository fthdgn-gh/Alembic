// SPDX-License-Identifier: GPL-3.0-or-later
#include "alembic/io.hpp"

#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <random>
#include <sys/stat.h>
#include <unistd.h>

namespace alembic {

std::string os_error_message(int error_number) {
    return std::strerror(error_number);
}

static Error io_error(const std::string& what, const std::string& path) {
    return make_error(Errc::io, what + " '" + path + "': " + os_error_message(errno));
}

File::~File() {
    if (fd_ >= 0) ::close(fd_);
}

File::File(File&& other) noexcept : fd_(other.fd_), path_(std::move(other.path_)) {
    other.fd_ = -1;
}

File& File::operator=(File&& other) noexcept {
    if (this != &other) {
        if (fd_ >= 0) ::close(fd_);
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
    if (::fstat(fd_, &st) != 0) return io_error("cannot stat", path_);
    return static_cast<std::uint64_t>(st.st_size);
}

Status File::read_at(std::uint64_t offset, MutableBytes out) const {
    std::size_t done = 0;
    while (done < out.size()) {
        ssize_t n = ::pread(fd_, out.data() + done, out.size() - done, static_cast<off_t>(offset + done));
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
        ssize_t n = ::pwrite(fd_, data.data() + done, data.size() - done, static_cast<off_t>(offset + done));
        if (n < 0) {
            if (errno == EINTR) continue;
            return io_error("cannot write", path_);
        }
        done += static_cast<std::size_t>(n);
    }
    return {};
}

Status File::truncate(std::uint64_t length) {
    if (::ftruncate(fd_, static_cast<off_t>(length)) != 0) return io_error("cannot resize", path_);
    return {};
}

Status File::sync() {
    if (::fsync(fd_) != 0) return io_error("cannot flush", path_);
    return {};
}

Status File::close() {
    if (fd_ < 0) return {};
    int fd = fd_;
    fd_ = -1;
    if (::close(fd) != 0) return io_error("cannot close", path_);
    return {};
}

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
    struct stat st {};
    return ::lstat(path.c_str(), &st) == 0;
}

bool is_directory(const std::string& path) {
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool is_regular_file(const std::string& path) {
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

Status make_parent_directories(const std::string& path) {
    for (std::size_t slash = path.find('/', 1); slash != std::string::npos; slash = path.find('/', slash + 1)) {
        std::string dir = path.substr(0, slash);
        if (dir.empty() || is_directory(dir)) continue;
        if (::mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) return io_error("cannot create folder", dir);
    }
    return {};
}

std::string base_name(const std::string& path) {
    std::size_t end = path.find_last_not_of('/');
    if (end == std::string::npos) return path.empty() ? "" : "/";
    std::size_t start = path.rfind('/', end);
    return path.substr(start == std::string::npos ? 0 : start + 1, end - (start == std::string::npos ? 0 : start + 1) + 1);
}

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

AtomicOutput::~AtomicOutput() {
    if (!committed_ && !temp_path_.empty()) {
        (void)file_.close();
        ::unlink(temp_path_.c_str());
    }
}

AtomicOutput::AtomicOutput(AtomicOutput&& other) noexcept
    : file_(std::move(other.file_)),
      final_path_(std::move(other.final_path_)),
      temp_path_(std::move(other.temp_path_)),
      committed_(other.committed_),
      overwrite_(other.overwrite_) {
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

Status AtomicOutput::commit() {
    ALEMBIC_TRY(file_.sync());
    ALEMBIC_TRY(file_.close());
    if (!overwrite_ && path_exists(final_path_)) return make_error(Errc::invalid_argument, "the output file already exists: " + final_path_);
    if (::rename(temp_path_.c_str(), final_path_.c_str()) != 0) return io_error("cannot rename to", final_path_);
    committed_ = true;
    return {};
}

} // namespace alembic
