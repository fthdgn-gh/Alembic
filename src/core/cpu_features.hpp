// SPDX-License-Identifier: GPL-3.0-or-later
// Which instruction-set extensions this x86-64 CPU has (CPUID), so code
// compiled for them with target attributes runs only where they exist. A
// build that targets them already (the PS5) needs no check. Elsewhere the
// answers are false.
#pragma once

#if defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#endif

namespace alembic::cpu {

struct Features {
    bool aes = false;    // AES-NI
    bool sse41 = false;
    bool sse42 = false;  // CRC32 (Castagnoli)
    bool ssse3 = false;
    bool sha = false;    // SHA-NI (SHA-1 and SHA-256)
};

inline const Features& features() {
    static const Features f = [] {
        Features r;
#if defined(__x86_64__) || defined(__i386__)
        unsigned a = 0, b = 0, c = 0, d = 0;
        if (__get_cpuid(1, &a, &b, &c, &d)) {
            r.ssse3 = (c >> 9) & 1;
            r.sse41 = (c >> 19) & 1;
            r.sse42 = (c >> 20) & 1;
            r.aes = (c >> 25) & 1;
        }
        if (__get_cpuid_max(0, nullptr) >= 7) {
            __cpuid_count(7, 0, a, b, c, d);
            r.sha = (b >> 29) & 1;
        }
#endif
        return r;
    }();
    return f;
}

} // namespace alembic::cpu
