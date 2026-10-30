// SPDX-License-Identifier: GPL-3.0-or-later
#include "smb.hpp"

#include "net.hpp"
#include "platform.hpp"

#include "alembic/crypto.hpp"
#include "alembic/unicode.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <deque>
#include <map>

namespace alembic::engine::smb {

using netcrypto::Md;

namespace {

// Commands ([MS-SMB2] 2.2.1).
enum Command : std::uint16_t {
    kNegotiate = 0,
    kSessionSetup = 1,
    kLogoff = 2,
    kTreeConnect = 3,
    kCreate = 5,
    kClose = 6,
    kRead = 8,
    kWrite = 9,
    kQueryDirectory = 14,
    kSetInfo = 17,
};

// NTSTATUS values the client acts on.
constexpr std::uint32_t kSuccess = 0;
constexpr std::uint32_t kPending = 0x00000103;
constexpr std::uint32_t kNoMoreFiles = 0x80000006;
constexpr std::uint32_t kMoreProcessing = 0xC0000016;
constexpr std::uint32_t kEndOfFile = 0xC0000011;

constexpr std::uint32_t kFlagResponse = 0x1, kFlagAsync = 0x2, kFlagSigned = 0x8;

// Access rights, create dispositions and options ([MS-SMB2] 2.2.13).
constexpr std::uint32_t kReadData = 0x1, kWriteData = 0x2, kReadAttributes = 0x80, kWriteAttributes = 0x100;
constexpr std::uint32_t kDelete = 0x10000, kSynchronize = 0x100000;
constexpr std::uint32_t kOpen = 1, kCreateNew = 2, kOverwriteIf = 5;
constexpr std::uint32_t kDirectoryFile = 0x1, kNonDirectoryFile = 0x40, kDeleteOnClose = 0x1000;
constexpr std::uint32_t kAttributeDirectory = 0x10, kAttributeNormal = 0x80;

// NTLM negotiate flags ([MS-NLMP] 2.2.2.5).
constexpr std::uint32_t kNtlmUnicode = 0x1, kNtlmRequestTarget = 0x4, kNtlmSign = 0x10, kNtlmNtlm = 0x200,
                        kNtlmAnonymous = 0x800, kNtlmAlwaysSign = 0x8000, kNtlmExtendedSecurity = 0x80000,
                        kNtlmTargetInfo = 0x800000, kNtlm128 = 0x20000000, kNtlmKeyExchange = 0x40000000,
                        kNtlm56 = 0x80000000;

constexpr int kDepth = 4; // reads or writes in flight

const char* status_name(std::uint32_t s) {
    switch (s) {
    case 0xC0000022: return "access denied";
    case 0xC000006D: return "wrong user name or password";
    case 0xC000006E: return "account restriction (a blank password, or sign-in hours)";
    case 0xC0000071: return "the password has expired";
    case 0xC0000072: return "the account is disabled";
    case 0xC0000234: return "the account is locked out";
    case 0xC0000034: return "no such file or folder";
    case 0xC000003A: return "no such folder";
    case 0xC0000033: return "invalid name";
    case 0xC0000035: return "it already exists";
    case 0xC0000043: return "the file is in use";
    case 0xC00000BA: return "it is a folder";
    case 0xC0000103: return "not a folder";
    case 0xC00000CC: return "no such share";
    case 0xC000007F: return "the disk is full";
    case 0xC00000BB: return "not supported by the server";
    case 0xC000009A: return "the server is out of resources";
    case 0xC0000203: case 0xC000035C: return "the session has expired";
    case 0xC00000C9: return "the share is not available";
    case 0xC0000101: return "the folder is not empty";
    case 0xC0000056: return "the file is being deleted";
    case 0xC000000D: return "invalid parameter";
    case 0xC0000010: return "invalid request";
    case 0xC0000061: return "the server does not allow this";
    case 0xC000015B: return "this kind of login is not allowed";
    default: return nullptr;
    }
}

std::string status_text(std::uint32_t s) {
    if (const char* name = status_name(s)) return name;
    char hex[16];
    std::snprintf(hex, sizeof hex, "0x%08X", s);
    return std::string("status ") + hex;
}

Error smb_error(const std::string& what) { return make_error(Errc::io, "SMB: " + what); }
Error smb_status(const std::string& what, std::uint32_t status) {
    // Missing names are an argument problem, not the network's.
    Errc code = status == 0xC0000034 || status == 0xC000003A ? Errc::invalid_argument : Errc::io;
    return make_error(code, "SMB: " + what + ": " + status_text(status));
}

std::uint16_t get16(const std::vector<std::uint8_t>& b, std::size_t at) {
    return at + 2 <= b.size() ? static_cast<std::uint16_t>(b[at] | (b[at + 1] << 8)) : 0;
}
std::uint32_t get32(const std::vector<std::uint8_t>& b, std::size_t at) {
    return at + 4 <= b.size() ? static_cast<std::uint32_t>(b[at] | (b[at + 1] << 8) | (b[at + 2] << 16) | (std::uint32_t(b[at + 3]) << 24)) : 0;
}
std::uint64_t get64(const std::vector<std::uint8_t>& b, std::size_t at) {
    return static_cast<std::uint64_t>(get32(b, at)) | (static_cast<std::uint64_t>(get32(b, at + 4)) << 32);
}

// A little-endian message under construction.
struct Out {
    std::vector<std::uint8_t> b;
    Out& u8(std::uint8_t v) { b.push_back(v); return *this; }
    Out& u16(std::uint16_t v) { u8(v & 0xff); return u8(v >> 8); }
    Out& u32(std::uint32_t v) { u16(v & 0xffff); return u16(static_cast<std::uint16_t>(v >> 16)); }
    Out& u64(std::uint64_t v) { u32(static_cast<std::uint32_t>(v)); return u32(static_cast<std::uint32_t>(v >> 32)); }
    Out& bytes(const void* p, std::size_t n) {
        b.insert(b.end(), static_cast<const std::uint8_t*>(p), static_cast<const std::uint8_t*>(p) + n);
        return *this;
    }
    Out& bytes(const std::vector<std::uint8_t>& v) { return bytes(v.data(), v.size()); }
    Out& zeros(std::size_t n) { b.insert(b.end(), n, 0); return *this; }
    Out& align(std::size_t to, std::size_t base = 0) {
        while ((b.size() + base) % to) b.push_back(0);
        return *this;
    }
    void put16(std::size_t at, std::uint16_t v) { b[at] = v & 0xff; b[at + 1] = static_cast<std::uint8_t>(v >> 8); }
    void put32(std::size_t at, std::uint32_t v) { for (int i = 0; i < 4; ++i) b[at + i] = static_cast<std::uint8_t>(v >> (8 * i)); }
};

std::vector<std::uint8_t> utf16le(const std::string& text) {
    std::vector<std::uint8_t> out;
    auto u = unicode::utf8_to_utf16(text);
    if (!u.ok()) return out;
    for (char16_t c : *u) {
        out.push_back(static_cast<std::uint8_t>(c & 0xff));
        out.push_back(static_cast<std::uint8_t>(c >> 8));
    }
    return out;
}

std::string from_utf16le(const std::uint8_t* p, std::size_t bytes) {
    std::u16string u;
    for (std::size_t i = 0; i + 1 < bytes; i += 2) u.push_back(static_cast<char16_t>(p[i] | (p[i + 1] << 8)));
    return unicode::utf16_to_utf8(u);
}

std::string upper(const std::string& text) {
    // NTLM upper-cases the user name; ASCII and Latin-1 cover the names seen in practice.
    auto u = unicode::utf8_to_utf16(text);
    if (!u.ok()) return text;
    for (char16_t& c : *u)
        if ((c >= u'a' && c <= u'z') || (c >= 0xE0 && c <= 0xFE && c != 0xF7)) c = static_cast<char16_t>(c - 32);
    return unicode::utf16_to_utf8(*u);
}

// FILETIME (100 ns since 1601) to Unix seconds.
std::int64_t unix_time(std::uint64_t filetime) {
    if (!filetime) return 0;
    return static_cast<std::int64_t>(filetime / 10000000ull) - 11644473600ll;
}

std::uint64_t filetime_now() {
    using namespace std::chrono;
    auto since = duration_cast<microseconds>(system_clock::now().time_since_epoch()).count();
    return static_cast<std::uint64_t>(since) * 10 + 116444736000000000ull;
}

void random_bytes(std::uint8_t* out, std::size_t n) {
    if (platform::secure_random(out, n).ok()) return;
    // Not secret: a challenge or an id only needs to differ between logins.
    std::uint64_t x = filetime_now() ^ reinterpret_cast<std::uintptr_t>(out);
    for (std::size_t i = 0; i < n; ++i) {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        out[i] = static_cast<std::uint8_t>(x);
    }
}

// DER: a tag and its length, then the contents.
std::vector<std::uint8_t> der(std::uint8_t tag, const std::vector<std::uint8_t>& content) {
    std::vector<std::uint8_t> out{tag};
    std::size_t n = content.size();
    if (n < 0x80) {
        out.push_back(static_cast<std::uint8_t>(n));
    } else if (n < 0x100) {
        out.push_back(0x81);
        out.push_back(static_cast<std::uint8_t>(n));
    } else if (n < 0x10000) {
        out.push_back(0x82);
        out.push_back(static_cast<std::uint8_t>(n >> 8));
        out.push_back(static_cast<std::uint8_t>(n));
    } else {
        out.push_back(0x83);
        out.push_back(static_cast<std::uint8_t>(n >> 16));
        out.push_back(static_cast<std::uint8_t>(n >> 8));
        out.push_back(static_cast<std::uint8_t>(n));
    }
    out.insert(out.end(), content.begin(), content.end());
    return out;
}
std::vector<std::uint8_t> cat(std::initializer_list<std::vector<std::uint8_t>> parts) {
    std::vector<std::uint8_t> out;
    for (const auto& p : parts) out.insert(out.end(), p.begin(), p.end());
    return out;
}

// SPNEGO ([RFC 4178]) around NTLMSSP: the first token offers NTLM only.
std::vector<std::uint8_t> spnego_init(const std::vector<std::uint8_t>& ntlm) {
    const std::vector<std::uint8_t> spnego_oid{0x06, 0x06, 0x2b, 0x06, 0x01, 0x05, 0x05, 0x02};
    const std::vector<std::uint8_t> ntlm_oid{0x06, 0x0a, 0x2b, 0x06, 0x01, 0x04, 0x01, 0x82, 0x37, 0x02, 0x02, 0x0a};
    auto mech_types = der(0xa0, der(0x30, ntlm_oid));
    auto token = der(0xa2, der(0x04, ntlm));
    auto init = der(0xa0, der(0x30, cat({mech_types, token})));
    return der(0x60, cat({spnego_oid, init}));
}
std::vector<std::uint8_t> spnego_response(const std::vector<std::uint8_t>& ntlm) {
    return der(0xa1, der(0x30, der(0xa2, der(0x04, ntlm))));
}
// The NTLMSSP message inside a server's SPNEGO token (or a raw one).
std::vector<std::uint8_t> find_ntlm(const std::vector<std::uint8_t>& token) {
    static const std::uint8_t sig[8] = {'N', 'T', 'L', 'M', 'S', 'S', 'P', 0};
    auto it = std::search(token.begin(), token.end(), sig, sig + 8);
    return std::vector<std::uint8_t>(it, token.end());
}

struct FileId {
    std::uint8_t bytes[16] = {};
    std::uint32_t tree = 0; // the share it is on
};

} // namespace

// ---------------------------------------------------------------- NTLM and keys

namespace detail {

Md ntowfv2(const std::string& password, const std::string& user, const std::string& domain) {
    auto pw = utf16le(password);
    Md nt = netcrypto::md4(Bytes(pw.data(), pw.size()));
    auto ud = utf16le(upper(user) + domain);
    return netcrypto::hmac_md5(Bytes(nt.data(), nt.size()), Bytes(ud.data(), ud.size()));
}

Ntlmv2 ntlmv2_response(const Md& ntowf, const std::uint8_t server_challenge[8], const std::uint8_t client_challenge[8],
                       std::uint64_t timestamp, Bytes target_info) {
    Out temp;
    temp.u8(1).u8(1).zeros(6).u64(timestamp).bytes(client_challenge, 8).zeros(4).bytes(target_info.data(), target_info.size()).zeros(4);
    Out signed_part;
    signed_part.bytes(server_challenge, 8).bytes(temp.b);
    Md proof = netcrypto::hmac_md5(Bytes(ntowf.data(), 16), Bytes(signed_part.b.data(), signed_part.b.size()));
    Ntlmv2 r;
    r.nt_response.assign(proof.begin(), proof.end());
    r.nt_response.insert(r.nt_response.end(), temp.b.begin(), temp.b.end());
    r.session_base_key = netcrypto::hmac_md5(Bytes(ntowf.data(), 16), Bytes(proof.data(), proof.size()));
    return r;
}

Md kdf(const std::uint8_t key[16], Bytes label, Bytes context) {
    Out m;
    m.u8(0).u8(0).u8(0).u8(1); // i = 1, big-endian
    m.bytes(label.data(), label.size()).u8(0).bytes(context.data(), context.size());
    m.u8(0).u8(0).u8(0).u8(128); // L = 128 bits, big-endian
    auto d = crypto::hmac_sha256(Bytes(key, 16), Bytes(m.b.data(), m.b.size()));
    Md out;
    std::memcpy(out.data(), d.data(), 16);
    return out;
}

void rc4(Bytes key, MutableBytes data) {
    std::uint8_t s[256];
    for (int i = 0; i < 256; ++i) s[i] = static_cast<std::uint8_t>(i);
    for (int i = 0, j = 0; i < 256; ++i) {
        j = (j + s[i] + key[static_cast<std::size_t>(i) % key.size()]) & 255;
        std::swap(s[i], s[j]);
    }
    int i = 0, j = 0;
    for (std::uint8_t& b : data) {
        i = (i + 1) & 255;
        j = (j + s[i]) & 255;
        std::swap(s[i], s[j]);
        b ^= s[(s[i] + s[j]) & 255];
    }
}

} // namespace detail

// ---------------------------------------------------------------- the client

namespace {

class Client final : public netfs::Session {
public:
    static Result<std::unique_ptr<Client>> connect(const netfs::Server& server);
    ~Client() override {
        if (socket_ != net::kNone) net::close(socket_);
    }

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
    struct Message {
        std::uint32_t status = 0;
        std::uint16_t command = 0;
        std::uint64_t id = 0;
        std::uint32_t flags = 0;
        std::vector<std::uint8_t> bytes; // header and body
        std::size_t body() const { return 64; }
    };
    struct Opened {
        FileId id;
        std::uint64_t size = 0;
        bool directory = false;
        std::int64_t mtime = 0;
    };

