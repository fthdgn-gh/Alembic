// SPDX-License-Identifier: GPL-3.0-or-later
// See probe.hpp. On the console this file calls libc and libkernel directly
// (files, sockets, memory), like platform_ps5.cpp: it measures exactly what
// those calls allow in the process it runs in.
#include "probe.hpp"

#include "alembic/sha256.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#if defined(__PROSPERO__)
#include "https_ps5.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/cpuset.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>

extern "C" {
void* scePthreadSelf(void);
int scePthreadGetaffinity(void* thread, std::uint64_t* mask);
int scePthreadGetprio(void* thread, int* prio);
int sceKernelGetCurrentCpu(void);
int sceKernelGetCpumode(void);
int sceKernelIsNeoMode(void);
std::size_t sceKernelGetDirectMemorySize(void);
int sceKernelLoadStartModule(const char* path, std::size_t argc, const void* argv, std::uint32_t flags, void* opt,
                             int* result);
int sceKernelDlsym(int handle, const char* symbol, void** address);
}
#endif

namespace alembic::engine::probe {

namespace {

using Clock = std::chrono::steady_clock;

double seconds_since(Clock::time_point t0) {
    return std::chrono::duration<double>(Clock::now() - t0).count();
}

[[maybe_unused]] std::string hex(std::uint64_t v) {
    char text[24];
    std::snprintf(text, sizeof text, "0x%llx", static_cast<unsigned long long>(v));
    return text;
}

// "0-3,8" from a bit mask.
std::string cpu_list(std::uint64_t mask) {
    std::string out;
    for (int c = 0; c < 64; ++c) {
        if (!(mask >> c & 1)) continue;
        int end = c;
        while (end + 1 < 64 && (mask >> (end + 1) & 1)) ++end;
        out += (out.empty() ? "" : ",") + std::to_string(c) + (end > c ? "-" + std::to_string(end) : "");
        c = end;
    }
    return out;
}

int current_cpu() {
#if defined(__PROSPERO__)
    return sceKernelGetCurrentCpu();
#else
    return -1;
#endif
}

// SHA-256 of 16 MB in 256 KB blocks spread over n threads (no allocation,
// no locks: how much CPU the process gets), and the CPUs its threads ran on.
JsonValue cpu_section() {
    JsonValue v = JsonValue::object();
    const unsigned cores = std::max(1u, std::thread::hardware_concurrency());
    v.set("hardware_concurrency", static_cast<std::int64_t>(cores));
#if defined(__PROSPERO__)
    {
        std::uint64_t mask = 0;
        int prio = -1;
        void* self = scePthreadSelf();
        const int a = scePthreadGetaffinity(self, &mask);
        const int p = scePthreadGetprio(self, &prio);
        v.set("thread_mask", hex(mask) + (a ? " (rc " + hex(static_cast<std::uint32_t>(a)) + ")" : ""));
        v.set("thread_cpus", cpu_list(mask));
        v.set("thread_prio", static_cast<std::int64_t>(prio));
        if (p) v.set("thread_prio_rc", hex(static_cast<std::uint32_t>(p)));
        cpuset_t set;
        CPU_ZERO(&set);
        if (cpuset_getaffinity(CPU_LEVEL_WHICH, CPU_WHICH_PID, -1, sizeof set, &set) == 0) {
            std::uint64_t m = 0;
            for (int c = 0; c < 64; ++c)
                if (CPU_ISSET(c, &set)) m |= 1ull << c;
            v.set("process_cpus", cpu_list(m));
        } else {
            v.set("process_cpus", std::string("cpuset_getaffinity: ") + std::strerror(errno));
        }
    }
#endif
    constexpr std::size_t kBlock = 0x40000, kTotal = 16u << 20, kBlocks = kTotal / kBlock;
    std::vector<std::uint8_t> data(kTotal);
    std::uint32_t x = 2463534242u;
    for (auto& b : data) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        b = static_cast<std::uint8_t>(x);
    }
    std::atomic<std::uint64_t> seen{0};
    JsonValue by = JsonValue::object();
    for (unsigned t = 1;; t *= 2) {
        const unsigned n = std::min(t, std::max(cores, 16u));
        const auto t0 = Clock::now();
        std::vector<std::thread> pool;
        for (unsigned i = 0; i < n; ++i)
            pool.emplace_back([&, i] {
                // 4 passes, so a thread lives long enough to be moved around.
                for (int pass = 0; pass < 4; ++pass)
                    for (std::size_t b = i; b < kBlocks; b += n) {
                        const int c = current_cpu();
                        if (c >= 0 && c < 64) seen.fetch_or(1ull << c, std::memory_order_relaxed);
                        Sha256 h;
                        h.update(Bytes(data.data() + b * kBlock, kBlock));
                        (void)h.finish();
                    }
            });
        for (std::thread& th : pool) th.join();
        const double s = seconds_since(t0);
        by.set(std::to_string(n), s > 0 ? 4.0 * kTotal / 1e6 / s : 0.0);
        if (n >= 16 || n >= std::max(cores, 16u)) break;
    }
    v.set("sha256_mb_s_by_threads", std::move(by));
    if (seen.load()) v.set("cpus_used", cpu_list(seen.load()));
    return v;
}

#if defined(__PROSPERO__)

std::string err(const char* what) { return std::string(what) + ": " + std::strerror(errno); }

// statfs is in libkernel_sys only, which a game process does not load: linked
// directly, the app jumped to an unresolved import (SIGSEGV, 2026-11-08). So
// it is looked up at run time; `why` says how that went.
using StatfsFn = int (*)(const char*, struct statfs*);
StatfsFn statfs_fn(std::string* why) {
    static StatfsFn fn = nullptr;
    static std::string note;
    static bool done = false;
    if (!done) {
        done = true;
        const int h = sceKernelLoadStartModule("/system/common/lib/libkernel_sys.sprx", 0, nullptr, 0, nullptr, nullptr);
        void* a = nullptr;
        if (h < 0) note = "libkernel_sys.sprx did not load: rc " + hex(static_cast<std::uint32_t>(h));
        else if (const int rc = sceKernelDlsym(h, "statfs", &a); rc != 0 || !a) note = "no statfs in libkernel_sys: rc " + hex(static_cast<std::uint32_t>(rc));
        else {
            fn = reinterpret_cast<StatfsFn>(a);
            note = "libkernel_sys handle " + std::to_string(h);
        }
    }
    *why = note;
    return fn;
}

JsonValue process_section(const Options& o) {
    JsonValue v = JsonValue::object();
    v.set("pid", static_cast<std::int64_t>(getpid()));
    v.set("ppid", static_cast<std::int64_t>(getppid()));
    v.set("uid", static_cast<std::int64_t>(getuid()));
    v.set("euid", static_cast<std::int64_t>(geteuid()));
    v.set("gid", static_cast<std::int64_t>(getgid()));
    char cwd[512] = {};
    v.set("cwd", getcwd(cwd, sizeof cwd) ? std::string(cwd) : err("getcwd"));
    int jailed = -1;
    std::size_t len = sizeof jailed;
    if (sysctlbyname("security.jail.jailed", &jailed, &len, nullptr, 0) == 0) v.set("jailed", jailed != 0);
    else v.set("jailed", err("sysctl"));
    int ncpu = 0;
    len = sizeof ncpu;
    if (sysctlbyname("hw.ncpu", &ncpu, &len, nullptr, 0) == 0) v.set("hw_ncpu", static_cast<std::int64_t>(ncpu));
    v.set("cpumode", static_cast<std::int64_t>(sceKernelGetCpumode()));
    v.set("neo_mode", static_cast<std::int64_t>(sceKernelIsNeoMode()));
    v.set("direct_memory_mb", static_cast<std::int64_t>(sceKernelGetDirectMemorySize() >> 20));
    (void)o;
    return v;
}

// The largest single malloc (not touched), then how much anonymous memory
// can be mapped and touched in 64 MB pieces up to the cap (all released).
JsonValue memory_section(const Options& o) {
    JsonValue v = JsonValue::object();
    std::uint64_t lo = 0, hi = 4096;  // MB
    while (lo < hi) {
        const std::uint64_t mid = (lo + hi + 1) / 2;
        void* p = std::malloc(static_cast<std::size_t>(mid) << 20);
        if (p) {
            std::free(p);
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }
    v.set("largest_malloc_mb", static_cast<std::int64_t>(lo));
    constexpr std::size_t kPiece = 64u << 20;
    std::vector<void*> pieces;
    const auto t0 = Clock::now();
    std::string stop = "cap reached";
    while (pieces.size() * 64 < o.mem_cap_mb) {
        void* p = mmap(nullptr, kPiece, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
        if (p == MAP_FAILED) {
            stop = err("mmap");
            break;
        }
        for (std::size_t at = 0; at < kPiece; at += 0x4000) static_cast<volatile std::uint8_t*>(p)[at] = 1;
        pieces.push_back(p);
    }
    v.set("mapped_touched_mb", static_cast<std::int64_t>(pieces.size() * 64));
    v.set("mapped_stop", stop);
    v.set("mapped_seconds", seconds_since(t0));
    for (void* p : pieces) munmap(p, kPiece);
    return v;
}

struct PathCheck {
    const char* path;
    bool try_write;  // only where a test file is harmless
};

const PathCheck kPaths[] = {
    {"/", false},           {"/data", true},          {"/data/homebrew", true}, {"/data/alembic", true},
    {"/user", false},       {"/user/app", false},     {"/user/appmeta", false}, {"/user/home", false},
    {"/system", false},     {"/system_data", false},  {"/system_ex", false},    {"/preinst", false},
    {"/mnt", false},        {"/mnt/sandbox", false},  {"/mnt/usb0", true},      {"/mnt/usb1", true},
    {"/mnt/usb2", true},    {"/mnt/usb3", true},      {"/mnt/ext0", true},      {"/mnt/ext1", true},
    {"/mnt/shadowmnt", false}, {"/app0", false},      {"/download0", true},     {"/hostapp", false},
    {"/dev", false},
};

bool try_write(const std::string& dir, std::string* why) {
    const std::string file = dir + "/.alembic-probe-" + std::to_string(getpid());
    const int fd = open(file.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0666);
    if (fd < 0) {
        *why = err("open");
        return false;
    }
    const bool ok = write(fd, "x", 1) == 1;
    if (!ok) *why = err("write");
    close(fd);
    unlink(file.c_str());
    return ok;
}

JsonValue files_section(const Options& o, std::vector<std::string>* writable) {
    JsonValue v = JsonValue::object();
    std::string statfs_note;
    const StatfsFn statfs_call = statfs_fn(&statfs_note);
    v.set("statfs", statfs_note);
    for (const PathCheck& pc : kPaths) {
        if (o.step) o.step(std::string("files ") + pc.path);
        JsonValue e = JsonValue::object();
        struct stat st {};
        if (stat(pc.path, &st) != 0) {
            e.set("stat", err("stat"));
            v.set(pc.path, std::move(e));
            continue;
        }
        e.set("dir", S_ISDIR(st.st_mode));
        e.set("mode", hex(st.st_mode & 07777));
        e.set("owner", static_cast<std::int64_t>(st.st_uid));
        if (S_ISDIR(st.st_mode)) {
            if (DIR* d = opendir(pc.path)) {
                std::int64_t n = 0;
                std::string first;
                while (dirent* de = readdir(d)) {
                    if (!std::strcmp(de->d_name, ".") || !std::strcmp(de->d_name, "..")) continue;
                    // Names only of system folders (the owner's own files stay out of reports).
                    if (n < 6 && (std::strncmp(pc.path, "/data", 5) != 0 && std::strncmp(pc.path, "/mnt/", 5) != 0 &&
                                  std::strncmp(pc.path, "/user", 5) != 0))
                        first += (first.empty() ? "" : " ") + std::string(de->d_name);
                    if (++n >= 10000) break;
                }
                closedir(d);
                e.set("entries", n);
                if (!first.empty()) e.set("first", first);
            } else {
                e.set("entries", err("opendir"));
            }
            struct statfs fs {};
            if (statfs_call && statfs_call(pc.path, &fs) == 0) {
                e.set("fs", std::string(fs.f_fstypename));
                e.set("mounted_on", std::string(fs.f_mntonname));
                e.set("free_gb", static_cast<double>(fs.f_bavail) * fs.f_bsize / 1e9);
                e.set("read_only", (fs.f_flags & MNT_RDONLY) != 0);
            }
            if (pc.try_write) {
                std::string why;
                const bool ok = try_write(pc.path, &why);
                e.set("writable", ok);
                if (!ok) e.set("write_error", why);
                else if (writable) writable->push_back(pc.path);
            }
        }
        v.set(pc.path, std::move(e));
    }
    return v;
}

// 256 MB written in 1 MB pieces (fsync at the end), read back, deleted.
JsonValue io_one(const std::string& dir) {
    JsonValue v = JsonValue::object();
    const std::string file = dir + "/.alembic-probe-io-" + std::to_string(getpid());
    constexpr std::size_t kPiece = 1u << 20, kCount = 256;
    std::vector<std::uint8_t> buf(kPiece);
    for (std::size_t i = 0; i < kPiece; ++i) buf[i] = static_cast<std::uint8_t>(i * 131 + 7);
    int fd = open(file.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0666);
    if (fd < 0) {
        v.set("error", err("open"));
        return v;
    }
    auto t0 = Clock::now();
    std::size_t done = 0;
    for (; done < kCount; ++done)
        if (write(fd, buf.data(), kPiece) != static_cast<ssize_t>(kPiece)) {
            v.set("error", err("write"));
            break;
        }
    fsync(fd);
    close(fd);
    double s = seconds_since(t0);
    v.set("write_mb_s", s > 0 ? done * 1.0 / s : 0.0);
    fd = open(file.c_str(), O_RDONLY);
    if (fd >= 0) {
        t0 = Clock::now();
        std::size_t got = 0;
        for (ssize_t r; (r = read(fd, buf.data(), kPiece)) > 0;) got += static_cast<std::size_t>(r);
        close(fd);
        s = seconds_since(t0);
        v.set("read_mb_s", s > 0 ? got / double(1u << 20) / s : 0.0);
    }
    unlink(file.c_str());
    return v;
}

std::string tcp_listen(std::uint16_t port, std::uint16_t* bound) {
    const int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return err("socket");
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a {};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    std::string out = "ok";
    if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) out = err("bind");
    else if (listen(s, 4) != 0) out = err("listen");
    else if (bound) {
        socklen_t len = sizeof a;
        getsockname(s, reinterpret_cast<sockaddr*>(&a), &len);
        *bound = ntohs(a.sin_port);
    }
    close(s);
    return out;
}

std::string tcp_connect_local(std::uint16_t port) {
    const int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return err("socket");
    sockaddr_in a {};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    std::string out = connect(s, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0 ? "ok" : err("connect");
    close(s);
    return out;
}

JsonValue network_section(const Options& o) {
    JsonValue v = JsonValue::object();
    std::uint16_t port = 0;
    std::string r = tcp_listen(0, &port);
    v.set("listen_any_port", r == "ok" ? "ok, got " + std::to_string(port) : r);
    v.set("listen_5949", tcp_listen(5949, nullptr));
    // The engine's port (the ELF loader's is left alone: it would take the
    // empty connection for a payload).
    v.set("connect_engine_5940", tcp_connect_local(5940));
    const int u = socket(AF_INET, SOCK_DGRAM, 0);
    if (u >= 0) {
        int one = 1;
        v.set("udp_broadcast", setsockopt(u, SOL_SOCKET, SO_BROADCAST, &one, sizeof one) == 0 ? "ok" : err("setsockopt"));
        close(u);
    }
    if (o.https) {
        const auto t0 = Clock::now();
        auto got = https_get("https://api.github.com/zen", 4096);
        v.set("https_github", got.ok() ? "ok" : got.error().message);
        v.set("https_seconds", seconds_since(t0));
    }
    return v;
}

JsonValue modules_section() {
    JsonValue v = JsonValue::object();
    for (const char* path : {"/system/common/lib/libSceAppInstUtil.sprx", "/system/common/lib/libSceBgft.sprx",
                             "/system/common/lib/libSceSystemService.sprx"}) {
        const int rc = sceKernelLoadStartModule(path, 0, nullptr, 0, nullptr, nullptr);
        v.set(std::strrchr(path, '/') + 1, rc >= 0 ? "handle " + std::to_string(rc) : "rc " + hex(static_cast<std::uint32_t>(rc)));
    }
    return v;
}

#endif

} // namespace

JsonValue run(const Options& o) {
    auto step = [&](const char* name) {
        if (o.step) o.step(name);
    };
    JsonValue v = JsonValue::object();
    v.set("process", o.process);
    const auto t0 = Clock::now();
#if defined(__PROSPERO__)
    v.set("platform", "ps5");
    step("process");
    v.set("identity", process_section(o));
    step("cpu");
    v.set("cpu", cpu_section());
    step("memory");
    v.set("memory", memory_section(o));
    step("files");
    std::vector<std::string> writable;
    v.set("files", files_section(o, &writable));
    if (o.io) {
        step("io");
        JsonValue io = JsonValue::object();
        // /data and the first USB or extended drive that took a test file.
        bool drive = false;
        for (const std::string& d : writable) {
            const bool is_drive = d.rfind("/mnt/", 0) == 0;
            if (d != "/data" && !(is_drive && !drive)) continue;
            drive = drive || is_drive;
            io.set(d, io_one(d));
        }
        v.set("io", std::move(io));
    }
    step("network");
    v.set("network", network_section(o));
    step("modules");
    v.set("modules", modules_section());
#else
    v.set("platform", "pc");
    v.set("note", "only CPU is measured off the console");
    step("cpu");
    v.set("cpu", cpu_section());
#endif
    v.set("seconds", seconds_since(t0));
    return v;
}

} // namespace alembic::engine::probe
