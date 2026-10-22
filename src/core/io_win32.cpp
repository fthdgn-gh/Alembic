// SPDX-License-Identifier: GPL-3.0-or-later
// The Windows side of io.hpp. Paths arrive as UTF-8 with '/' (or '\') between
// parts and go to the wide Win32 calls as UTF-16. Absolute paths get the
// "\\?\" prefix, which lifts the 260-character limit and keeps names such as
// "CON" or "a." as they are; that prefix needs '\' and no "." or ".." parts,
// so paths are normalised first.
#ifdef _WIN32

#include "alembic/io.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <io.h>
#include <windows.h>
#include <shellapi.h> // CommandLineToArgvW

namespace alembic {

namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string narrow(const wchar_t* w, std::size_t length) {
    if (length == 0) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, static_cast<int>(length), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, static_cast<int>(length), s.data(), n, nullptr, nullptr);
    return s;
}

std::string narrow(const std::wstring& w) { return narrow(w.data(), w.size()); }

std::string message_for(DWORD code) {
    wchar_t* text = nullptr;
    DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                             nullptr, code, 0, reinterpret_cast<wchar_t*>(&text), 0, nullptr);
    std::string out = n ? narrow(text, n) : "error " + std::to_string(code);
    if (text) LocalFree(text);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ' || out.back() == '.')) out.pop_back();
    return out;
}

Error io_error(const std::string& what, const std::string& path, DWORD code = GetLastError()) {
    Errc errc = code == ERROR_NOT_SAME_DEVICE ? Errc::unsupported : Errc::io;
    return make_error(errc, what + " '" + path + "': " + message_for(code));
}

bool is_sep(wchar_t c) { return c == L'/' || c == L'\\'; }

// UTF-8 path -> what the wide calls take.
std::wstring native(const std::string& path) {
    std::wstring w = widen(path);
    for (wchar_t& c : w)
        if (c == L'/') c = L'\\';
    if (w.rfind(L"\\\\?\\", 0) == 0) return w; // already a long path
    bool drive = w.size() >= 3 && w[1] == L':' && w[2] == L'\\';
    bool unc = w.size() >= 3 && w[0] == L'\\' && w[1] == L'\\' && w[2] != L'\\';
    if (!drive && !unc) {
        // Relative (or "C:x"): make it absolute so it gets the prefix too.
        DWORD n = GetFullPathNameW(w.c_str(), 0, nullptr, nullptr);
        if (n == 0) return w;
        std::wstring full(n, L'\0');
        n = GetFullPathNameW(w.c_str(), n, full.data(), nullptr);
        full.resize(n);
        bool ok = n >= 3 && ((full[1] == L':' && full[2] == L'\\') || (full[0] == L'\\' && full[1] == L'\\'));
        return ok && full != w ? native(narrow(full)) : w;
    }
    // Normalise "." and ".." and repeated separators; "\\?\" takes the path as it is.
    std::size_t root_end = drive ? 3 : 2;
    if (unc) { // keep \\server\share as the root
        std::size_t server = w.find(L'\\', 2);
        std::size_t share = server == std::wstring::npos ? std::wstring::npos : w.find(L'\\', server + 1);
        root_end = share == std::wstring::npos ? w.size() : share + 1;
    }
    std::wstring root = w.substr(0, root_end);
    if (!root.empty() && root.back() != L'\\') root += L'\\';
    std::vector<std::wstring> parts;
    for (std::size_t i = root_end; i <= w.size();) {
        std::size_t j = w.find(L'\\', i);
        if (j == std::wstring::npos) j = w.size();
        std::wstring part = w.substr(i, j - i);
        if (part == L"..") {
            if (!parts.empty()) parts.pop_back();
        } else if (!part.empty() && part != L".") {
            parts.push_back(part);
        }
        i = j + 1;
    }
    std::wstring out = unc ? L"\\\\?\\UNC\\" + root.substr(2) : L"\\\\?\\" + root;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i) out += L'\\';
        out += parts[i];
    }
    return out;
}

