// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/client.hpp"

#include <utility>

namespace alembic::app {

std::string Reply::error() const {
    if (json.is_object() && !json["error"].str().empty()) return json["error"].str();
    if (status == 0) return body.empty() ? "the engine does not answer" : body;
    if (!body.empty() && body.size() < 200) return body;
    return "HTTP " + std::to_string(status);
}

Engine::~Engine() { stop(); }

void Engine::start(const std::string& host, std::uint16_t port) {
    host_ = host;
    port_ = port;
    stopping_ = false;
    for (int i = 0; i < kLanes; ++i) {
        Lane& lane = lanes_[i];
        lane.owner = this;
        lane.index = i;
        // Console threads are never time-sliced and their default stack is
        // small: give the worker a stack of its own size. Never name threads
        // (pthread_setname_np hangs on the console).
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 512 * 1024);
        lane.running = pthread_create(&lane.thread, &attr, &Engine::thread_main, &lane) == 0;
        pthread_attr_destroy(&attr);
    }
}

void Engine::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    for (Lane& lane : lanes_) {
        if (lane.running) pthread_join(lane.thread, nullptr);
        lane.running = false;
    }
}

void* Engine::thread_main(void* arg) {
    Lane* lane = static_cast<Lane*>(arg);
    lane->owner->run(*lane);
    return nullptr;
}

void Engine::run(Lane& lane) {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [&] { return stopping_ || !lane.queue.empty(); });
            if (stopping_) return;
            job = std::move(lane.queue.front());
            lane.queue.pop_front();
        }
        job.reply = http_request(host_, port_, job.method, job.path, job.body, job.timeout_ms);
        if (job.transform) job.transform(job.reply.status, job.reply.body);
        std::lock_guard<std::mutex> lock(mutex_);
        finished_.push_back(std::move(job));
    }
}

void Engine::call(const std::string& method, const std::string& path, const std::string& body, Callback done,
                  int lane, int timeout_ms) {
    Job job;
    job.method = method;
    job.path = path;
    job.body = body;
    job.timeout_ms = timeout_ms;
    job.done = std::move(done);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        lanes_[lane < 0 || lane >= kLanes ? 0 : lane].queue.push_back(std::move(job));
        ++in_flight_;
    }
    wake_.notify_all();
}

void Engine::fetch(const std::string& path, Transform transform, std::function<void(int, std::string)> done,
                   int lane) {
    Job job;
    job.method = "GET";
    job.path = path;
    job.raw = true;
    job.transform = std::move(transform);
    job.done_raw = std::move(done);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        lanes_[lane < 0 || lane >= kLanes ? 0 : lane].queue.push_back(std::move(job));
        ++in_flight_;
    }
    wake_.notify_all();
}

void Engine::pump() {
    std::vector<Job> done;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        done.swap(finished_);
        in_flight_ -= static_cast<int>(done.size());
    }
    for (Job& job : done) {
        if (job.raw) {
            if (job.done_raw) job.done_raw(job.reply.status, std::move(job.reply.body));
            continue;
        }
        Reply reply;
        reply.status = job.reply.status;
        reply.body = std::move(job.reply.body);
        if (reply.status != 0 && !reply.body.empty() && (reply.body[0] == '{' || reply.body[0] == '[')) {
            auto parsed = engine::parse_json(reply.body);
            if (parsed) reply.json = std::move(parsed.value());
        }
        if (job.done) job.done(reply);
    }
}

int Engine::pending() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return in_flight_;
}

std::string url_encode(const std::string& text) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : text) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~' || c == '/') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

std::string url(const std::string& path, std::initializer_list<std::pair<const char*, std::string>> query) {
    std::string out = path;
    char sep = '?';
    for (const auto& [key, value] : query) {
        out += sep;
        out += key;
        out += '=';
        out += url_encode(value);
        sep = '&';
    }
    return out;
}

}  // namespace alembic::app