    Status negotiate();
    Status login(const std::string& user, const std::string& password);
    Result<std::uint32_t> tree_connect(const std::string& share);
    // The share a path is on and the path within it ('\\' between names).
    // Without a share in the server's settings, the first name is the share.
    Result<std::pair<std::uint32_t, std::string>> locate(const std::string& path);
    Result<std::vector<netfs::Entry>> list_shares();
    // One DCE/RPC exchange on an open named pipe: all fragments of the reply.
    Result<std::vector<std::vector<std::uint8_t>>> pipe_call(const FileId& pipe, const std::vector<std::uint8_t>& pdu);

    // Sends a request; `payload` is the data a READ or WRITE moves (for its credit charge).
    Result<std::uint64_t> send(std::uint16_t command, const std::vector<std::uint8_t>& body, std::size_t payload = 0,
                               std::vector<std::uint8_t>* sent = nullptr);
    Result<Message> receive();
    Result<Message> wait(std::uint64_t id);
    Result<Message> call(std::uint16_t command, const std::vector<std::uint8_t>& body) {
        ALEMBIC_ASSIGN(std::uint64_t id, send(command, body));
        return wait(id);
    }

    Result<Opened> create_in(std::uint32_t tree, const std::string& name, const std::string& shown, std::uint32_t access,
                             std::uint32_t disposition, std::uint32_t options, std::uint32_t attributes = 0);
    Result<Opened> create(const std::string& path, std::uint32_t access, std::uint32_t disposition, std::uint32_t options,
                          std::uint32_t attributes = 0);
    Status close(const FileId& id);