constexpr std::int64_t kEpochDifference = 11644473600LL; // 1601-01-01 to 1970-01-01, in seconds

std::int64_t unix_time(const FILETIME& ft) {
    ULARGE_INTEGER v;
    v.LowPart = ft.dwLowDateTime;
    v.HighPart = ft.dwHighDateTime;
    if (v.QuadPart == 0) return 0;
    return static_cast<std::int64_t>(v.QuadPart / 10000000ULL) - kEpochDifference;
}

FILETIME file_time(std::int64_t unix_seconds) {
    ULARGE_INTEGER v;
    v.QuadPart = static_cast<ULONGLONG>((unix_seconds + kEpochDifference) * 10000000LL);
    FILETIME ft;
    ft.dwLowDateTime = v.LowPart;
    ft.dwHighDateTime = v.HighPart;
    return ft;
}

HANDLE handle_of(std::intptr_t fd) { return reinterpret_cast<HANDLE>(fd); }

} // namespace

std::string last_os_error() { return message_for(GetLastError()); }

File::~File() {
    if (fd_ != -1) CloseHandle(handle_of(fd_));
}

File::File(File&& other) noexcept : fd_(other.fd_), path_(std::move(other.path_)) {
    other.fd_ = -1;
}

File& File::operator=(File&& other) noexcept {
    if (this != &other) {
        if (fd_ != -1) CloseHandle(handle_of(fd_));
        fd_ = other.fd_;
        path_ = std::move(other.path_);
        other.fd_ = -1;
    }
    return *this;
}

