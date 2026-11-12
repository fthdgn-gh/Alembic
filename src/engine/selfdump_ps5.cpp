// SPDX-License-Identifier: GPL-3.0-or-later
// Decrypts a game's SELF segments on the PS5 by borrowing the kernel's own
// self pager: for one mmap, the vnode pager-table entry is pointed at the self
// pager, so the console's loader path pages the segment in decrypted. No
// crypto of our own. self_extract.cpp assembles the whole ELF from the
// segments this returns.
//
// Ported from ps5-payload-dev/ftpsrv self-prospero.c (GPL-3.0, John Törnblom;
// derived from idlesauce/ps5-self-pager). The per-firmware pager-table offsets
// are that project's. Only compiled into the PS5 engine; the PC build uses the
// stub in platform_host.cpp.
#include "platform.hpp"

#if defined(__PROSPERO__)

#include <cerrno>
#include <cstring>
#include <mutex>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <ps5/kernel.h>

namespace alembic::engine::platform {

namespace {

constexpr std::uint64_t kSuperpageSize = 0x200000;

std::mutex g_mutex; // the pager swap is system-wide while in effect: one at a time
bool g_resolved = false;
bool g_known = false;
std::intptr_t g_pager_table = 0;
std::intptr_t g_ops_vnode = 0;
std::intptr_t g_ops_self = 0;

// The pager table's offset from the kernel data base, per firmware (fw >> 16).
std::intptr_t pager_table_offset(std::uint32_t fw) {
    switch (fw >> 16) {
    case 0x100: case 0x101: case 0x102: case 0x105:
    case 0x110: case 0x111: case 0x112: return 0xC27C40;
    case 0x113: case 0x114: return 0xC27CA0;
    case 0x200: return 0xC4EF60;
    case 0x220: case 0x225: case 0x226: return 0xC4EFA0;
    case 0x230: case 0x250: case 0x270: return 0xC4F120;
    case 0x300: case 0x310: case 0x320: case 0x321: return 0xCAF8C0;
    case 0x400: case 0x402: case 0x403: case 0x450: case 0x451: return 0xD20840;
    case 0x500: case 0x502: case 0x510: case 0x550: return 0xE0FEF0;
    case 0x600: case 0x602: case 0x650: return 0xE30410;
    case 0x700: case 0x701: return 0xE310C0;
    case 0x720: case 0x740: case 0x760: case 0x761: return 0xE31180;
    case 0x800: case 0x820: case 0x840: case 0x860: return 0xE31250;
    case 0x900: case 0x905: case 0x920: case 0x940: case 0x960: return 0xDE0420;
    case 0x1000: case 0x1001: case 0x1020: case 0x1040: case 0x1060: return 0xDE04F0;
    case 0x1100: case 0x1120: return 0xDF1940;
    case 0x1140: case 0x1160: return 0xDF1960;
    case 0x1200: case 0x1202: case 0x1220: case 0x1240: case 0x1260: case 0x1270: return 0xDF2860;
    case 0x1300: case 0x1320: return 0xE038D0;
    case 0x1340: case 0x1342: case 0x1360: return 0xE03910;
    case 0x1400: case 0x1410: case 0x1420: case 0x1422: return 0xE13E00;
    default: return 0;
    }
}

void resolve_once() {
    if (g_resolved) return;
    g_resolved = true;
    const std::intptr_t off = pager_table_offset(kernel_get_fw_version());
    if (!off) return;
    g_pager_table = KERNEL_ADDRESS_DATA_BASE + off;
    g_ops_vnode = static_cast<std::intptr_t>(kernel_getlong(g_pager_table + 2 * 8));
    g_ops_self = static_cast<std::intptr_t>(kernel_getlong(g_pager_table + 7 * 8));
    g_known = g_ops_vnode && g_ops_self;
}

// mmap one segment with the vnode pager entry swapped for the self pager's, so
// the kernel pages it in decrypted; the entry is restored right after.
void* mmap_self(std::size_t len, int fd, off_t offset, std::uint64_t align) {
    if (kernel_setlong(g_pager_table + 2 * 8, static_cast<std::uint64_t>(g_ops_self))) return MAP_FAILED;
    void* data = mmap(nullptr, len, PROT_READ, MAP_PRIVATE | MAP_ALIGNED(align), fd, offset);
    if (kernel_setlong(g_pager_table + 2 * 8, static_cast<std::uint64_t>(g_ops_vnode))) return MAP_FAILED;
    return data;
}

Result<std::vector<std::uint8_t>> decrypt_segment(const std::string& path, const self_extract::Segment& seg) {
    std::lock_guard<std::mutex> lock(g_mutex);
    resolve_once();
    if (!g_known) return make_error(Errc::unsupported, "this firmware's self pager is not known");

    const int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return make_error(Errc::io, "cannot open " + path + ": " + std::strerror(errno));
    struct CloseFd {
        int fd;
        ~CloseFd() { close(fd); }
    } guard{fd};

    off_t offset = static_cast<off_t>(static_cast<std::uint64_t>(seg.index) << 32);
    if (kernel_get_fw_version() >= 0x9000000) {
        const std::uint64_t aligned_vaddr = seg.vaddr & ~(seg.align - 1);
        offset |= static_cast<off_t>(aligned_vaddr & (kSuperpageSize - 1));
    }
    void* data = mmap_self(static_cast<std::size_t>(seg.file_size), fd, offset, seg.align);
    if (data == MAP_FAILED) return make_error(Errc::io, "mapping the segment failed");
    std::vector<std::uint8_t> out;
    bool ok = mlock(data, static_cast<std::size_t>(seg.file_size)) == 0;
    if (ok) {
        out.assign(static_cast<const std::uint8_t*>(data), static_cast<const std::uint8_t*>(data) + seg.file_size);
    }
    munmap(data, static_cast<std::size_t>(seg.file_size));
    if (!ok) return make_error(Errc::io, "reading the decrypted segment failed");
    return out;
}

} // namespace

self_extract::Decryptor self_decryptor(std::string* why) {
    std::lock_guard<std::mutex> lock(g_mutex);
    resolve_once();
    if (!g_known) {
        if (why) *why = "this PS5 firmware is not supported for decrypting executables yet";
        return {};
    }
    return [](const std::string& path, const self_extract::Segment& seg) { return decrypt_segment(path, seg); };
}

} // namespace alembic::engine::platform

#endif // __PROSPERO__
