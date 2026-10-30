// SPDX-License-Identifier: GPL-3.0-or-later
#include "nfs.hpp"

#include "net.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <optional>
#include <set>

namespace alembic::engine::nfs {

namespace {

// Programs, versions and procedures (RFC 1833, RFC 1813).
constexpr std::uint32_t kPortmapper = 100000, kMount = 100005, kNfs = 100003;
constexpr std::uint32_t kGetPort = 3;
constexpr std::uint32_t kMnt = 1, kExport = 5;
enum Proc : std::uint32_t {
    kGetattr = 1,
    kLookup = 3,
    kRead = 6,
    kWrite = 7,
    kCreate = 8,
    kMkdir = 9,
    kRemove = 12,
    kRename = 14,
    kReaddir = 16,
    kReaddirplus = 17,
    kFsinfo = 19,
    kCommit = 21,
};
constexpr std::uint32_t kTypeDirectory = 2;
constexpr int kDepth = 4; // reads or writes in flight

const char* status_name(std::uint32_t s) {
    switch (s) {
    case 1: return "not allowed (the export maps this uid:gid to a user without rights)";
    case 2: return "no such file or folder";
    case 5: return "input/output error on the server";
    case 6: return "no such device";
    case 13: return "access denied";
    case 17: return "it already exists";
    case 18: return "cannot move across file systems";
    case 20: return "not a folder";
    case 21: return "it is a folder";
    case 22: return "invalid argument";
    case 27: return "the file is too large";
    case 28: return "the disk is full";
    case 30: return "the export is read-only";
    case 63: return "the name is too long";
    case 66: return "the folder is not empty";
    case 69: return "over the quota";
    case 70: return "stale file handle (moved or deleted on the server)";
    case 10001: return "bad file handle";
    case 10004: return "not supported by the server";
    case 10006: return "server fault";
    case 10008: return "the server is busy, try again";
    default: return nullptr;
    }
}

Error nfs_error(const std::string& what) { return make_error(Errc::io, "NFS: " + what); }
Error nfs_status(const std::string& what, std::uint32_t status) {
    const char* name = status_name(status);
    return make_error(status == 2 ? Errc::invalid_argument : Errc::io,
                      "NFS: " + what + ": " + (name ? std::string(name) : "error " + std::to_string(status)));
}

// XDR (RFC 4506): big-endian, everything padded to 4 bytes.
struct Xdr {
    std::vector<std::uint8_t> b;
    Xdr& u32(std::uint32_t v) {
        for (int i = 3; i >= 0; --i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
        return *this;
    }
    Xdr& u64(std::uint64_t v) { return u32(static_cast<std::uint32_t>(v >> 32)).u32(static_cast<std::uint32_t>(v)); }
    Xdr& opaque(const void* p, std::size_t n) {
        u32(static_cast<std::uint32_t>(n));
        return fixed(p, n);
    }
    Xdr& fixed(const void* p, std::size_t n) {
        b.insert(b.end(), static_cast<const std::uint8_t*>(p), static_cast<const std::uint8_t*>(p) + n);
        while (b.size() % 4) b.push_back(0);
        return *this;
    }
    Xdr& str(const std::string& s) { return opaque(s.data(), s.size()); }
    Xdr& fh(const std::vector<std::uint8_t>& h) { return opaque(h.data(), h.size()); }
};

struct In {
    const std::vector<std::uint8_t>& b;
    std::size_t at = 0;
    bool bad = false;
    std::uint32_t u32() {
        if (at + 4 > b.size()) {
            bad = true;
            at = b.size();
            return 0;
        }
        std::uint32_t v = (std::uint32_t(b[at]) << 24) | (std::uint32_t(b[at + 1]) << 16) | (std::uint32_t(b[at + 2]) << 8) | b[at + 3];
        at += 4;
        return v;
    }
    std::uint64_t u64() {
        std::uint64_t hi = u32();
        return (hi << 32) | u32();
    }
    void skip(std::size_t n) {
        n = (n + 3) & ~std::size_t{3};
        if (at + n > b.size()) {
            bad = true;
            at = b.size();
        } else {
            at += n;
        }
    }
    std::vector<std::uint8_t> opaque(std::size_t max = 1u << 20) {
        std::uint32_t n = u32();
        if (bad || n > max || at + n > b.size()) {
            bad = true;
            return {};
        }
        std::vector<std::uint8_t> v(b.begin() + at, b.begin() + at + n);
        skip(n);
        return v;
    }
    std::string str() {
        auto v = opaque(4096);
        return std::string(v.begin(), v.end());
    }
};

struct Attr {
    std::uint32_t type = 0;
    std::uint64_t size = 0;
    std::int64_t mtime = 0;
};
Attr fattr3(In& in) {
    Attr a;
    a.type = in.u32();
    in.u32(); // mode
    in.u32(); // nlink
    in.u32(); // uid
    in.u32(); // gid
    a.size = in.u64();
    in.u64(); // used
    in.u64(); // rdev
    in.u64(); // fsid
    in.u64(); // fileid
    in.u64(); // atime
    a.mtime = in.u32();
    in.u32();
    in.u64(); // ctime
    return a;
}
// post_op_attr
bool post_op_attr(In& in, Attr* a) {
    if (!in.u32()) return false;
    Attr x = fattr3(in);
    if (a) *a = x;
    return true;
}
void wcc_data(In& in) {
    if (in.u32()) in.skip(24); // pre_op_attr
    post_op_attr(in, nullptr);
}

// One TCP connection to an RPC program, with record marking. Calls carry
// AUTH_SYS; replies are matched to calls by xid, so several can be in flight.
class Rpc {
public:
    ~Rpc() {
        if (socket_ != net::kNone) net::close(socket_);
    }
    Status open(const std::string& host, std::uint16_t port, std::uint32_t program, std::uint32_t version, std::uint32_t uid,
                std::uint32_t gid, int timeout_ms = 10000) {
        program_ = program;
        version_ = version;
        uid_ = uid;
        gid_ = gid;
        ALEMBIC_ASSIGN(socket_, netfs::tcp_connect(host, port, timeout_ms, "NFS", nullptr, true));
        return {};
    }
    Result<std::uint32_t> send(std::uint32_t proc, const Xdr& args) {
        const std::uint32_t xid = next_xid_++;
        Xdr m;
        m.u32(xid).u32(0 /* call */).u32(2).u32(program_).u32(version_).u32(proc);
        if (program_ == kPortmapper) {
            m.u32(0).u32(0); // AUTH_NONE
        } else {
            Xdr cred;
            cred.u32(0).str("alembic").u32(uid_).u32(gid_).u32(1).u32(gid_);
            m.u32(1).opaque(cred.b.data(), cred.b.size());
        }
        m.u32(0).u32(0); // verifier: AUTH_NONE
        m.b.insert(m.b.end(), args.b.begin(), args.b.end());
        const std::uint32_t mark = 0x80000000u | static_cast<std::uint32_t>(m.b.size());
        std::uint8_t head[4] = {static_cast<std::uint8_t>(mark >> 24), static_cast<std::uint8_t>(mark >> 16),
                                static_cast<std::uint8_t>(mark >> 8), static_cast<std::uint8_t>(mark)};
        ALEMBIC_TRY(netfs::send_all(socket_, head, 4, "NFS"));
        ALEMBIC_TRY(netfs::send_all(socket_, m.b.data(), m.b.size(), "NFS"));
        return xid;
    }
    // The results of call `xid` (after the accepted-reply header).
    Result<std::vector<std::uint8_t>> wait(std::uint32_t xid) {
        if (auto it = early_.find(xid); it != early_.end()) {
            auto v = std::move(it->second);
            early_.erase(it);
            return v;
        }
        for (;;) {
            std::vector<std::uint8_t> record;
            for (bool last = false; !last;) {
                std::uint8_t head[4];
                ALEMBIC_TRY(netfs::recv_all(socket_, head, 4, "NFS"));
                const std::uint32_t mark = (std::uint32_t(head[0]) << 24) | (std::uint32_t(head[1]) << 16) | (std::uint32_t(head[2]) << 8) | head[3];
                last = mark & 0x80000000u;
                const std::size_t n = mark & 0x7fffffffu;
                if (n > (64u << 20)) return nfs_error("reply too large");
                const std::size_t at = record.size();
                record.resize(at + n);
                ALEMBIC_TRY(netfs::recv_all(socket_, record.data() + at, n, "NFS"));
            }
            In in{record};
            const std::uint32_t id = in.u32();
            if (in.u32() != 1) continue; // not a reply
            if (in.u32() != 0) { // denied
                const std::uint32_t why = in.u32();
                if (why == 1) return nfs_error("the server refused the login (AUTH_SYS " + std::to_string(in.u32()) + ")");
                return nfs_error("the server does not speak this RPC version");
            }
            in.u32();                 // verifier flavour
            in.opaque(400);           // verifier body
            const std::uint32_t accept = in.u32();
            if (in.bad) return nfs_error("malformed reply");
            if (accept != 0) {
                if (id != xid) continue;
                return nfs_error(accept == 1   ? "the program is not available on this port"
                                 : accept == 2 ? "the server does not speak NFS version 3"
                                 : accept == 3 ? "procedure not available"
                                               : "the server could not decode a request");
            }
            std::vector<std::uint8_t> results(record.begin() + in.at, record.end());
            if (id == xid) return results;
            early_[id] = std::move(results);
        }
    }
    Result<std::vector<std::uint8_t>> call(std::uint32_t proc, const Xdr& args) {
        ALEMBIC_ASSIGN(std::uint32_t xid, send(proc, args));
        return wait(xid);
    }

private:
    net::Socket socket_ = net::kNone;
    std::uint32_t program_ = 0, version_ = 0, uid_ = 0, gid_ = 0;
    std::uint32_t next_xid_ = 0x41420000;
    std::map<std::uint32_t, std::vector<std::uint8_t>> early_;
};

std::vector<std::string> split(const std::string& path) {
    std::vector<std::string> parts;
    for (std::size_t at = 0; at < path.size();) {
        std::size_t slash = path.find('/', at);
        std::string part = path.substr(at, slash == std::string::npos ? std::string::npos : slash - at);
        at = slash == std::string::npos ? path.size() : slash + 1;
        if (!part.empty() && part != ".") parts.push_back(part);
    }
    return parts;
}
std::string joined(const std::vector<std::string>& parts, std::size_t from = 0, std::size_t to = std::string::npos) {
    std::string s;
    for (std::size_t i = from; i < std::min(to, parts.size()); ++i) s += "/" + parts[i];
    return s.empty() ? "/" : s;
}

} // namespace

namespace detail {

std::vector<std::string> export_children(const std::vector<std::string>& exports, const std::string& directory) {
    const auto dir = split(directory);
    std::set<std::string> names;
    for (const std::string& e : exports) {
        const auto parts = split(e);
        if (parts.size() <= dir.size() || !std::equal(dir.begin(), dir.end(), parts.begin())) continue;
        names.insert(parts[dir.size()]);
    }
    return {names.begin(), names.end()};
}

std::string export_of(const std::vector<std::string>& exports, const std::string& path) {
    const auto p = split(path);
    std::string best;
    std::size_t best_len = 0;
    for (const std::string& e : exports) {
        const auto parts = split(e);
        if (parts.size() > p.size() || !std::equal(parts.begin(), parts.end(), p.begin())) continue;
        if (best.empty() || parts.size() > best_len) {
            best = joined(parts);
            best_len = parts.size();
        }
    }
    return best;
}

} // namespace detail

namespace {

class Client final : public netfs::Session {
public:
    static Result<std::unique_ptr<Client>> connect(const netfs::Server& server);

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
    using Handle = std::vector<std::uint8_t>;
    struct Found {
        Handle fh;
        Attr attr;
        bool has_attr = false;
    };