Result<File> File::open(const std::string& path, Mode mode) {
    DWORD access = mode == Mode::read ? GENERIC_READ : GENERIC_READ | GENERIC_WRITE;
    DWORD disposition = mode == Mode::create_new ? CREATE_NEW : OPEN_EXISTING;
    // Others may read, write, rename and delete it while it is open, as on POSIX.
    HANDLE h = CreateFileW(native(path).c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return io_error("cannot open", path);
    File file;
    file.fd_ = reinterpret_cast<std::intptr_t>(h);
    file.path_ = path;
    return file;
}

Result<std::uint64_t> File::size() const {
    LARGE_INTEGER size;
    if (!GetFileSizeEx(handle_of(fd_), &size)) return io_error("cannot stat", path_);
    return static_cast<std::uint64_t>(size.QuadPart);
}

// ReadFile/WriteFile with an OVERLAPPED offset on a synchronous handle read
// and write at that offset, like pread/pwrite; calls from several threads
// at once are fine.
Status File::read_at(std::uint64_t offset, MutableBytes out) const {
    std::size_t done = 0;
    while (done < out.size()) {
        DWORD want = static_cast<DWORD>(std::min<std::size_t>(out.size() - done, 1u << 30));
        OVERLAPPED o{};
        std::uint64_t at = offset + done;
        o.Offset = static_cast<DWORD>(at);
        o.OffsetHigh = static_cast<DWORD>(at >> 32);
        DWORD got = 0;
        if (!ReadFile(handle_of(fd_), out.data() + done, want, &got, &o)) {
            DWORD e = GetLastError();
            if (e == ERROR_HANDLE_EOF) got = 0;
            else return io_error("cannot read", path_, e);
        }
        if (got == 0) return make_error(Errc::invalid_data, "unexpected end of file in '" + path_ + "'");
        done += got;
    }
    return {};
}

Status File::write_at(std::uint64_t offset, Bytes data) {
    std::size_t done = 0;
    while (done < data.size()) {
        DWORD want = static_cast<DWORD>(std::min<std::size_t>(data.size() - done, 1u << 30));
        OVERLAPPED o{};
        std::uint64_t at = offset + done;
        o.Offset = static_cast<DWORD>(at);
        o.OffsetHigh = static_cast<DWORD>(at >> 32);
        DWORD put = 0;
        if (!WriteFile(handle_of(fd_), data.data() + done, want, &put, &o)) return io_error("cannot write", path_);
        if (put == 0) return make_error(Errc::io, "cannot write '" + path_ + "': nothing was written");
        done += put;
    }
    return {};
}

Status File::truncate(std::uint64_t length) {
    FILE_END_OF_FILE_INFO info;
    info.EndOfFile.QuadPart = static_cast<LONGLONG>(length);
    if (!SetFileInformationByHandle(handle_of(fd_), FileEndOfFileInfo, &info, sizeof info))
        return io_error("cannot resize", path_);
    return {};
}

Status File::sync() {
    if (!FlushFileBuffers(handle_of(fd_))) return io_error("cannot flush", path_);
    return {};
}

Status File::close() {
    if (fd_ == -1) return {};
    HANDLE h = handle_of(fd_);
    fd_ = -1;
    if (!CloseHandle(h)) return io_error("cannot close", path_);
    return {};
}

PathStat stat_path(const std::string& path, bool follow_links, std::string* error) {
    PathStat out;
    WIN32_FILE_ATTRIBUTE_DATA data;
    std::wstring w = native(path);
    if (!GetFileAttributesExW(w.c_str(), GetFileExInfoStandard, &data)) {
        if (error) *error = last_os_error();
        return out;
    }
    if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && follow_links) {
        // A link: report what it points at.
        HANDLE h = CreateFileW(w.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            if (error) *error = last_os_error();
            return out;
        }
        BY_HANDLE_FILE_INFORMATION info;
        BOOL ok = GetFileInformationByHandle(h, &info);
        CloseHandle(h);
        if (!ok) {
            if (error) *error = last_os_error();
            return out;
        }
        data.dwFileAttributes = info.dwFileAttributes & ~DWORD(FILE_ATTRIBUTE_REPARSE_POINT);
        data.ftLastAccessTime = info.ftLastAccessTime;
        data.ftLastWriteTime = info.ftLastWriteTime;
        data.nFileSizeHigh = info.nFileSizeHigh;
        data.nFileSizeLow = info.nFileSizeLow;
    }
    if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) out.kind = PathKind::link;
    else if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) out.kind = PathKind::directory;
    else if (data.dwFileAttributes & FILE_ATTRIBUTE_DEVICE) out.kind = PathKind::other;
    else out.kind = PathKind::file;
    if (out.kind == PathKind::file) out.size = (std::uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    out.atime = unix_time(data.ftLastAccessTime);
    out.mtime = unix_time(data.ftLastWriteTime);
    return out;
}

Result<std::vector<std::string>> list_directory(const std::string& path) {
    std::wstring pattern = native(path);
    if (!pattern.empty() && !is_sep(pattern.back())) pattern += L'\\';
    pattern += L'*';
    WIN32_FIND_DATAW data;
    HANDLE h = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    std::vector<std::string> names;
    if (h == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_FILE_NOT_FOUND) return names; // an empty drive root
        return io_error("cannot open folder", path);
    }
    do {
        std::wstring name = data.cFileName;
        if (name != L"." && name != L"..") names.push_back(narrow(name));
    } while (FindNextFileW(h, &data));
    DWORD e = GetLastError();
    FindClose(h);
    if (e != ERROR_NO_MORE_FILES) return io_error("cannot read folder", path, e);
    return names;
}

Status make_directory(const std::string& path) {
    if (!CreateDirectoryW(native(path).c_str(), nullptr)) return io_error("cannot create folder", path);
    return {};
}

Status remove_file(const std::string& path) {
    std::wstring w = native(path);
    if (DeleteFileW(w.c_str())) return {};
    // POSIX deletes read-only files; so does Alembic.
    DWORD e = GetLastError();
    if (e == ERROR_ACCESS_DENIED) {
        DWORD attributes = GetFileAttributesW(w.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_READONLY) &&
            SetFileAttributesW(w.c_str(), attributes & ~DWORD(FILE_ATTRIBUTE_READONLY)) && DeleteFileW(w.c_str()))
            return {};
    }
    return io_error("cannot delete", path, e);
}

