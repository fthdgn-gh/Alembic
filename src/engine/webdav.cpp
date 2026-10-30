// SPDX-License-Identifier: GPL-3.0-or-later
#include "webdav.hpp"

#include "net.hpp"
#include "netcrypto.hpp"
#include "platform.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace alembic::engine::webdav {

namespace {

Error dav_error(const std::string& what) { return make_error(Errc::io, "WebDAV: " + what); }

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::string status_text(int code) {
    switch (code) {
    case 401: return "wrong user name or password";
    case 403: return "not allowed";
    case 404: return "not found";
    case 405: return "the server does not allow this here";
    case 409: return "the folder above it is missing";
    case 412: return "it already exists";
    case 413: return "the file is too large for the server";
    case 423: return "it is locked";
    case 507: return "the server is out of space";
    default: return "HTTP " + std::to_string(code);
    }
}

Error http_error(const std::string& what, int code) {
    return make_error(code == 404 ? Errc::invalid_argument : Errc::io, "WebDAV: " + what + ": " + status_text(code));
}

// "realm=\"x\", nonce=\"y\", qop=\"auth\"" -> its parameters (names in lower case).
std::vector<std::pair<std::string, std::string>> auth_params(const std::string& text) {
    std::vector<std::pair<std::string, std::string>> out;
    std::size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && (text[i] == ',' || std::isspace(static_cast<unsigned char>(text[i])))) ++i;
        std::size_t eq = text.find('=', i);
        if (eq == std::string::npos) break;
        std::string name = lower(trim(text.substr(i, eq - i)));
        i = eq + 1;
        std::string value;
        if (i < text.size() && text[i] == '"') {
            for (++i; i < text.size() && text[i] != '"'; ++i) {
                if (text[i] == '\\' && i + 1 < text.size()) ++i;
                value += text[i];
            }
            ++i;
        } else {
            std::size_t comma = text.find(',', i);
            value = trim(text.substr(i, comma == std::string::npos ? std::string::npos : comma - i));
            i = comma == std::string::npos ? text.size() : comma;
        }
        out.emplace_back(name, value);
    }
    return out;
}

std::string base64(const std::string& in) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    std::size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        std::uint32_t v = (std::uint8_t(in[i]) << 16) | (std::uint8_t(in[i + 1]) << 8) | std::uint8_t(in[i + 2]);
        out += t[v >> 18];
        out += t[(v >> 12) & 63];
        out += t[(v >> 6) & 63];
        out += t[v & 63];
    }
    if (i + 1 == in.size()) {
        std::uint32_t v = std::uint8_t(in[i]) << 16;
        out += t[v >> 18];
        out += t[(v >> 12) & 63];
        out += "==";
    } else if (i + 2 == in.size()) {
        std::uint32_t v = (std::uint8_t(in[i]) << 16) | (std::uint8_t(in[i + 1]) << 8);
        out += t[v >> 18];
        out += t[(v >> 12) & 63];
        out += t[(v >> 6) & 63];
        out += '=';
    }
    return out;
}

std::string md5_hex(const std::string& s) {
    auto d = netcrypto::md5(Bytes(reinterpret_cast<const std::uint8_t*>(s.data()), s.size()));
    return netcrypto::hex(Bytes(d.data(), d.size()));
}

std::string xml_text(const std::string& s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') {
            out += s[i];
            continue;
        }
        std::size_t semi = s.find(';', i);
        if (semi == std::string::npos) {
            out += s[i];
            continue;
        }
        std::string name = s.substr(i + 1, semi - i - 1);
        if (name == "amp") out += '&';
        else if (name == "lt") out += '<';
        else if (name == "gt") out += '>';
        else if (name == "quot") out += '"';
        else if (name == "apos") out += '\'';
        else if (!name.empty() && name[0] == '#') {
            unsigned long cp = name.size() > 1 && (name[1] == 'x' || name[1] == 'X') ? std::strtoul(name.c_str() + 2, nullptr, 16)
                                                                                      : std::strtoul(name.c_str() + 1, nullptr, 10);
            if (cp < 0x80) out += static_cast<char>(cp);
            else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 63)); }
            else if (cp < 0x10000) { out += static_cast<char>(0xE0 | (cp >> 12)); out += static_cast<char>(0x80 | ((cp >> 6) & 63)); out += static_cast<char>(0x80 | (cp & 63)); }
            else { out += static_cast<char>(0xF0 | (cp >> 18)); out += static_cast<char>(0x80 | ((cp >> 12) & 63)); out += static_cast<char>(0x80 | ((cp >> 6) & 63)); out += static_cast<char>(0x80 | (cp & 63)); }
        } else {
            out += s.substr(i, semi - i + 1);
        }
        i = semi;
    }
    return out;
}

