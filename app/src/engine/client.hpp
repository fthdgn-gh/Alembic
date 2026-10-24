// SPDX-License-Identifier: GPL-3.0-or-later
// The engine as the app sees it: requests run on worker threads (a frame
// never waits for the network) and their replies come back on the frame
// loop's thread through pump(). Lane 0 is for quick calls the screen waits
// on; lane 1 for the steady polling and slow calls (inspect, plan), so one
// never queues behind the other.
#pragma once

#include "engine/http.hpp"
#include "json_value.hpp"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include <pthread.h>

namespace alembic::app {

using engine::JsonValue;

struct Reply {
    int status = 0;    // HTTP status; 0 when the engine did not answer
    std::string body;  // raw body
    JsonValue json;    // parsed body (null when it is not JSON)
    bool ok() const { return status >= 200 && status < 300; }
    // The engine's error text ({"error": "..."}), else the body or status.
    std::string error() const;
};

class Engine {
public:
    using Callback = std::function<void(const Reply&)>;
    static constexpr int kLanes = 2;

    ~Engine();
    void start(const std::string& host, std::uint16_t port);
    void stop();

    void call(const std::string& method, const std::string& path, const std::string& body, Callback done,
              int lane = 0, int timeout_ms = 8000);
    void get(const std::string& path, Callback done, int lane = 0) { call("GET", path, "", std::move(done), lane); }
    void post(const std::string& path, const JsonValue& body, Callback done, int lane = 0) {
        call("POST", path, body.dump(), std::move(done), lane);
    }

    // Runs the callbacks of finished requests on the calling thread.
    void pump();
    // Requests sent or waiting (for a busy indicator).
    int pending() const;

    const std::string& host() const { return host_; }
    std::uint16_t port() const { return port_; }

    // Raw file bytes of a GET (icons), with no JSON. `transform` runs on the
    // worker thread first (decoding an image there keeps frames smooth).
    using Transform = std::function<void(int status, std::string& body)>;
    void fetch(const std::string& path, Transform transform, std::function<void(int status, std::string body)> done,
               int lane = 1);

private:
    struct Job {
        std::string method, path, body;
        int timeout_ms = 8000;
        bool raw = false;
        Callback done;
        Transform transform;
        std::function<void(int, std::string)> done_raw;
        HttpReply reply;
    };
    struct Lane {
        Engine* owner = nullptr;
        int index = 0;
        pthread_t thread{};
        bool running = false;
        std::deque<Job> queue;
    };
    static void* thread_main(void* lane);
    void run(Lane& lane);

    std::string host_;
    std::uint16_t port_ = 0;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    bool stopping_ = false;
    Lane lanes_[kLanes];
    std::vector<Job> finished_;
    int in_flight_ = 0;
};

// Builds "path?k=v&k2=v2" with the values percent-encoded.
std::string url(const std::string& path, std::initializer_list<std::pair<const char*, std::string>> query);
std::string url_encode(const std::string& text);

}  // namespace alembic::app