    net::Socket socket_ = net::kNone;
    std::uint16_t dialect_ = 0;
    std::uint32_t max_read_ = 65536, max_write_ = 65536, max_transact_ = 65536;
    bool large_mtu_ = false;
    bool sign_ = false;
    std::uint8_t session_key_[16] = {}, signing_key_[16] = {};
    netcrypto::Sha512Digest preauth_{}; // 3.1.1
    std::uint64_t session_id_ = 0;
    std::uint32_t tree_id_ = 0;
    std::uint64_t next_id_ = 0;
    std::uint32_t credits_ = 1;
    std::map<std::uint64_t, Message> early_; // replies that came before the one waited for
    std::string prefix_;                       // a folder within the share, '\' between names
    std::string host_;
    bool whole_server_ = false;                // no share given: the server's shares are the top folders
    std::uint32_t main_tree_ = 0;              // the given share's
    std::map<std::string, std::uint32_t> trees_; // connected shares (whole server), by lower-case name

    // The open download: requests in flight in order, the data they brought.
    bool reading_ = false;
    FileId read_file_;
    std::uint64_t read_size_ = 0, read_next_ = 0;
    std::deque<std::uint64_t> reads_;
    std::vector<std::uint8_t> read_data_;
    std::size_t read_at_ = 0;
    Status fill_reads();

    // The open upload: writes in flight, and data not yet sent.
    bool writing_ = false;
    FileId write_file_;
    std::uint64_t write_offset_ = 0;
    std::deque<std::uint64_t> writes_;
    std::vector<std::uint8_t> write_buffer_;
    Status send_write(const std::uint8_t* data, std::size_t size);
    Status finish_write(std::uint64_t id);

