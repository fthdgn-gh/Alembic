// SPDX-License-Identifier: GPL-3.0-or-later
// PS5 payload side. Notifications follow ps5-payload-dev/sdk's samples/notify
// (GPL-3.0-or-later); keeping the console awake follows PS5-LZ4-Compression's
// src/lzc_power_guard.c (GPL-3.0-or-later).
#include "platform.hpp"
#include "json_value.hpp"

#include <chrono>
#include <cstdio>
#include <mutex>

extern "C" {
int sceNotificationSend(int user_id, bool is_logged, const char* payload) __attribute__((weak));
int sceSystemServicePowerTick(void) __attribute__((weak));
}

namespace alembic::engine::platform {

namespace {
constexpr int kSystemUser = 0xFE; // SCE_NOTIFICATION_LOCAL_USER_ID_SYSTEM
}

const char* name() { return "ps5"; }

void notify(const std::string& message) {
    std::fprintf(stderr, "[notify] %s\n", message.c_str());
    if (!sceNotificationSend) return;
    // A plain toast with the predefined download icon.
    std::string payload = "{\"rawData\":{\"viewTemplateType\":\"InteractiveToastTemplateB\","
                          "\"channelType\":\"Downloads\",\"useCaseId\":\"IDC\",\"toastOverwriteType\":\"No\","
                          "\"isImmediate\":true,\"priority\":100,\"viewData\":{"
                          "\"icon\":{\"type\":\"Predefined\",\"parameters\":{\"icon\":\"download\"}},"
                          "\"message\":{\"body\":" +
                          json_quote(message) +
                          "},\"subMessage\":{\"body\":\"Alembic\"}}},"
                          "\"createdDateTime\":\"2026-01-01T00:00:00.000Z\",\"localNotificationId\":\"5940\"}";
    sceNotificationSend(kSystemUser, true, payload.c_str());
}

void keep_awake() {
    // At most once every 20 seconds.
    static std::mutex mutex;
    static std::chrono::steady_clock::time_point last{};
    std::lock_guard<std::mutex> lock(mutex);
    auto now = std::chrono::steady_clock::now();
    if (last != std::chrono::steady_clock::time_point{} && now - last < std::chrono::seconds(20)) return;
    last = now;
    if (sceSystemServicePowerTick) sceSystemServicePowerTick();
}

std::vector<std::string> default_roots() {
    std::vector<std::string> roots{"/data"};
    for (int i = 0; i < 8; ++i) roots.push_back("/mnt/usb" + std::to_string(i));
    for (int i = 0; i < 2; ++i) roots.push_back("/mnt/ext" + std::to_string(i));
    return roots;
}

std::string default_state_dir() { return "/data/alembic"; }

} // namespace alembic::engine::platform