// The path part of an href: "http://host:80/a%20b/" -> "/a b/".
std::string href_path(const std::string& href) {
    std::string h = href;
    if (std::size_t scheme = h.find("://"); scheme != std::string::npos) {
        std::size_t slash = h.find('/', scheme + 3);
        h = slash == std::string::npos ? "/" : h.substr(slash);
    }
    return detail::decode(h);
}

std::string without_slash(std::string p) {
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    return p;
}

} // namespace

namespace detail {

std::string encode_path(const std::string& path) {
    static const char* digits = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : path) {
        if (std::isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += digits[c >> 4];
            out += digits[c & 15];
        }
    }
    return out;
}

std::string decode(const std::string& text) {
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size() && std::isxdigit(static_cast<unsigned char>(text[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(text[i + 2]))) {
            out += static_cast<char>(std::strtol(text.substr(i + 1, 2).c_str(), nullptr, 16));
            i += 2;
        } else {
            out += text[i];
        }
    }
    return out;
}

std::int64_t parse_http_date(const std::string& text) {
    // "Sun, 06 Nov 1994 08:49:37 GMT"
    static const char* months[] = {"jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec"};
    std::size_t comma = text.find(',');
    std::string rest = trim(comma == std::string::npos ? text : text.substr(comma + 1));
    int day = 0, year = 0, h = 0, m = 0, s = 0;
    char month[4] = {};
    if (std::sscanf(rest.c_str(), "%d %3s %d %d:%d:%d", &day, month, &year, &h, &m, &s) != 6) return 0;
    std::string mon = lower(month);
    int mi = -1;
    for (int i = 0; i < 12; ++i)
        if (mon == months[i]) mi = i;
    if (mi < 0) return 0;
    // Days from the civil date (Howard Hinnant's algorithm), so no timegm is needed.
    int mon1 = mi + 1;
    int y = year - (mon1 <= 2);
    int era = (y >= 0 ? y : y - 399) / 400;
    int yoe = y - era * 400;
    int mp = (mon1 + 9) % 12;
    int doy = (153 * mp + 2) / 5 + day - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    std::int64_t days = static_cast<std::int64_t>(era) * 146097 + doe - 719468;
    return days * 86400 + h * 3600 + m * 60 + s;
}

std::vector<Item> parse_multistatus(const std::string& xml) {
    std::vector<Item> out;
    Item current;
    bool in_response = false, in_resourcetype = false;
    std::string capture; // the element whose text is wanted
    std::size_t i = 0;
    while (i < xml.size()) {
        std::size_t lt = xml.find('<', i);
        if (lt == std::string::npos) break;
        if (!capture.empty()) {
            std::string text = xml_text(xml.substr(i, lt - i));
            if (capture == "href") current.href = href_path(trim(text));
            else if (capture == "getcontentlength") current.entry.size = std::strtoull(trim(text).c_str(), nullptr, 10);
            else if (capture == "getlastmodified") current.entry.mtime = parse_http_date(text);
            capture.clear();
        }
        if (xml.compare(lt, 4, "<!--") == 0) {
            std::size_t end = xml.find("-->", lt);
            i = end == std::string::npos ? xml.size() : end + 3;
            continue;
        }
        std::size_t gt = xml.find('>', lt);
        if (gt == std::string::npos) break;
        std::string tag = xml.substr(lt + 1, gt - lt - 1);
        i = gt + 1;
        if (tag.empty() || tag[0] == '?' || tag[0] == '!') continue;
        bool closing = tag[0] == '/';
        bool empty = !tag.empty() && tag.back() == '/';
        std::size_t start = closing ? 1 : 0;
        std::size_t end = tag.find_first_of(" \t\r\n/", start);
        std::string name = tag.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (std::size_t colon = name.find(':'); colon != std::string::npos) name = name.substr(colon + 1);
        name = lower(name);
        if (closing) {
            if (name == "response" && in_response) {
                out.push_back(current);
                in_response = false;
            } else if (name == "resourcetype") {
                in_resourcetype = false;
            }
            continue;
        }
        if (name == "response") {
            current = Item{};
            in_response = true;
        } else if (!in_response) {
            continue;
        } else if (name == "resourcetype") {
            in_resourcetype = !empty;
        } else if (name == "collection" && in_resourcetype) {
            current.entry.is_directory = true;
        } else if (!empty && (name == "href" || name == "getcontentlength" || name == "getlastmodified")) {
            capture = name;
        }
    }
    for (Item& it : out) {
        std::string p = without_slash(it.href);
        it.entry.name = p.substr(p.rfind('/') + 1);
        if (it.entry.is_directory) it.entry.size = 0;
    }
    return out;
}

} // namespace detail