    std::uint32_t chunk(std::uint32_t max) const {
        if (!large_mtu_) return std::min<std::uint32_t>(max, 65536);
        return std::min<std::uint32_t>(max, 1u << 20);
    }
    std::uint16_t charge(std::size_t payload) const {
        if (dialect_ <= 0x0202) return 0;
        return static_cast<std::uint16_t>(payload ? (payload - 1) / 65536 + 1 : 1);
    }
};

Result<std::uint64_t> Client::send(std::uint16_t command, const std::vector<std::uint8_t>& body, std::size_t payload,
                                   std::vector<std::uint8_t>* sent) {
    std::uint16_t credit_charge = charge(payload);
    std::uint32_t needed = std::max<std::uint32_t>(1, credit_charge);
    // Wait for credits: each reply in flight brings some back.
    while (credits_ < needed && (!reads_.empty() || !writes_.empty())) {
        ALEMBIC_ASSIGN(Message m, receive());
        early_[m.id] = std::move(m);
    }
    const std::uint64_t id = next_id_;
    next_id_ += needed;
    credits_ = credits_ >= needed ? credits_ - needed : 0;
    Out h;
    h.u8(0xFE).u8('S').u8('M').u8('B').u16(64).u16(credit_charge).u32(0).u16(command).u16(static_cast<std::uint16_t>(needed + 64));
    h.u32(sign_ && command != kNegotiate && command != kSessionSetup ? kFlagSigned : 0).u32(0).u64(id);
    h.u32(0xFEFF).u32(tree_id_).u64(session_id_).zeros(16);
    h.bytes(body);
    if (h.b[16] & kFlagSigned) {
        Md sig;
        if (dialect_ >= 0x0300) {
            sig = netcrypto::aes_cmac(signing_key_, Bytes(h.b.data(), h.b.size()));
        } else {
            auto full = crypto::hmac_sha256(Bytes(session_key_, 16), Bytes(h.b.data(), h.b.size()));
            std::memcpy(sig.data(), full.data(), 16);
        }
        std::memcpy(h.b.data() + 48, sig.data(), 16);
    }
    std::uint8_t frame[4] = {0, static_cast<std::uint8_t>(h.b.size() >> 16), static_cast<std::uint8_t>(h.b.size() >> 8),
                             static_cast<std::uint8_t>(h.b.size())};
    ALEMBIC_TRY(netfs::send_all(socket_, frame, 4, "SMB"));
    ALEMBIC_TRY(netfs::send_all(socket_, h.b.data(), h.b.size(), "SMB"));
    if (sent) *sent = std::move(h.b);
    return id;
}

Result<Client::Message> Client::receive() {
    for (;;) {
        std::uint8_t frame[4];
        ALEMBIC_TRY(netfs::recv_all(socket_, frame, 4, "SMB"));
        std::size_t length = (std::size_t(frame[1]) << 16) | (std::size_t(frame[2]) << 8) | frame[3];
        if (frame[0] != 0 || length < 64) return smb_error("unexpected data from the server");
        Message m;
        m.bytes.resize(length);
        ALEMBIC_TRY(netfs::recv_all(socket_, m.bytes.data(), length, "SMB"));
        if (m.bytes[0] == 0xFD) return smb_error("the server encrypts its replies, which Alembic does not support yet");
        if (m.bytes[0] != 0xFE || m.bytes[1] != 'S') return smb_error("not an SMB2 reply (an SMB1-only server?)");
        m.status = get32(m.bytes, 8);
        m.command = get16(m.bytes, 12);
        credits_ += get16(m.bytes, 14);
        m.flags = get32(m.bytes, 16);
        m.id = get64(m.bytes, 24);
        if (!(m.flags & kFlagResponse)) continue;
        if (m.id == ~std::uint64_t{0}) continue;                 // an oplock break: none were asked for
        if ((m.flags & kFlagAsync) && m.status == kPending) continue; // the real reply follows
        return m;
    }
}

Result<Client::Message> Client::wait(std::uint64_t id) {
    if (auto it = early_.find(id); it != early_.end()) {
        Message m = std::move(it->second);
        early_.erase(it);
        return m;
    }
    for (;;) {
        ALEMBIC_ASSIGN(Message m, receive());
        if (m.id == id) return m;
        early_[m.id] = std::move(m);
    }
}

Status Client::negotiate() {
    static const std::uint16_t dialects[] = {0x0202, 0x0210, 0x0300, 0x0302, 0x0311};
    Out b;
    b.u16(36).u16(5).u16(1 /* signing enabled */).u16(0).u32(0x4 /* large MTU */);
    std::uint8_t guid[16];
    random_bytes(guid, 16);
    b.bytes(guid, 16);
    const std::size_t context_offset_at = b.b.size();
    b.u32(0).u16(1).u16(0);
    for (std::uint16_t d : dialects) b.u16(d);
    b.align(8, 64);
    b.put32(context_offset_at, static_cast<std::uint32_t>(64 + b.b.size()));
    // SMB2_PREAUTH_INTEGRITY_CAPABILITIES: SHA-512 with a 32-byte salt.
    std::uint8_t salt[32];
    random_bytes(salt, 32);
    b.u16(1).u16(38).u32(0).u16(1).u16(32).u16(1).bytes(salt, 32);
    std::vector<std::uint8_t> request;
    ALEMBIC_ASSIGN(std::uint64_t id, send(kNegotiate, b.b, 0, &request));
    ALEMBIC_ASSIGN(Message r, wait(id));
    if (r.status != kSuccess) return smb_status("negotiation failed", r.status);
    const auto& m = r.bytes;
    dialect_ = get16(m, 64 + 4);
    if (dialect_ < 0x0202 || dialect_ > 0x0311) return smb_error("the server offers no SMB2 or SMB3 dialect Alembic speaks");
    std::uint32_t caps = get32(m, 64 + 24);
    large_mtu_ = dialect_ > 0x0202 && (caps & 0x4);
    max_transact_ = std::max<std::uint32_t>(get32(m, 64 + 28), 65536);
    max_read_ = std::max<std::uint32_t>(get32(m, 64 + 32), 65536);
    max_write_ = std::max<std::uint32_t>(get32(m, 64 + 36), 65536);
    if (dialect_ == 0x0311) {
        netcrypto::Sha512 h;
        h.update(Bytes(preauth_.data(), 64));
        h.update(Bytes(request.data(), request.size()));
        preauth_ = h.finish();
        netcrypto::Sha512 h2;
        h2.update(Bytes(preauth_.data(), 64));
        h2.update(Bytes(r.bytes.data(), r.bytes.size()));
        preauth_ = h2.finish();
    }
    return {};
}

Status Client::login(const std::string& user_text, const std::string& password) {
    // "DOMAIN\user" names the domain; otherwise there is none (local accounts).
    std::string user = user_text, domain;
    if (std::size_t slash = user.find('\\'); slash != std::string::npos) {
        domain = user.substr(0, slash);
        user = user.substr(slash + 1);
    }
    const bool anonymous = user.empty();
    const std::uint32_t wanted = kNtlmUnicode | kNtlmRequestTarget | kNtlmSign | kNtlmNtlm | kNtlmAlwaysSign |
                                 kNtlmExtendedSecurity | kNtlmTargetInfo | kNtlm128 | kNtlmKeyExchange | kNtlm56;
    netcrypto::Sha512Digest hash = preauth_;
    auto setup = [&](const std::vector<std::uint8_t>& token, Message* reply) -> Status {
        Out b;
        b.u16(25).u8(0).u8(1).u32(0).u32(0).u16(64 + 24).u16(static_cast<std::uint16_t>(token.size())).u64(0).bytes(token);
        std::vector<std::uint8_t> request;
        ALEMBIC_ASSIGN(std::uint64_t id, send(kSessionSetup, b.b, 0, &request));
        if (dialect_ == 0x0311) {
            netcrypto::Sha512 h;
            h.update(Bytes(hash.data(), 64));
            h.update(Bytes(request.data(), request.size()));
            hash = h.finish();
        }
        ALEMBIC_ASSIGN(*reply, wait(id));
        if (reply->status == kMoreProcessing && dialect_ == 0x0311) {
            netcrypto::Sha512 h;
            h.update(Bytes(hash.data(), 64));
            h.update(Bytes(reply->bytes.data(), reply->bytes.size()));
            hash = h.finish();
        }
        return {};
    };

    // 1. NEGOTIATE_MESSAGE
    Out n1;
    n1.bytes("NTLMSSP", 8).u32(1).u32(wanted).zeros(16);
    Message r1;
    ALEMBIC_TRY(setup(spnego_init(n1.b), &r1));
    if (r1.status != kMoreProcessing) return smb_status("login failed", r1.status);
    session_id_ = get64(r1.bytes, 40);
    std::vector<std::uint8_t> sec(r1.bytes.begin() + std::min<std::size_t>(r1.bytes.size(), get16(r1.bytes, 64 + 4)), r1.bytes.end());
    sec.resize(std::min<std::size_t>(sec.size(), get16(r1.bytes, 64 + 6)));
    // 2. CHALLENGE_MESSAGE
    std::vector<std::uint8_t> challenge = find_ntlm(sec);
    if (challenge.size() < 48 || get32(challenge, 8) != 2) return smb_error("the server did not offer an NTLM login");
    const std::uint32_t offered = get32(challenge, 20);
    std::uint8_t server_challenge[8];
    std::memcpy(server_challenge, challenge.data() + 24, 8);
    std::uint16_t info_length = get16(challenge, 40);
    std::uint32_t info_offset = get32(challenge, 44);
    if (info_offset > challenge.size() || info_length > challenge.size() - info_offset)
        return smb_error("the server's NTLM challenge is malformed");
    std::vector<std::uint8_t> target_info(challenge.begin() + info_offset, challenge.begin() + info_offset + info_length);
    std::uint64_t timestamp = 0;
    bool has_timestamp = false;
    for (std::size_t at = 0; at + 4 <= target_info.size();) {
        std::uint16_t av = get16(target_info, at), len = get16(target_info, at + 2);
        if (av == 0) break;
        if (av == 7 && len == 8) {
            timestamp = get64(target_info, at + 4);
            has_timestamp = true;
        }
        at += 4 + len;
    }
    if (!has_timestamp) timestamp = filetime_now();

    // 3. AUTHENTICATE_MESSAGE
    std::uint32_t flags = (wanted & offered) | kNtlmUnicode | kNtlmNtlm;
    std::vector<std::uint8_t> lm, nt, encrypted_key;
    Md exported{};
    if (anonymous) {
        flags |= kNtlmAnonymous;
        flags &= ~kNtlmKeyExchange;
        lm.assign(1, 0);
    } else {
        std::uint8_t client_challenge[8];
        random_bytes(client_challenge, 8);
        Md ntowf = detail::ntowfv2(password, user, domain);
        auto v2 = detail::ntlmv2_response(ntowf, server_challenge, client_challenge, timestamp,
                                          Bytes(target_info.data(), target_info.size()));
        nt = std::move(v2.nt_response);
        if (has_timestamp) {
            lm.assign(24, 0);
        } else {
            Out c;
            c.bytes(server_challenge, 8).bytes(client_challenge, 8);
            Md l = netcrypto::hmac_md5(Bytes(ntowf.data(), 16), Bytes(c.b.data(), c.b.size()));
            lm.assign(l.begin(), l.end());
            lm.insert(lm.end(), client_challenge, client_challenge + 8);
        }
        exported = v2.session_base_key;
        if (flags & kNtlmKeyExchange) {
            random_bytes(exported.data(), 16);
            encrypted_key.assign(exported.begin(), exported.end());
            detail::rc4(Bytes(v2.session_base_key.data(), 16), MutableBytes(encrypted_key.data(), 16));
        }
    }
    auto dom = utf16le(domain), usr = utf16le(user), ws = utf16le("ALEMBIC");
    Out a;
    a.bytes("NTLMSSP", 8).u32(3);
    std::uint32_t at = 64;
    auto field = [&](const std::vector<std::uint8_t>& v) {
        a.u16(static_cast<std::uint16_t>(v.size())).u16(static_cast<std::uint16_t>(v.size())).u32(at);
        at += static_cast<std::uint32_t>(v.size());
    };
    field(lm);
    field(nt);
    field(dom);
    field(usr);
    field(ws);
    field(encrypted_key);
    a.u32(flags);
    a.bytes(lm).bytes(nt).bytes(dom).bytes(usr).bytes(ws).bytes(encrypted_key);
    Message r2;
    ALEMBIC_TRY(setup(spnego_response(a.b), &r2));
    if (r2.status != kSuccess) return smb_status("login failed", r2.status);
    const std::uint16_t session_flags = get16(r2.bytes, 64 + 2);
    if (session_flags & 0x4) return smb_error("the server requires encryption, which Alembic does not support yet");
    const bool guest = anonymous || (session_flags & 0x3);
    if (!guest) {
        std::memcpy(session_key_, exported.data(), 16);
        if (dialect_ == 0x0311) {
            static const char label[] = "SMBSigningKey"; // with its NUL
            Md k = detail::kdf(session_key_, Bytes(reinterpret_cast<const std::uint8_t*>(label), sizeof label), Bytes(hash.data(), 64));
            std::memcpy(signing_key_, k.data(), 16);
        } else if (dialect_ >= 0x0300) {
            static const char label[] = "SMB2AESCMAC", context[] = "SmbSign";
            Md k = detail::kdf(session_key_, Bytes(reinterpret_cast<const std::uint8_t*>(label), sizeof label),
                               Bytes(reinterpret_cast<const std::uint8_t*>(context), sizeof context));
            std::memcpy(signing_key_, k.data(), 16);
        }
        sign_ = true;
    }
    return {};
}

Result<std::uint32_t> Client::tree_connect(const std::string& share) {
    auto path = utf16le("\\\\" + host_ + "\\" + share);
    Out b;
    b.u16(9).u16(0).u16(64 + 8).u16(static_cast<std::uint16_t>(path.size())).bytes(path);
    tree_id_ = 0;
    ALEMBIC_ASSIGN(Message r, call(kTreeConnect, b.b));
    if (r.status != kSuccess) return smb_status("cannot open the share " + share, r.status);
    if (get32(r.bytes, 64 + 4) & 0x8000) return smb_error("the share " + share + " requires encryption, which Alembic does not support yet");
    return get32(r.bytes, 36);
}

Result<std::unique_ptr<Client>> Client::connect(const netfs::Server& server) {
    std::unique_ptr<Client> c(new Client());
    c->host_ = server.host;
    ALEMBIC_ASSIGN(c->socket_, netfs::tcp_connect(server.host, server.port, 10000, "SMB"));
    ALEMBIC_TRY(c->negotiate());
    ALEMBIC_TRY(c->login(server.user, server.password));
    std::string root = server.root, share = root.substr(0, root.find('/'));
    if (share.empty()) {
        c->whole_server_ = true; // shares are connected when first used
        return c;
    }
    if (root.size() > share.size()) {
        c->prefix_ = root.substr(share.size() + 1);
        for (char& ch : c->prefix_)
            if (ch == '/') ch = '\\';
    }
    ALEMBIC_ASSIGN(c->main_tree_, c->tree_connect(share));
    return c;
}

Result<std::pair<std::uint32_t, std::string>> Client::locate(const std::string& path) {
    std::vector<std::string> parts;
    for (std::size_t at = 0; at < path.size();) {
        std::size_t slash = path.find('/', at);
        std::string part = path.substr(at, slash == std::string::npos ? std::string::npos : slash - at);
        at = slash == std::string::npos ? path.size() : slash + 1;
        if (!part.empty() && part != ".") parts.push_back(part);
    }
    std::uint32_t tree = main_tree_;
    std::string n = prefix_;
    std::size_t first = 0;
    if (whole_server_) {
        if (parts.empty()) return make_error(Errc::invalid_argument, "SMB: the server's top level holds only shares");
        std::string key = parts[0];
        for (char& ch : key) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (auto it = trees_.find(key); it != trees_.end()) {
            tree = it->second;
        } else {
            ALEMBIC_ASSIGN(tree, tree_connect(parts[0]));
            trees_[key] = tree;
        }
        first = 1;
    }
    for (std::size_t i = first; i < parts.size(); ++i) {
        if (!n.empty()) n += '\\';
        n += parts[i];
    }
    return std::make_pair(tree, n);
}

Result<Client::Opened> Client::create_in(std::uint32_t tree, const std::string& name, const std::string& shown,
                                          std::uint32_t access, std::uint32_t disposition, std::uint32_t options,
                                          std::uint32_t attributes) {
    auto n = utf16le(name);
    Out b;
    b.u16(57).u8(0).u8(0).u32(2 /* impersonation */).u64(0).u64(0);
    b.u32(access).u32(attributes).u32(0x7 /* share read, write, delete */).u32(disposition).u32(options);
    b.u16(64 + 56).u16(static_cast<std::uint16_t>(n.size())).u32(0).u32(0);
    b.bytes(n);
    if (n.empty()) b.u8(0);
    tree_id_ = tree;
    ALEMBIC_ASSIGN(Message r, call(kCreate, b.b));
    if (r.status != kSuccess) return smb_status("cannot open " + shown, r.status);
    Opened o;
    o.mtime = unix_time(get64(r.bytes, 64 + 24));
    o.size = get64(r.bytes, 64 + 48);
    o.directory = get32(r.bytes, 64 + 56) & kAttributeDirectory;
    if (r.bytes.size() < 64 + 80) return smb_error("short reply to CREATE");
    std::memcpy(o.id.bytes, r.bytes.data() + 64 + 64, 16);
    o.id.tree = tree;
    return o;
}

Result<Client::Opened> Client::create(const std::string& path, std::uint32_t access, std::uint32_t disposition,
                                       std::uint32_t options, std::uint32_t attributes) {
    ALEMBIC_ASSIGN(auto where, locate(path));
    return create_in(where.first, where.second, path, access, disposition, options, attributes);
}

Status Client::close(const FileId& id) {
    Out b;
    b.u16(24).u16(0).u32(0).bytes(id.bytes, 16);
    tree_id_ = id.tree;
    ALEMBIC_ASSIGN(Message r, call(kClose, b.b));
    if (r.status != kSuccess) return smb_status("closing a file failed", r.status);
    return {};
}

// ---------------------------------------------------------------- shares

namespace {

// DCE/RPC ([C706], [MS-RPCE]) over the srvsvc pipe: NetrShareEnumAll
// ([MS-SRVS] 3.1.4.8) at level 1.
constexpr std::uint8_t kSrvsvc[16] = {0xc8, 0x4f, 0x32, 0x4b, 0x70, 0x16, 0xd3, 0x01, 0x12, 0x78, 0x5a, 0x47, 0xbf, 0x6e, 0xe1, 0x88};
constexpr std::uint8_t kNdr[16] = {0x04, 0x5d, 0x88, 0x8a, 0xeb, 0x1c, 0xc9, 0x11, 0x9f, 0xe8, 0x08, 0x00, 0x2b, 0x10, 0x48, 0x60};
constexpr std::uint32_t kBufferOverflow = 0x80000005;

std::vector<std::uint8_t> rpc_pdu(std::uint8_t type, std::uint32_t call, const std::vector<std::uint8_t>& body) {
    Out p;
    p.u8(5).u8(0).u8(type).u8(0x03 /* first and last fragment */).u8(0x10).u8(0).u8(0).u8(0);
    p.u16(static_cast<std::uint16_t>(16 + body.size())).u16(0).u32(call).bytes(body);
    return p.b;
}

// An NDR unique pointer to a conformant varying UTF-16 string, with its NUL.
void ndr_string(Out& o, const std::string& text) {
    auto u = utf16le(text);
    const std::uint32_t chars = static_cast<std::uint32_t>(u.size() / 2 + 1);
    o.u32(0x00020000).u32(chars).u32(0).u32(chars).bytes(u).u16(0);
    o.align(4);
}

struct NdrReader {
    const std::vector<std::uint8_t>& b;
    std::size_t at = 0;
    bool bad = false;
    std::uint32_t u32() {
        at = (at + 3) & ~std::size_t{3};
        if (at + 4 > b.size()) {
            bad = true;
            return 0;
        }
        std::uint32_t v = get32(b, at);
        at += 4;
        return v;
    }
    std::string str() {
        u32(); // maximum count
        u32(); // offset
        std::uint32_t chars = u32();
        if (bad || at + std::size_t{chars} * 2 > b.size()) {
            bad = true;
            return "";
        }
        std::string s = from_utf16le(b.data() + at, chars * 2);
        at += chars * 2;
        while (!s.empty() && s.back() == '\0') s.pop_back();
        return s;
    }
};

} // namespace

Result<std::vector<std::vector<std::uint8_t>>> Client::pipe_call(const FileId& pipe, const std::vector<std::uint8_t>& pdu) {
    Out w;
    w.u16(49).u16(64 + 48).u32(static_cast<std::uint32_t>(pdu.size())).u64(0).bytes(pipe.bytes, 16);
    w.u32(0).u32(0).u16(0).u16(0).u32(0).bytes(pdu);
    tree_id_ = pipe.tree;
    ALEMBIC_ASSIGN(Message wr, call(kWrite, w.b));
    if (wr.status != kSuccess) return smb_status("the share list request failed", wr.status);
    std::vector<std::uint8_t> data;
    std::vector<std::vector<std::uint8_t>> fragments;
    for (int reads = 0; reads < 1000; ++reads) {
        // Whole fragments from what came so far; done at the last one.
        while (data.size() >= 16 && get16(data, 8) <= data.size()) {
            const std::size_t length = get16(data, 8);
            if (length < 16) return smb_error("malformed share list reply");
            fragments.emplace_back(data.begin(), data.begin() + length);
            const bool last = data[3] & 0x02;
            data.erase(data.begin(), data.begin() + length);
            if (last) return fragments;
        }
        Out r;
        r.u16(49).u8(0x50).u8(0).u32(65536).u64(0).bytes(pipe.bytes, 16).u32(0).u32(0).u32(0).u16(0).u16(0).u8(0);
        tree_id_ = pipe.tree;
        ALEMBIC_ASSIGN(Message m, call(kRead, r.b));
        if (m.status != kSuccess && m.status != kBufferOverflow) return smb_status("the share list reply failed", m.status);
        std::size_t data_at = m.bytes.size() > 66 ? m.bytes[64 + 2] : 0, length = get32(m.bytes, 64 + 4);
        if (data_at < 64 || data_at + length > m.bytes.size()) return smb_error("malformed READ reply");
        data.insert(data.end(), m.bytes.begin() + data_at, m.bytes.begin() + data_at + length);
    }
    return smb_error("the share list reply never ended");
}

Result<std::vector<netfs::Entry>> Client::list_shares() {
    std::uint32_t ipc = 0;
    if (auto it = trees_.find("ipc$"); it != trees_.end()) {
        ipc = it->second;
    } else {
        ALEMBIC_ASSIGN(ipc, tree_connect("IPC$"));
        trees_["ipc$"] = ipc;
    }
    ALEMBIC_ASSIGN(Opened pipe, create_in(ipc, "srvsvc", "the server's share list", 0x0012019F, kOpen, 0));
    auto result = [&]() -> Result<std::vector<netfs::Entry>> {
        // Bind to SRVSVC 3.0 with the NDR transfer syntax.
        Out bind;
        bind.u16(4280).u16(4280).u32(0).u8(1).u8(0).u16(0);
        bind.u16(0).u8(1).u8(0).bytes(kSrvsvc, 16).u16(3).u16(0).bytes(kNdr, 16).u32(2);
        ALEMBIC_ASSIGN(auto ack, pipe_call(pipe.id, rpc_pdu(11, 1, bind.b)));
        if (ack.empty() || ack[0][2] != 12) return smb_error("the server refused the share list service");
        {
            const auto& a = ack[0];
            std::size_t at = 24 + 2 + get16(a, 24);
            at = (at + 3) & ~std::size_t{3};
            if (at + 6 > a.size() || a[at] < 1 || get16(a, at + 4) != 0) return smb_error("the server refused the share list service");
        }
        // NetrShareEnumAll(\\host, level 1, no buffer, any length, resume 0)
        Out stub;
        ndr_string(stub, "\\\\" + host_);
        stub.u32(1).u32(1).u32(0x00020004).u32(0).u32(0).u32(0xFFFFFFFF).u32(0x00020008).u32(0);
        Out req;
        req.u32(static_cast<std::uint32_t>(stub.b.size())).u16(0).u16(15).bytes(stub.b);
        ALEMBIC_ASSIGN(auto reply, pipe_call(pipe.id, rpc_pdu(0, 2, req.b)));
        std::vector<std::uint8_t> body;
        for (const auto& f : reply) {
            if (f[2] == 3) return smb_error("the share list failed (RPC fault " + std::to_string(get32(f, 24)) + ")");
            if (f[2] != 2 || f.size() < 24) return smb_error("malformed share list reply");
            const std::size_t end = f.size() - get16(f, 10); // without auth data
            body.insert(body.end(), f.begin() + 24, f.begin() + std::max<std::size_t>(24, end));
        }
        NdrReader n{body};
        n.u32();                     // level
        n.u32();                     // union arm
        if (!n.u32()) return std::vector<netfs::Entry>{}; // no container
        n.u32();                     // entries read
        if (!n.u32()) return std::vector<netfs::Entry>{}; // no array
        const std::uint32_t count = n.u32();
        if (count > 100000) return smb_error("malformed share list reply");
        struct Info {
            std::uint32_t name, type, remark;
        };
        std::vector<Info> infos(count);
        for (auto& i : infos) {
            i.name = n.u32();
            i.type = n.u32();
            i.remark = n.u32();
        }
        std::vector<netfs::Entry> out;
        for (const auto& i : infos) {
            std::string name = i.name ? n.str() : "";
            if (i.remark) n.str();
            if (n.bad) return smb_error("malformed share list reply");
            // Disk shares only; not the administrative ones (C$, ADMIN$) nor
            // others hidden by a final '$' (they can still be named as the share).
            if ((i.type & 0x0FFFFFFF) != 0 || (i.type & 0x80000000) || name.empty() || name.back() == '$') continue;
            netfs::Entry e;
            e.name = name;
            e.is_directory = true;
            out.push_back(std::move(e));
        }
        return out;
    }();
    Status closed = close(pipe.id);
    if (!result.ok()) return result.error();
    (void)closed;
    return result;
}

Result<std::uint64_t> Client::size(const std::string& path) {
    if (whole_server_ && path.find('/', 1) == std::string::npos)
        return make_error(Errc::invalid_argument, "SMB: " + path + " is a share");
    ALEMBIC_ASSIGN(Opened o, create(path, kReadAttributes | kSynchronize, kOpen, 0));
    ALEMBIC_TRY(close(o.id));
    if (o.directory) return make_error(Errc::invalid_argument, "SMB: " + path + " is a folder");
    return o.size;
}

Result<std::vector<netfs::Entry>> Client::list(const std::string& directory) {
    if (whole_server_ && (directory.empty() || directory == "/")) return list_shares();
    ALEMBIC_ASSIGN(Opened dir, create(directory, kReadData | kReadAttributes | kSynchronize, kOpen, kDirectoryFile));
    std::vector<netfs::Entry> out;
    const auto pattern = utf16le("*");
    Status failed;
    for (bool first = true;; first = false) {
        Out b;
        b.u16(33).u8(1 /* FileDirectoryInformation */).u8(first ? 1 : 0).u32(0).bytes(dir.id.bytes, 16);
        b.u16(64 + 32).u16(static_cast<std::uint16_t>(pattern.size())).u32(std::min<std::uint32_t>(max_transact_, 65536));
        b.bytes(pattern);
        auto r = call(kQueryDirectory, b.b);
        if (!r.ok()) return r.error(); // the connection is gone; nothing to close
        if (r->status == kNoMoreFiles) break;
        if (r->status != kSuccess) {
            failed = smb_status("cannot list " + directory, r->status);
            break;
        }
        const auto& m = r->bytes;
        std::size_t at = get16(m, 64 + 2), end = at + get32(m, 64 + 4);
        if (end > m.size()) end = m.size();
        while (at + 64 <= end) {
            std::uint32_t next = get32(m, at);
            std::uint32_t name_length = get32(m, at + 60);
            if (at + 64 + name_length > end) break;
            netfs::Entry e;
            e.name = from_utf16le(m.data() + at + 64, name_length);
            e.mtime = unix_time(get64(m, at + 24));
            e.size = get64(m, at + 40);
            e.is_directory = get32(m, at + 56) & kAttributeDirectory;
            if (e.is_directory) e.size = 0;
            if (e.name != "." && e.name != "..") out.push_back(std::move(e));
            if (!next) break;
            at += next;
        }
    }
    Status closed = close(dir.id);
    if (!failed.ok()) return failed.error();
    if (!closed.ok()) return closed.error();
    return out;
}

Status Client::make_directory(const std::string& path) {
    ALEMBIC_ASSIGN(Opened o, create(path, kReadData | kReadAttributes | kSynchronize, kCreateNew, kDirectoryFile, kAttributeDirectory));
    return close(o.id);
}

Status Client::rename(const std::string& from, const std::string& to) {
    ALEMBIC_ASSIGN(auto there, locate(to));
    ALEMBIC_ASSIGN(Opened o, create(from, kDelete | kReadAttributes | kSynchronize, kOpen, 0));
    if (there.first != o.id.tree) {
        (void)close(o.id);
        return smb_error("cannot move " + from + " to another share");
    }
    auto target = utf16le(there.second);
    Out info;
    info.u8(0 /* do not replace */).zeros(7).u64(0).u32(static_cast<std::uint32_t>(target.size())).bytes(target);
    Out b;
    b.u16(33).u8(1 /* file */).u8(10 /* FileRenameInformation */).u32(static_cast<std::uint32_t>(info.b.size()));
    b.u16(64 + 32).u16(0).u32(0).bytes(o.id.bytes, 16).bytes(info.b);
    tree_id_ = o.id.tree;
    auto r = call(kSetInfo, b.b);
    Status closed = close(o.id);
    if (!r.ok()) return r.error();
    if (r->status != kSuccess) return smb_status("cannot rename " + from, r->status);
    return closed;
}

Status Client::remove(const std::string& path) {
    ALEMBIC_ASSIGN(Opened o, create(path, kDelete | kReadAttributes | kSynchronize, kOpen, kNonDirectoryFile | kDeleteOnClose));
    return close(o.id);
}

// ---------------------------------------------------------------- reading

Status Client::fill_reads() {
    const std::uint32_t size = chunk(max_read_);
    while (reads_.size() < kDepth && read_next_ < read_size_) {
        // Without credits for another request, wait for the ones in flight.
        if (!reads_.empty() && credits_ < std::max<std::uint32_t>(1, charge(size))) break;
        std::uint32_t n = static_cast<std::uint32_t>(std::min<std::uint64_t>(size, read_size_ - read_next_));
        Out b;
        b.u16(49).u8(0x50).u8(0).u32(n).u64(read_next_).bytes(read_file_.bytes, 16).u32(0).u32(0).u32(0).u16(0).u16(0).u8(0);
        tree_id_ = read_file_.tree;
        ALEMBIC_ASSIGN(std::uint64_t id, send(kRead, b.b, n));
        reads_.push_back(id);
        read_next_ += n;
    }
    return {};
}

Status Client::start_download(const std::string& path, std::uint64_t offset) {
    if (reading_) ALEMBIC_TRY(stop_download());
    ALEMBIC_ASSIGN(Opened o, create(path, kReadData | kReadAttributes | kSynchronize, kOpen, kNonDirectoryFile));
    reading_ = true;
    read_file_ = o.id;
    read_size_ = o.size;
    read_next_ = std::min(offset, o.size);
    read_data_.clear();
    read_at_ = 0;
    return fill_reads();
}

Result<std::size_t> Client::read(MutableBytes out) {
    if (!reading_) return std::size_t{0};
    while (read_at_ >= read_data_.size()) {
        if (reads_.empty()) return std::size_t{0}; // the end of the file
        std::uint64_t id = reads_.front();
        reads_.pop_front();
        ALEMBIC_ASSIGN(Message r, wait(id));
        if (r.status == kEndOfFile) { // the file got shorter
            read_next_ = read_size_;
            continue;
        }
        if (r.status != kSuccess) return smb_status("reading failed", r.status);
        std::size_t data_at = r.bytes.size() > 66 ? r.bytes[64 + 2] : 0;
        std::size_t length = get32(r.bytes, 64 + 4);
        if (data_at < 64 || data_at + length > r.bytes.size()) return smb_error("malformed READ reply");
        read_data_.assign(r.bytes.begin() + data_at, r.bytes.begin() + data_at + length);
        read_at_ = 0;
        ALEMBIC_TRY(fill_reads());
    }
    std::size_t n = std::min(out.size(), read_data_.size() - read_at_);
    std::memcpy(out.data(), read_data_.data() + read_at_, n);
    read_at_ += n;
    return n;
}

Status Client::stop_download() {
    if (!reading_) return {};
    reading_ = false;
    // The replies of reads in flight are drained so that the next ones match.
    while (!reads_.empty()) {
        std::uint64_t id = reads_.front();
        reads_.pop_front();
        ALEMBIC_ASSIGN(Message r, wait(id));
        (void)r;
    }
    read_data_.clear();
    read_at_ = 0;
    return close(read_file_);
}

// ---------------------------------------------------------------- writing

Status Client::start_upload(const std::string& path) {
    if (reading_) ALEMBIC_TRY(stop_download());
    if (writing_) ALEMBIC_TRY(finish_upload());
    ALEMBIC_ASSIGN(Opened o, create(path, kWriteData | kReadAttributes | kWriteAttributes | kSynchronize, kOverwriteIf,
                                    kNonDirectoryFile, kAttributeNormal));
    writing_ = true;
    write_file_ = o.id;
    write_offset_ = 0;
    write_buffer_.clear();
    return {};
}

Status Client::finish_write(std::uint64_t id) {
    ALEMBIC_ASSIGN(Message r, wait(id));
    if (r.status != kSuccess) return smb_status("writing failed", r.status);
    return {};
}

Status Client::send_write(const std::uint8_t* data, std::size_t size) {
    while (writes_.size() >= kDepth) {
        std::uint64_t id = writes_.front();
        writes_.pop_front();
        ALEMBIC_TRY(finish_write(id));
    }
    Out b;
    b.b.reserve(48 + size);
    b.u16(49).u16(64 + 48).u32(static_cast<std::uint32_t>(size)).u64(write_offset_).bytes(write_file_.bytes, 16);
    b.u32(0).u32(0).u16(0).u16(0).u32(0).bytes(data, size);
    tree_id_ = write_file_.tree;
    ALEMBIC_ASSIGN(std::uint64_t id, send(kWrite, b.b, size));
    writes_.push_back(id);
    write_offset_ += size;
    return {};
}

Status Client::write(Bytes data) {
    if (!writing_) return smb_error("no upload is open");
    const std::size_t size = chunk(max_write_);
    std::size_t at = 0;
    // Fill the buffer to a whole chunk, then send whole chunks straight from `data`.
    if (!write_buffer_.empty()) {
        std::size_t n = std::min(size - write_buffer_.size(), data.size());
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
        std::uint64_t id = writes_.front();
        writes_.pop_front();
        Status w = finish_write(id);
        if (s.ok() && !w.ok()) s = w;
    }
    Status c = close(write_file_);
    if (!s.ok()) return s;
    return c;
}

} // namespace

Result<std::unique_ptr<netfs::Session>> open(const netfs::Server& server) {
    ALEMBIC_ASSIGN(auto c, Client::connect(server));
    return std::unique_ptr<netfs::Session>(std::move(c));
}

} // namespace alembic::engine::smb
