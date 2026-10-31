// SPDX-License-Identifier: GPL-3.0-or-later
#include "pkg_sources.hpp"

#include "alembic/io.hpp"
#include "platform.hpp"

#include <chrono>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

namespace alembic::engine::pkg_sources {

namespace {

struct Request {
    std::int64_t ms = 0; // since the source was added
    std::string method;
    std::string range;   // as sent ("" for none)
    std::uint64_t from = 0, length = 0;
    int status = 0;
};

struct Source {
    std::string token, path;
    std::shared_ptr<const RandomAccess> file;
    std::uint64_t size = 0;
    std::chrono::steady_clock::time_point added;
    std::vector<Request> requests; // the first kKeep
    std::uint64_t request_count = 0, bytes_served = 0, highest_end = 0;
    std::chrono::steady_clock::time_point last; // the last request's start
};

constexpr std::size_t kKeep = 4000;
constexpr std::size_t kSources = 16; // the oldest go beyond this

std::mutex mutex;
std::deque<std::shared_ptr<Source>> sources;

std::string new_token() {
    std::uint8_t raw[16];
    if (!platform::secure_random(raw, sizeof raw).ok()) {
        const auto t = std::chrono::steady_clock::now().time_since_epoch().count();
        for (std::size_t i = 0; i < sizeof raw; ++i) raw[i] = static_cast<std::uint8_t>(t >> (8 * (i % 8))) ^ static_cast<std::uint8_t>(i * 37);
    }
    std::string out;
    char hex[3];
    for (std::uint8_t b : raw) {
        std::snprintf(hex, sizeof hex, "%02x", b);
        out += hex;
    }
    return out;
}

HttpResponse plain(int status, const std::string& text) {
    HttpResponse r;
    r.status = status;
    r.content_type = "text/plain";
    r.body = text;
    return r;
}

// "bytes=a-b", "bytes=a-" or "bytes=-n" (the first range of a list).
bool parse_range(const std::string& header, std::uint64_t size, std::uint64_t& from, std::uint64_t& length) {
    if (header.rfind("bytes=", 0) != 0) return false;
    std::string spec = header.substr(6);
    if (std::size_t comma = spec.find(','); comma != std::string::npos) spec.resize(comma);
    const std::size_t dash = spec.find('-');
    if (dash == std::string::npos) return false;
    auto number = [](const std::string& s, std::uint64_t& v) {
        if (s.empty()) return false;
        v = 0;
        for (char c : s) {
            if (c < '0' || c > '9') return false;
            v = v * 10 + static_cast<std::uint64_t>(c - '0');
        }
        return true;
    };
    std::uint64_t a = 0, b = 0;
    const std::string first = spec.substr(0, dash), last = spec.substr(dash + 1);
    if (first.empty()) { // the last n bytes
        if (!number(last, b) || b == 0) return false;
        from = b > size ? 0 : size - b;
        length = size - from;
        return true;
    }
    if (!number(first, a) || a >= size) return false;
    if (last.empty()) b = size - 1;
    else if (!number(last, b) || b < a) return false;
    if (b >= size) b = size - 1;
    from = a;
    length = b - a + 1;
    return true;
}

} // namespace

Result<std::string> add(const std::string& path) {
    ALEMBIC_ASSIGN(File file, File::open(path, File::Mode::read));
    ALEMBIC_ASSIGN(std::uint64_t size, file.size());
    auto s = std::make_shared<Source>();
    s->token = new_token();
    s->path = path;
    s->file = std::make_shared<File>(std::move(file));
    s->size = size;
    s->added = std::chrono::steady_clock::now();
    s->last = s->added;
    std::lock_guard<std::mutex> lock(mutex);
    sources.push_back(s);
    while (sources.size() > kSources) sources.pop_front();
    return s->token;
}

HttpResponse serve(const HttpRequest& request) {
    if (request.peer != "127.0.0.1") return plain(403, "only for this console's installer");
    if (request.method != "GET" && request.method != "HEAD") return plain(405, "GET or HEAD");
    std::string name = request.path.substr(std::string("/pkg-source/").size());
    if (name.size() < 4 || name.compare(name.size() - 4, 4, ".pkg") != 0) return plain(404, "not found");
    name.resize(name.size() - 4);
    std::shared_ptr<Source> s;
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (const auto& c : sources)
            if (c->token == name) s = c;
    }
    if (!s) return plain(404, "not found");

    Request rec;
    rec.method = request.method;
    rec.ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - s->added).count();
    HttpResponse r;
    r.content_type = "application/octet-stream";
    r.headers["Accept-Ranges"] = "bytes";
    r.stream = s->file;
    std::uint64_t from = 0, length = s->size;
    if (auto it = request.headers.find("range"); it != request.headers.end()) {
        rec.range = it->second;
        if (!parse_range(it->second, s->size, from, length)) {
            r = plain(416, "bad range");
            r.headers["Content-Range"] = "bytes */" + std::to_string(s->size);
            rec.status = 416;
        } else {
            r.status = 206;
            r.headers["Content-Range"] =
                "bytes " + std::to_string(from) + "-" + std::to_string(from + length - 1) + "/" + std::to_string(s->size);
        }
    }
    if (rec.status == 0) {
        rec.status = r.status;
        r.stream_offset = from;
        r.stream_length = length;
        rec.from = from;
        rec.length = length;
    }
    {
        std::lock_guard<std::mutex> lock(mutex);
        ++s->request_count;
        s->last = std::chrono::steady_clock::now();
        if (request.method == "GET" && rec.status < 300) {
            s->bytes_served += rec.length;
            s->highest_end = std::max(s->highest_end, rec.from + rec.length);
        }
        if (s->requests.size() < kKeep) s->requests.push_back(rec);
    }
    return r;
}

std::optional<Status> status(const std::string& token) {
    std::lock_guard<std::mutex> lock(mutex);
    for (const auto& s : sources)
        if (s->token == token) {
            Status st;
            st.size = s->size;
            st.served = s->bytes_served;
            st.requests = s->request_count;
            // A request in progress streams for a while: idle counts from its start,
            // so the waiter allows for the longest one (an 18 MiB range).
            st.idle_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - s->last).count();
            return st;
        }
    return std::nullopt;
}

void remove(const std::string& token) {
    std::lock_guard<std::mutex> lock(mutex);
    for (auto it = sources.begin(); it != sources.end(); ++it)
        if ((*it)->token == token) {
            sources.erase(it);
            return;
        }
}

JsonValue report() {
    std::lock_guard<std::mutex> lock(mutex);
    JsonValue list = JsonValue::array();
    for (const auto& s : sources) {
        JsonValue v = JsonValue::object();
        v.set("token", s->token);
        v.set("path", s->path);
        v.set("size", static_cast<std::int64_t>(s->size));
        v.set("requests", static_cast<std::int64_t>(s->request_count));
        v.set("bytes_served", static_cast<std::int64_t>(s->bytes_served));
        v.set("highest_end", static_cast<std::int64_t>(s->highest_end));
        JsonValue reqs = JsonValue::array();
        for (const Request& q : s->requests) {
            JsonValue e = JsonValue::object();
            e.set("ms", q.ms);
            e.set("method", q.method);
            if (!q.range.empty()) e.set("range", q.range);
            e.set("from", static_cast<std::int64_t>(q.from));
            e.set("length", static_cast<std::int64_t>(q.length));
            e.set("status", static_cast<std::int64_t>(q.status));
            reqs.push(std::move(e));
        }
        v.set("log", std::move(reqs));
        list.push(std::move(v));
    }
    return list;
}

} // namespace alembic::engine::pkg_sources