namespace {

struct Response {
    int status = 0;
    std::vector<std::pair<std::string, std::string>> headers; // names in lower case
    std::string body;
    std::string header(const std::string& name) const {
        for (const auto& [n, v] : headers)
            if (n == name) return v;
        return "";
    }
};

class Client final : public netfs::Session {
public:
    explicit Client(netfs::Server server) : server_(std::move(server)) {}
    ~Client() override { drop(); }

    Status login();

    Result<std::uint64_t> size(const std::string& path) override;
    Result<std::vector<netfs::Entry>> list(const std::string& directory) override;
    Status make_directory(const std::string& path) override;
    Status rename(const std::string& from, const std::string& to) override;
    Status remove(const std::string& path) override;
    Status start_download(const std::string& path, std::uint64_t offset) override;
    Result<std::size_t> read(MutableBytes out) override;
    Status stop_download() override;
    Status start_upload(const std::string& path) override;
    Status write(Bytes data) override;
    Status finish_upload() override;

private:
    // The request target for a path on the server: the root, then the path.
    std::string target(const std::string& path, bool folder = false) const {
        std::string p = server_.root == "/" ? path : server_.root + (path == "/" ? "" : path);
        if (p.empty()) p = "/";
        if (folder && p.back() != '/') p += '/';
        return detail::encode_path(p);
    }
    std::string origin() const {
        return "http://" + server_.host + (server_.port == 80 ? "" : ":" + std::to_string(server_.port));
    }

    void drop() {
        if (socket_ != net::kNone) net::close(socket_);
        socket_ = net::kNone;
        buffer_.clear();
    }
    Status connect() {
        if (socket_ != net::kNone) return {};
        ALEMBIC_ASSIGN(socket_, netfs::tcp_connect(server_.host, server_.port, 10000, "WebDAV"));
        return {};
    }
    std::string authorization(const std::string& method, const std::string& uri);
    bool learn_auth(const Response& r); // from a 401; false when nothing new can be tried

    Status send_head(const std::string& method, const std::string& uri, const std::string& extra, std::size_t body_size, bool chunked);
    Result<Response> read_head();
    Status read_body(Response& r, bool head_only);
    Result<std::size_t> body_some(char* out, std::size_t size); // the streaming body; 0 at its end
    Result<std::string> line();

    // A whole request with a body in memory: retried once on a stale
    // kept-alive connection and once more after a 401 that taught a login.
    Result<Response> request(const std::string& method, const std::string& uri, const std::string& extra = "",
                             const std::string& body = "");

    netfs::Server server_;
    net::Socket socket_ = net::kNone;
    std::string buffer_; // received, not yet used

    enum class Auth { none, basic, digest } auth_ = Auth::none;
    std::string realm_, nonce_, opaque_, algorithm_;
    bool qop_auth_ = false;
    unsigned nonce_count_ = 0;