    Result<std::uint16_t> mount_port();
    Result<Handle> mount(const std::string& export_path);
    Result<std::vector<std::string>> exports();
    // The export a server path is in, and the path inside it.
    Result<std::pair<std::string, std::string>> place(const std::string& path);
    Result<Found> lookup(const std::string& path);
    Result<std::pair<Handle, std::string>> parent(const std::string& path); // the folder's handle and the name
    Result<Attr> getattr(const Handle& fh);
    Status read_limits(const Handle& root);

    std::string host_;
    std::uint16_t port_ = 2049;
    std::uint32_t uid_ = 0, gid_ = 0;
    std::string root_; // the export of the settings; "" for all of them
    Rpc nfs_;
    std::optional<std::uint16_t> mount_port_;
    std::optional<std::vector<std::string>> exports_;
    std::map<std::string, Handle> roots_;  // mounted exports
    std::map<std::string, Handle> cache_;  // looked-up paths (server paths)
    std::uint32_t rsize_ = 65536, wsize_ = 65536;
    bool limits_read_ = false;

    // The open download.
    bool reading_ = false;
    Handle read_fh_;
    std::uint64_t read_size_ = 0, read_next_ = 0;
    std::deque<std::uint32_t> reads_;
    std::vector<std::uint8_t> read_data_;
    std::size_t read_at_ = 0;
    bool read_eof_ = false;
    Status fill_reads();