Status remove_directory(const std::string& path) {
    if (!RemoveDirectoryW(native(path).c_str())) return io_error("cannot delete", path);
    return {};
}

Status rename_path(const std::string& from, const std::string& to) {
    if (MoveFileExW(native(from).c_str(), native(to).c_str(), MOVEFILE_REPLACE_EXISTING)) return {};
    Error e = io_error("cannot rename", from);
    e.message += " (to '" + to + "')";
    return e;
}

Status set_file_times(const std::string& path, std::int64_t atime, std::int64_t mtime) {
    HANDLE h = CreateFileW(native(path).c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr); // folders too
    if (h == INVALID_HANDLE_VALUE) return io_error("cannot set the times of", path);
    FILETIME a = file_time(atime), m = file_time(mtime);
    BOOL ok = SetFileTime(h, nullptr, &a, &m);
    DWORD e = GetLastError();
    CloseHandle(h);
    if (!ok) return io_error("cannot set the times of", path, e);
    return {};
}

void flush_file_system() {}

Result<DiskSpace> disk_space(const std::string& path) {
    ULARGE_INTEGER available, total;
    std::wstring w = native(path);
    if (!w.empty() && !is_sep(w.back())) w += L'\\'; // a folder; required for shares
    if (!GetDiskFreeSpaceExW(w.c_str(), &available, &total, nullptr)) return io_error("cannot read the free space of", path);
    DiskSpace s;
    s.total = total.QuadPart;
    s.free = available.QuadPart;
    return s;
}

int process_id() { return static_cast<int>(GetCurrentProcessId()); }

bool is_absolute_path(const std::string& path) {
    if (path.size() >= 3 && ((path[0] | 0x20) >= 'a' && (path[0] | 0x20) <= 'z') && path[1] == ':' &&
        (path[2] == '/' || path[2] == '\\'))
        return true;
    return path.size() >= 2 && (path[0] == '/' || path[0] == '\\') && (path[1] == '/' || path[1] == '\\');
}

std::string portable_path(std::string path) {
    for (char& c : path)
        if (c == '\\') c = '/';
    return path;
}

std::string environment(const char* name) {
    const wchar_t* v = _wgetenv(widen(name).c_str());
    return v ? narrow(v, wcslen(v)) : "";
}

std::vector<std::string> program_arguments(int argc, char** argv) {
    int n = 0;
    wchar_t** wide = CommandLineToArgvW(GetCommandLineW(), &n);
    if (!wide) return std::vector<std::string>(argv, argv + argc);
    std::vector<std::string> out;
    for (int i = 0; i < n; ++i) out.push_back(narrow(wide[i], wcslen(wide[i])));
    LocalFree(wide);
    return out;
}

void use_utf8_console() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
}

bool stderr_is_terminal() { return _isatty(_fileno(stderr)) != 0; }

std::FILE* open_stdio(const std::string& path, const char* mode) {
    return _wfopen(native(path).c_str(), widen(mode).c_str());
}

Result<std::string> absolute_path(const std::string& path) {
    std::wstring w = widen(path.empty() ? "." : path);
    for (wchar_t& c : w)
        if (c == L'/') c = L'\\';
    DWORD n = GetFullPathNameW(w.c_str(), 0, nullptr, nullptr);
    if (n == 0) return io_error("cannot resolve", path);
    std::wstring full(n, L'\0');
    n = GetFullPathNameW(w.c_str(), n, full.data(), nullptr);
    if (n == 0) return io_error("cannot resolve", path);
    full.resize(n);
    if (full.rfind(L"\\\\?\\UNC\\", 0) == 0) full = L"\\\\" + full.substr(8);
    else if (full.rfind(L"\\\\?\\", 0) == 0) full = full.substr(4);
    std::string out = portable_path(narrow(full));
    // A drive root keeps its slash ("C:/"); nothing else ends in one.
    while (out.size() > 3 && out.back() == '/') out.pop_back();
    return out;
}

} // namespace alembic

#endif // _WIN32