    // The streaming body of a GET (or the reply of a PUT).
    bool streaming_ = false, chunked_ = false, until_close_ = false, keep_ = true;
    std::uint64_t left_ = 0; // in the body, or in the current chunk
    bool body_done_ = false;
    bool uploading_ = false;
    std::string upload_path_;
};

std::string Client::authorization(const std::string& method, const std::string& uri) {
    if (auth_ == Auth::basic) return "Authorization: Basic " + base64(server_.user + ":" + server_.password) + "\r\n";
    if (auth_ != Auth::digest) return "";
    std::string ha1 = md5_hex(server_.user + ":" + realm_ + ":" + server_.password);
    std::string cnonce;
    if (lower(algorithm_) == "md5-sess") {
        std::uint8_t r[8] = {};
        (void)platform::secure_random(r, sizeof r);
        cnonce = netcrypto::hex(Bytes(r, 8));
        ha1 = md5_hex(ha1 + ":" + nonce_ + ":" + cnonce);
    }
    std::string ha2 = md5_hex(method + ":" + uri);
    std::string h = "Authorization: Digest username=\"" + server_.user + "\", realm=\"" + realm_ + "\", nonce=\"" + nonce_ +
                    "\", uri=\"" + uri + "\"";
    if (qop_auth_) {
        if (cnonce.empty()) {
            std::uint8_t r[8] = {};
            (void)platform::secure_random(r, sizeof r);
            cnonce = netcrypto::hex(Bytes(r, 8));
        }
        char nc[9];
        std::snprintf(nc, sizeof nc, "%08x", ++nonce_count_);
        h += ", qop=auth, nc=" + std::string(nc) + ", cnonce=\"" + cnonce + "\", response=\"" +
             md5_hex(ha1 + ":" + nonce_ + ":" + nc + ":" + cnonce + ":auth:" + ha2) + "\"";
    } else {
        h += ", response=\"" + md5_hex(ha1 + ":" + nonce_ + ":" + ha2) + "\"";
    }
    if (!algorithm_.empty()) h += ", algorithm=" + algorithm_;
    if (!opaque_.empty()) h += ", opaque=\"" + opaque_ + "\"";
    return h + "\r\n";
}

bool Client::learn_auth(const Response& r) {
    if (server_.user.empty()) return false;
    std::string digest, basic;
    for (const auto& [n, v] : r.headers) {
        if (n != "www-authenticate") continue;
        std::string scheme = lower(v.substr(0, v.find(' ')));
        if (scheme == "digest") digest = v.substr(v.find(' ') + 1);
        else if (scheme == "basic") basic = v;
    }
    if (!digest.empty()) {
        bool stale = false;
        std::string algorithm;
        bool qop = false;
        std::string realm, nonce, opaque;
        for (const auto& [n, v] : auth_params(digest)) {
            if (n == "realm") realm = v;
            else if (n == "nonce") nonce = v;
            else if (n == "opaque") opaque = v;
            else if (n == "algorithm") algorithm = v;
            else if (n == "stale") stale = lower(v) == "true";
            else if (n == "qop") {
                for (std::size_t at = 0; at <= v.size();) {
                    std::size_t comma = v.find(',', at);
                    if (lower(trim(v.substr(at, comma == std::string::npos ? std::string::npos : comma - at))) == "auth") qop = true;
                    if (comma == std::string::npos) break;
                    at = comma + 1;
                }
            }
        }
        std::string a = lower(algorithm);
        if (!a.empty() && a != "md5" && a != "md5-sess") {
            if (basic.empty()) return false; // SHA-256 Digest only: not supported
        } else {
            // A Digest answer was refused, and not for a stale nonce: the login is wrong.
            if (auth_ == Auth::digest && !stale) return false;
            auth_ = Auth::digest;
            realm_ = realm;
            nonce_ = nonce;
            opaque_ = opaque;
            algorithm_ = algorithm;
            qop_auth_ = qop;
            nonce_count_ = 0;
            return true;
        }
    }
    if (!basic.empty() && auth_ != Auth::basic) {
        auth_ = Auth::basic;
        return true;
    }
    return false;
}

Status Client::send_head(const std::string& method, const std::string& uri, const std::string& extra, std::size_t body_size,
                         bool chunked) {
    std::string head = method + " " + uri + " HTTP/1.1\r\nHost: " + server_.host +
                       (server_.port == 80 ? "" : ":" + std::to_string(server_.port)) +
                       "\r\nUser-Agent: Alembic\r\nConnection: keep-alive\r\n" + authorization(method, uri) + extra;
    if (chunked) head += "Transfer-Encoding: chunked\r\n";
    else if (body_size || method == "PUT" || method == "PROPFIND") head += "Content-Length: " + std::to_string(body_size) + "\r\n";
    head += "\r\n";
    return netfs::send_all(socket_, head.data(), head.size(), "WebDAV");
}

Result<std::string> Client::line() {
    for (;;) {
        std::size_t eol = buffer_.find('\n');
        if (eol != std::string::npos) {
            std::string l = buffer_.substr(0, eol);
            buffer_.erase(0, eol + 1);
            if (!l.empty() && l.back() == '\r') l.pop_back();
            return l;
        }
        if (buffer_.size() > 65536) return dav_error("a reply line is too long");
        char chunk[8192];
        long n = net::recv(socket_, chunk, sizeof chunk);
        if (n < 0 && net::should_retry()) continue;
        if (n <= 0) return dav_error("the server closed the connection");
        buffer_.append(chunk, static_cast<std::size_t>(n));
    }
}

Result<Response> Client::read_head() {
    for (;;) {
        Response r;
        ALEMBIC_ASSIGN(std::string status, line());
        if (status.compare(0, 5, "HTTP/") != 0) return dav_error("not an HTTP reply: " + status.substr(0, 40));
        std::size_t sp = status.find(' ');
        r.status = sp == std::string::npos ? 0 : std::atoi(status.c_str() + sp + 1);
        for (;;) {
            ALEMBIC_ASSIGN(std::string h, line());
            if (h.empty()) break;
            std::size_t colon = h.find(':');
            if (colon == std::string::npos) continue;
            r.headers.emplace_back(lower(trim(h.substr(0, colon))), trim(h.substr(colon + 1)));
        }
        if (r.status >= 100 && r.status < 200) continue; // 100 Continue
        return r;
    }
}

// Sets up the body of `r` for body_some(); with `head_only` (HEAD, 204, 304) there is none.
Status Client::read_body(Response& r, bool head_only) {
    streaming_ = true;
    body_done_ = false;
    chunked_ = lower(r.header("transfer-encoding")).find("chunked") != std::string::npos;
    std::string length = r.header("content-length");
    until_close_ = false;
    keep_ = lower(r.header("connection")) != "close";
    left_ = 0;
    if (head_only || r.status == 204 || r.status == 304) {
        body_done_ = true;
    } else if (chunked_) {
        left_ = 0; // a size line comes first
    } else if (!length.empty()) {
        left_ = std::strtoull(length.c_str(), nullptr, 10);
        body_done_ = left_ == 0;
    } else {
        until_close_ = true;
        keep_ = false;
    }
    return {};
}

Result<std::size_t> Client::body_some(char* out, std::size_t size) {
    for (;;) {
        if (body_done_) {
            streaming_ = false;
            if (!keep_) drop();
            return std::size_t{0};
        }
        if (chunked_ && left_ == 0) {
            ALEMBIC_ASSIGN(std::string l, line());
            if (l.empty()) { // the CRLF after the previous chunk
                ALEMBIC_ASSIGN(l, line());
            }
            left_ = std::strtoull(l.c_str(), nullptr, 16);
            if (left_ == 0) {
                for (;;) { // trailers, up to an empty line
                    ALEMBIC_ASSIGN(std::string t, line());
                    if (t.empty()) break;
                }
                body_done_ = true;
                continue;
            }
        }
        std::size_t want = until_close_ ? size : static_cast<std::size_t>(std::min<std::uint64_t>(size, left_));
        if (!buffer_.empty()) {
            std::size_t n = std::min(want, buffer_.size());
            std::memcpy(out, buffer_.data(), n);
            buffer_.erase(0, n);
            if (!until_close_) {
                left_ -= n;
                if (!chunked_ && left_ == 0) body_done_ = true;
            }
            return n;
        }
        long n = net::recv(socket_, out, want);
        if (n < 0 && net::should_retry()) continue;
        if (n < 0) return dav_error("download interrupted");
        if (n == 0) {
            if (until_close_) {
                body_done_ = true;
                continue;
            }
            return dav_error("the server closed the connection early");
        }
        if (!until_close_) {
            left_ -= static_cast<std::uint64_t>(n);
            if (!chunked_ && left_ == 0) body_done_ = true;
        }
        return static_cast<std::size_t>(n);
    }
}

Result<Response> Client::request(const std::string& method, const std::string& uri, const std::string& extra, const std::string& body) {
    if (streaming_) ALEMBIC_TRY(stop_download());
    for (int attempt = 0; attempt < 4; ++attempt) {
        const bool fresh = socket_ == net::kNone;
        ALEMBIC_TRY(connect());
        Status sent = send_head(method, uri, extra, body.size(), false);
        if (sent.ok() && !body.empty()) sent = netfs::send_all(socket_, body.data(), body.size(), "WebDAV");
        Result<Response> r = sent.ok() ? read_head() : Result<Response>(sent.error());
        if (!r.ok()) {
            drop();
            if (!fresh) continue; // a kept-alive connection the server had closed
            return r.error();
        }
        ALEMBIC_TRY(read_body(*r, method == "HEAD"));
        char chunk[16384];
        for (;;) {
            ALEMBIC_ASSIGN(std::size_t n, body_some(chunk, sizeof chunk));
            if (n == 0) break;
            if (r->body.size() < (8u << 20)) r->body.append(chunk, n);
        }
        if (r->status == 401 && learn_auth(*r)) continue;
        return r;
    }
    return dav_error("the server keeps refusing the login");
}

Status Client::login() {
    ALEMBIC_ASSIGN(Response r, request("PROPFIND", target("/", true), "Depth: 0\r\n"));
    if (r.status == 207) return {};
    if (r.status == 401) return dav_error(server_.user.empty() ? "the server asks for a user name and password" : "wrong user name or password");
    if (r.status == 404) return dav_error("no such folder on the server: " + server_.root);
    if (r.status == 405 || r.status == 501) return dav_error("the server does not answer WebDAV requests at " + server_.root);
    if (r.status >= 300 && r.status < 400) {
        std::string where = r.header("location");
        if (where.rfind("https:", 0) == 0) return dav_error("the server moves to HTTPS, which Alembic does not support yet");
        return dav_error("the server sends this folder elsewhere: " + where);
    }
    return http_error("cannot open " + server_.root, r.status);
}

static const char kPropfindBody[] =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
    "<D:propfind xmlns:D=\"DAV:\"><D:prop><D:resourcetype/><D:getcontentlength/><D:getlastmodified/></D:prop></D:propfind>";

Result<std::uint64_t> Client::size(const std::string& path) {
    ALEMBIC_ASSIGN(Response r, request("PROPFIND", target(path), "Depth: 0\r\nContent-Type: application/xml\r\n", kPropfindBody));
    if (r.status == 301 || r.status == 302) return make_error(Errc::invalid_argument, "WebDAV: " + path + " is a folder");
    if (r.status != 207) return http_error("cannot read " + path, r.status);
    auto items = detail::parse_multistatus(r.body);
    if (items.empty()) return dav_error("no answer for " + path);
    if (items.front().entry.is_directory) return make_error(Errc::invalid_argument, "WebDAV: " + path + " is a folder");
    return items.front().entry.size;
}

Result<std::vector<netfs::Entry>> Client::list(const std::string& directory) {
    ALEMBIC_ASSIGN(Response r, request("PROPFIND", target(directory, true), "Depth: 1\r\nContent-Type: application/xml\r\n", kPropfindBody));
    if (r.status != 207) return http_error("cannot list " + directory, r.status);
    std::string self = without_slash(detail::decode(target(directory, true)));
    std::vector<netfs::Entry> out;
    for (auto& item : detail::parse_multistatus(r.body)) {
        if (without_slash(item.href) == self || item.entry.name.empty()) continue;
        out.push_back(std::move(item.entry));
    }
    return out;
}

Status Client::make_directory(const std::string& path) {
    ALEMBIC_ASSIGN(Response r, request("MKCOL", target(path, true)));
    if (r.status == 201 || r.status == 200) return {};
    return http_error("cannot create " + path, r.status);
}

Status Client::rename(const std::string& from, const std::string& to) {
    ALEMBIC_ASSIGN(Response r, request("MOVE", target(from), "Destination: " + origin() + target(to) + "\r\nOverwrite: F\r\n"));
    if (r.status == 201 || r.status == 204 || r.status == 200) return {};
    return http_error("cannot rename " + from, r.status);
}

Status Client::remove(const std::string& path) {
    ALEMBIC_ASSIGN(Response r, request("DELETE", target(path)));
    if (r.status == 200 || r.status == 204 || r.status == 202) return {};
    return http_error("cannot delete " + path, r.status);
}

Status Client::start_download(const std::string& path, std::uint64_t offset) {
    if (streaming_) ALEMBIC_TRY(stop_download());
    const std::string uri = target(path);
    for (int attempt = 0; attempt < 4; ++attempt) {
        const bool fresh = socket_ == net::kNone;
        ALEMBIC_TRY(connect());
        Status sent = send_head("GET", uri, offset ? "Range: bytes=" + std::to_string(offset) + "-\r\n" : "", 0, false);
        Result<Response> r = sent.ok() ? read_head() : Result<Response>(sent.error());
        if (!r.ok()) {
            drop();
            if (!fresh) continue;
            return r.error();
        }
        if (r->status == 416) { // from the end of the file: nothing to read
            ALEMBIC_TRY(read_body(*r, false));
            char sink[4096];
            for (;;) {
                ALEMBIC_ASSIGN(std::size_t n, body_some(sink, sizeof sink));
                if (n == 0) break;
            }
            streaming_ = true;
            body_done_ = true;
            return {};
        }
        if (r->status == 401) {
            ALEMBIC_TRY(read_body(*r, false));
            char sink[4096];
            for (;;) {
                ALEMBIC_ASSIGN(std::size_t n, body_some(sink, sizeof sink));
                if (n == 0) break;
            }
            if (learn_auth(*r)) continue;
            return http_error("cannot read " + path, 401);
        }
        if (r->status == 200 && offset) {
            drop();
            return dav_error("the server cannot read from an offset (no Range support)");
        }
        if (r->status != 200 && r->status != 206) {
            drop();
            return http_error("cannot read " + path, r->status);
        }
        return read_body(*r, false);
    }
    return dav_error("the server keeps refusing the login");
}

Result<std::size_t> Client::read(MutableBytes out) {
    if (!streaming_) return std::size_t{0};
    return body_some(reinterpret_cast<char*>(out.data()), out.size());
}

Status Client::stop_download() {
    if (!streaming_) return {};
    streaming_ = false;
    if (!body_done_) drop(); // the rest of the body is not wanted: a new connection is cheaper
    return {};
}

Status Client::start_upload(const std::string& path) {
    if (streaming_) ALEMBIC_TRY(stop_download());
    ALEMBIC_TRY(connect());
    upload_path_ = path;
    Status s = send_head("PUT", target(path), "Content-Type: application/octet-stream\r\n", 0, true);
    if (!s.ok()) {
        // A kept-alive connection may have been closed by the server: once more on a new one.
        drop();
        ALEMBIC_TRY(connect());
        ALEMBIC_TRY(send_head("PUT", target(path), "Content-Type: application/octet-stream\r\n", 0, true));
    }
    uploading_ = true;
    return {};
}

Status Client::write(Bytes data) {
    if (!uploading_) return dav_error("no upload is open");
    if (data.empty()) return {};
    char size[24];
    std::snprintf(size, sizeof size, "%zx\r\n", data.size());
    ALEMBIC_TRY(netfs::send_all(socket_, size, std::strlen(size), "WebDAV"));
    ALEMBIC_TRY(netfs::send_all(socket_, data.data(), data.size(), "WebDAV"));
    return netfs::send_all(socket_, "\r\n", 2, "WebDAV");
}

Status Client::finish_upload() {
    if (!uploading_) return {};
    uploading_ = false;
    Status s = netfs::send_all(socket_, "0\r\n\r\n", 5, "WebDAV");
    if (!s.ok()) {
        drop();
        return s;
    }
    auto r = read_head();
    if (!r.ok()) {
        drop();
        return r.error();
    }
    ALEMBIC_TRY(read_body(*r, false));
    char sink[4096];
    for (;;) {
        auto n = body_some(sink, sizeof sink);
        if (!n.ok()) {
            drop();
            break;
        }
        if (*n == 0) break;
    }
    if (r->status == 200 || r->status == 201 || r->status == 204) return {};
    if (r->status == 411) return dav_error("the server does not take uploads of unknown length (chunked transfer coding)");
    return http_error("cannot write " + upload_path_, r->status);
}

} // namespace

Result<std::unique_ptr<netfs::Session>> open(const netfs::Server& server) {
    if (server.host.empty()) return dav_error("no address");
    std::unique_ptr<Client> c(new Client(server));
    ALEMBIC_TRY(c->login());
    return std::unique_ptr<netfs::Session>(std::move(c));
}

} // namespace alembic::engine::webdav