    // The open upload.
    bool writing_ = false;
    Handle write_fh_;
    std::string write_path_;
    std::uint64_t write_offset_ = 0;
    std::deque<std::uint32_t> writes_;
    std::vector<std::uint8_t> write_buffer_;
    std::vector<std::uint8_t> verifier_;
    bool verifier_changed_ = false;
    Status send_write(const std::uint8_t* data, std::size_t size);
    Status finish_write(std::uint32_t xid);
};

Result<std::uint16_t> Client::mount_port() {
    if (mount_port_) return *mount_port_;
    // The portmapper's answer, else the NFS port itself (servers without one).
    std::uint16_t port = port_;
    Rpc pm;
    if (pm.open(host_, 111, kPortmapper, 2, 0, 0, 3000).ok()) {
        Xdr a;
        a.u32(kMount).u32(3).u32(6 /* TCP */).u32(0);
        auto r = pm.call(kGetPort, a);
        if (r.ok()) {
            In in{*r};
            std::uint32_t p = in.u32();
            if (!in.bad && p > 0 && p < 65536) port = static_cast<std::uint16_t>(p);
        }
    }
    mount_port_ = port;
    return port;
}

Result<Client::Handle> Client::mount(const std::string& export_path) {
    if (auto it = roots_.find(export_path); it != roots_.end()) return it->second;
    ALEMBIC_ASSIGN(std::uint16_t port, mount_port());
    Rpc m;
    ALEMBIC_TRY(m.open(host_, port, kMount, 3, uid_, gid_));
    Xdr a;
    a.str(export_path);
    ALEMBIC_ASSIGN(auto r, m.call(kMnt, a));
    In in{r};
    const std::uint32_t status = in.u32();
    if (status == 13 || status == 1)
        return nfs_error("the server does not let this console mount " + export_path +
                         " (check the export's allowed addresses; exports marked \"secure\" need a port below 1024)");
    if (status != 0) return nfs_status("cannot mount " + export_path, status);
    Handle fh = in.opaque(64);
    if (in.bad || fh.empty()) return nfs_error("malformed MOUNT reply");
    roots_[export_path] = fh;
    return fh;
}

Result<std::vector<std::string>> Client::exports() {
    if (exports_) return *exports_;
    std::vector<std::string> list;
    ALEMBIC_ASSIGN(std::uint16_t port, mount_port());
    Rpc m;
    ALEMBIC_TRY(m.open(host_, port, kMount, 3, uid_, gid_));
    auto r = m.call(kExport, Xdr());
    if (r.ok()) {
        In in{*r};
        while (!in.bad && in.u32()) {
            std::string dir = in.str();
            while (!in.bad && in.u32()) in.str(); // groups
            if (!in.bad && !dir.empty() && dir.front() == '/') list.push_back(dir);
        }
    }
    // A server that lists nothing (go-nfs, rclone) serves one tree at "/".
    if (list.empty()) list.push_back("/");
    exports_ = list;
    return list;
}

Result<std::pair<std::string, std::string>> Client::place(const std::string& path) {
    if (!root_.empty()) return std::make_pair(root_, joined(split(path)));
    ALEMBIC_ASSIGN(auto all, exports());
    std::string e = detail::export_of(all, path);
    if (e.empty()) return make_error(Errc::invalid_argument, "NFS: " + path + " is above the exports");
    const auto ep = split(e), p = split(path);
    return std::make_pair(e, joined(p, ep.size()));
}

Status Client::read_limits(const Handle& root) {
    if (limits_read_) return {};
    limits_read_ = true;
    Xdr a;
    a.fh(root);
    auto r = nfs_.call(kFsinfo, a);
    if (!r.ok()) return {};
    In in{*r};
    if (in.u32() != 0) return {};
    post_op_attr(in, nullptr);
    std::uint32_t rtmax = in.u32(), rtpref = in.u32();
    in.u32();
    std::uint32_t wtmax = in.u32(), wtpref = in.u32();
    if (in.bad) return {};
    // As much as the server takes per request, up to 1 MiB.
    auto pick = [](std::uint32_t max, std::uint32_t pref) {
        const std::uint32_t v = max ? max : pref;
        return std::clamp<std::uint32_t>(v, 4096, 1u << 20);
    };
    rsize_ = pick(rtmax, rtpref);
    wsize_ = pick(wtmax, wtpref);
    return {};
}

Result<Attr> Client::getattr(const Handle& fh) {
    Xdr a;
    a.fh(fh);
    ALEMBIC_ASSIGN(auto r, nfs_.call(kGetattr, a));
    In in{r};
    const std::uint32_t status = in.u32();
    if (status != 0) return nfs_status("cannot read attributes", status);
    Attr at = fattr3(in);
    if (in.bad) return nfs_error("malformed GETATTR reply");
    return at;
}

Result<Client::Found> Client::lookup(const std::string& path) {
    ALEMBIC_ASSIGN(auto where, place(path));
    ALEMBIC_ASSIGN(Handle root, mount(where.first));
    ALEMBIC_TRY(read_limits(root));
    const auto parts = split(where.second);
    Found f;
    f.fh = root;
    std::size_t i = 0;
    // Start from the deepest folder already looked up.
    for (std::size_t n = parts.size(); n > 0; --n) {
        auto it = cache_.find(where.first + "|" + joined(parts, 0, n));
        if (it != cache_.end()) {
            f.fh = it->second;
            i = n;
            break;
        }
    }
    for (; i < parts.size(); ++i) {
        Xdr a;
        a.fh(f.fh).str(parts[i]);
        ALEMBIC_ASSIGN(auto r, nfs_.call(kLookup, a));
        In in{r};
        const std::uint32_t status = in.u32();
        if (status != 0) return nfs_status("cannot open " + path, status);
        f.fh = in.opaque(64);
        f.has_attr = post_op_attr(in, &f.attr);
        if (in.bad) return nfs_error("malformed LOOKUP reply");
        if (i + 1 < parts.size()) cache_[where.first + "|" + joined(parts, 0, i + 1)] = f.fh; // folders only
    }
    if (!f.has_attr) {
        ALEMBIC_ASSIGN(f.attr, getattr(f.fh));
        f.has_attr = true;
    }
    return f;
}

Result<std::pair<Client::Handle, std::string>> Client::parent(const std::string& path) {
    const auto parts = split(path);
    if (parts.empty()) return make_error(Errc::invalid_argument, "NFS: no name");
    ALEMBIC_ASSIGN(Found dir, lookup(joined(parts, 0, parts.size() - 1)));
    if (dir.attr.type != kTypeDirectory) return make_error(Errc::invalid_argument, "NFS: not a folder: " + joined(parts, 0, parts.size() - 1));
    return std::make_pair(dir.fh, parts.back());
}

Result<std::unique_ptr<Client>> Client::connect(const netfs::Server& server) {
    std::unique_ptr<Client> c(new Client());
    c->host_ = server.host;
    c->port_ = server.port;
    c->root_ = server.root;
    if (!server.user.empty()) {
        unsigned long uid = 0, gid = 0;
        if (std::sscanf(server.user.c_str(), "%lu:%lu", &uid, &gid) >= 1) {
            c->uid_ = static_cast<std::uint32_t>(uid);
            c->gid_ = static_cast<std::uint32_t>(gid);
        }
    }
    ALEMBIC_TRY(c->nfs_.open(server.host, server.port, kNfs, 3, c->uid_, c->gid_));
    // Mounting checks the export and the login; without an export, listing them does.
    if (!c->root_.empty()) {
        ALEMBIC_ASSIGN(Handle root, c->mount(c->root_));
        auto attr = c->getattr(root);
        if (!attr.ok()) return attr.error();
    } else {
        ALEMBIC_ASSIGN(auto all, c->exports());
        (void)all;
    }
    return c;
}

Result<std::uint64_t> Client::size(const std::string& path) {
    if (root_.empty()) {
        ALEMBIC_ASSIGN(auto all, exports());
        if (detail::export_of(all, path).empty()) return make_error(Errc::invalid_argument, "NFS: " + path + " is a folder");
    }
    ALEMBIC_ASSIGN(Found f, lookup(path));
    if (f.attr.type == kTypeDirectory) return make_error(Errc::invalid_argument, "NFS: " + path + " is a folder");
    return f.attr.size;
}

Result<std::vector<netfs::Entry>> Client::list(const std::string& directory) {
    std::vector<netfs::Entry> out;
    if (root_.empty()) {
        ALEMBIC_ASSIGN(auto all, exports());
        if (detail::export_of(all, directory).empty()) {
            // Above the exports: the next names on the way to them.
            for (const std::string& name : detail::export_children(all, directory)) {
                netfs::Entry e;
                e.name = name;
                e.is_directory = true;
                out.push_back(std::move(e));
            }
            return out;
        }
    }
    ALEMBIC_ASSIGN(Found dir, lookup(directory));
    if (dir.attr.type != kTypeDirectory) return make_error(Errc::invalid_argument, "NFS: not a folder: " + directory);
    std::uint64_t cookie = 0;
    std::uint8_t verifier[8] = {};
    bool plus = true;
    for (int pages = 0; pages < 100000; ++pages) {
        Xdr a;
        a.fh(dir.fh).u64(cookie).fixed(verifier, 8);
        if (plus) a.u32(32768).u32(256 * 1024);
        else a.u32(64 * 1024);
        ALEMBIC_ASSIGN(auto r, nfs_.call(plus ? kReaddirplus : kReaddir, a));
        In in{r};
        const std::uint32_t status = in.u32();
        if (status == 10004 && plus) { // READDIRPLUS not supported: READDIR, then GETATTR
            plus = false;
            --pages;
            continue;
        }
        if (status != 0) return nfs_status("cannot list " + directory, status);
        post_op_attr(in, nullptr);
        for (int i = 0; i < 8; ++i) verifier[i] = in.at + i < r.size() ? r[in.at + i] : 0;
        in.skip(8);
        while (!in.bad && in.u32()) {
            in.u64(); // fileid
            netfs::Entry e;
            e.name = in.str();
            cookie = in.u64();
            Attr at;
            bool has = false;
            Handle fh;
            if (plus) {
                has = post_op_attr(in, &at);
                if (in.u32()) fh = in.opaque(64);
            }
            if (in.bad) break;
            if (e.name == "." || e.name == "..") continue;
            if (!has) {
                if (fh.empty()) {
                    Xdr l;
                    l.fh(dir.fh).str(e.name);
                    auto lr = nfs_.call(kLookup, l);
                    if (lr.ok()) {
                        In li{*lr};
                        if (li.u32() == 0) {
                            fh = li.opaque(64);
                            has = post_op_attr(li, &at);
                        }
                    }
                }
                if (!has && !fh.empty()) {
                    auto ga = getattr(fh);
                    if (ga.ok()) {
                        at = *ga;
                        has = true;
                    }
                }
            }
            e.is_directory = at.type == kTypeDirectory;
            e.size = e.is_directory ? 0 : at.size;
            e.mtime = at.mtime;
            out.push_back(std::move(e));
        }
        const bool eof = in.u32();
        if (in.bad) return nfs_error("malformed READDIR reply");
        if (eof) break;
    }
    return out;
}

Status Client::make_directory(const std::string& path) {
    ALEMBIC_ASSIGN(auto where, parent(path));
    Xdr a;
    a.fh(where.first).str(where.second);
    a.u32(1).u32(0755).u32(0).u32(0).u32(0).u32(0).u32(0); // mode 0755; uid, gid, size, times left alone
    ALEMBIC_ASSIGN(auto r, nfs_.call(kMkdir, a));
    In in{r};
    const std::uint32_t status = in.u32();
    if (status != 0) return nfs_status("cannot create " + path, status);
    return {};
}

Status Client::rename(const std::string& from, const std::string& to) {
    ALEMBIC_ASSIGN(auto a, parent(from));
    ALEMBIC_ASSIGN(auto b, parent(to));
    Xdr x;
    x.fh(a.first).str(a.second).fh(b.first).str(b.second);
    ALEMBIC_ASSIGN(auto r, nfs_.call(kRename, x));
    In in{r};
    const std::uint32_t status = in.u32();
    cache_.clear();
    if (status != 0) return nfs_status("cannot rename " + from, status);
    return {};
}

Status Client::remove(const std::string& path) {
    ALEMBIC_ASSIGN(auto where, parent(path));
    Xdr a;
    a.fh(where.first).str(where.second);
    ALEMBIC_ASSIGN(auto r, nfs_.call(kRemove, a));
    In in{r};
    const std::uint32_t status = in.u32();
    cache_.clear();
    if (status != 0) return nfs_status("cannot delete " + path, status);
    return {};
}

// ---------------------------------------------------------------- reading

Status Client::fill_reads() {
    while (reads_.size() < kDepth && read_next_ < read_size_) {
        const std::uint32_t n = static_cast<std::uint32_t>(std::min<std::uint64_t>(rsize_, read_size_ - read_next_));
        Xdr a;
        a.fh(read_fh_).u64(read_next_).u32(n);
        ALEMBIC_ASSIGN(std::uint32_t xid, nfs_.send(kRead, a));
        reads_.push_back(xid);
        read_next_ += n;
    }
    return {};
}

Status Client::start_download(const std::string& path, std::uint64_t offset) {
    if (reading_) ALEMBIC_TRY(stop_download());
    ALEMBIC_ASSIGN(Found f, lookup(path));
    if (f.attr.type == kTypeDirectory) return make_error(Errc::invalid_argument, "NFS: " + path + " is a folder");
    reading_ = true;
    read_fh_ = f.fh;
    read_size_ = f.attr.size;
    read_next_ = std::min(offset, read_size_);
    read_data_.clear();
    read_at_ = 0;
    read_eof_ = false;
    return fill_reads();
}

Result<std::size_t> Client::read(MutableBytes out) {
    if (!reading_) return std::size_t{0};
    while (read_at_ >= read_data_.size()) {
        if (reads_.empty() || read_eof_) return std::size_t{0};
        const std::uint32_t xid = reads_.front();
        reads_.pop_front();
        ALEMBIC_ASSIGN(auto r, nfs_.wait(xid));
        In in{r};
        const std::uint32_t status = in.u32();
        if (status != 0) return nfs_status("reading failed", status);
        post_op_attr(in, nullptr);
        in.u32(); // count
        const bool eof = in.u32();
        read_data_ = in.opaque(64u << 20);
        if (in.bad) return nfs_error("malformed READ reply");
        read_at_ = 0;
        if (eof && read_data_.empty()) read_eof_ = true; // the file got shorter
        ALEMBIC_TRY(fill_reads());
    }
    const std::size_t n = std::min(out.size(), read_data_.size() - read_at_);
    std::memcpy(out.data(), read_data_.data() + read_at_, n);
    read_at_ += n;
    return n;
}

Status Client::stop_download() {
    if (!reading_) return {};
    reading_ = false;
    while (!reads_.empty()) {
        const std::uint32_t xid = reads_.front();
        reads_.pop_front();
        ALEMBIC_ASSIGN(auto r, nfs_.wait(xid));
        (void)r;
    }
    read_data_.clear();
    read_at_ = 0;
    return {};
}

// ---------------------------------------------------------------- writing

Status Client::start_upload(const std::string& path) {
    if (reading_) ALEMBIC_TRY(stop_download());
    if (writing_) ALEMBIC_TRY(finish_upload());
    ALEMBIC_ASSIGN(auto where, parent(path));
    // CREATE UNCHECKED, mode 0644 and size 0: a new file, or an old one emptied.
    Xdr a;
    a.fh(where.first).str(where.second).u32(0);
    a.u32(1).u32(0644).u32(0).u32(0).u32(1).u64(0).u32(0).u32(0);
    ALEMBIC_ASSIGN(auto r, nfs_.call(kCreate, a));
    In in{r};
    const std::uint32_t status = in.u32();
    if (status != 0) return nfs_status("cannot write " + path, status);
    Handle fh;
    if (in.u32()) fh = in.opaque(64);
    if (fh.empty()) { // no handle in the reply: look it up
        ALEMBIC_ASSIGN(Found f, lookup(path));
        fh = f.fh;
    }
    writing_ = true;
    write_fh_ = fh;
    write_path_ = path;
    write_offset_ = 0;
    write_buffer_.clear();
    verifier_.clear();
    verifier_changed_ = false;
    return {};
}

Status Client::finish_write(std::uint32_t xid) {
    ALEMBIC_ASSIGN(auto r, nfs_.wait(xid));
    In in{r};
    const std::uint32_t status = in.u32();
    if (status != 0) return nfs_status("writing failed", status);
    wcc_data(in);
    in.u32(); // count
    in.u32(); // committed
    std::vector<std::uint8_t> v(8);
    for (int i = 0; i < 8; ++i) v[i] = in.at + i < r.size() ? r[in.at + i] : 0;
    if (in.bad) return nfs_error("malformed WRITE reply");
    if (verifier_.empty()) verifier_ = v;
    else if (v != verifier_) verifier_changed_ = true;
    return {};
}

Status Client::send_write(const std::uint8_t* data, std::size_t size) {
    while (writes_.size() >= kDepth) {
        const std::uint32_t xid = writes_.front();
        writes_.pop_front();
        ALEMBIC_TRY(finish_write(xid));
    }
    Xdr a;
    a.b.reserve(64 + size);
    a.fh(write_fh_).u64(write_offset_).u32(static_cast<std::uint32_t>(size)).u32(0 /* UNSTABLE */).opaque(data, size);
    ALEMBIC_ASSIGN(std::uint32_t xid, nfs_.send(kWrite, a));
    writes_.push_back(xid);
    write_offset_ += size;
    return {};
}

Status Client::write(Bytes data) {
    if (!writing_) return nfs_error("no upload is open");
    const std::size_t size = wsize_;
    std::size_t at = 0;
    if (!write_buffer_.empty()) {
        const std::size_t n = std::min(size - write_buffer_.size(), data.size());
        write_buffer_.insert(write_buffer_.end(), data.data(), data.data() + n);
        at = n;
        if (write_buffer_.size() < size) return {};
        ALEMBIC_TRY(send_write(write_buffer_.data(), write_buffer_.size()));
        write_buffer_.clear();
    }
    while (data.size() - at >= size) {
        ALEMBIC_TRY(send_write(data.data() + at, size));
        at += size;
    }
    write_buffer_.assign(data.data() + at, data.data() + data.size());
    return {};
}

Status Client::finish_upload() {
    if (!writing_) return {};
    writing_ = false;
    Status s;
    if (!write_buffer_.empty()) s = send_write(write_buffer_.data(), write_buffer_.size());
    write_buffer_.clear();
    while (!writes_.empty()) {
        const std::uint32_t xid = writes_.front();
        writes_.pop_front();
        Status w = finish_write(xid);
        if (s.ok() && !w.ok()) s = w;
    }
    if (!s.ok()) return s;
    // COMMIT: the server has it all on disk, unless it restarted meanwhile
    // (its verifier changed: unstable writes may be lost).
    Xdr a;
    a.fh(write_fh_).u64(0).u32(0);
    ALEMBIC_ASSIGN(auto r, nfs_.call(kCommit, a));
    In in{r};
    const std::uint32_t status = in.u32();
    if (status != 0) return nfs_status("saving " + write_path_ + " failed", status);
    wcc_data(in);
    std::vector<std::uint8_t> v(8);
    for (int i = 0; i < 8; ++i) v[i] = in.at + i < r.size() ? r[in.at + i] : 0;
    if (verifier_changed_ || (!verifier_.empty() && v != verifier_))
        return nfs_error("the server restarted during the upload of " + write_path_ + "; start it again");
    return {};
}

} // namespace

Result<std::unique_ptr<netfs::Session>> open(const netfs::Server& server) {
    ALEMBIC_ASSIGN(auto c, Client::connect(server));
    return std::unique_ptr<netfs::Session>(std::move(c));
}

} // namespace alembic::engine::nfs
