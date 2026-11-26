// SPDX-License-Identifier: GPL-3.0-or-later
#include "experiment.hpp"

#include "alembic/io.hpp"
#include "platform.hpp"

#include "alembic/convert.hpp"
#include "alembic/fpkg_install.hpp"
#include "fs_ops.hpp"

#include "sqlite3.h"

#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

namespace alembic::engine::experiment {
namespace {

std::mutex g_mutex;
bool g_running = false;
JsonValue g_last = JsonValue::object();
std::string g_current;
std::uint64_t g_bytes = 0;

void set_current(const std::string& what, std::uint64_t bytes) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_current = what;
    g_bytes = bytes;
}

std::uint32_t mode_of(const JsonValue& op, std::uint32_t fallback) {
    const JsonValue& m = op["mode"];
    if (m.type() == JsonValue::Type::string) return static_cast<std::uint32_t>(std::strtoul(m.str().c_str(), nullptr, 8));
    if (m.type() == JsonValue::Type::number) return static_cast<std::uint32_t>(m.integer(fallback));
    return fallback;
}

std::string sqlite_error(sqlite3* db, const std::string& what) {
    return what + ": " + (db ? sqlite3_errmsg(db) : "cannot open");
}

struct Db {
    sqlite3* db = nullptr;
    ~Db() {
        if (db) sqlite3_close_v2(db);
    }
};

bool hex_bytes(const std::string& hex, std::vector<std::uint8_t>& out) {
    if (hex.size() % 2) return false;
    auto v = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
    out.clear();
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        int a = v(hex[i]), b = v(hex[i + 1]);
        if (a < 0 || b < 0) return false;
        out.push_back(static_cast<std::uint8_t>(a * 16 + b));
    }
    return true;
}

// One "sql" op: its statements in one transaction, committed unless dry.
std::string run_sql(const JsonValue& op, bool dry, JsonValue& result) {
    const std::string path = op["db"].str();
    if (path.empty()) return "sql: no db";
    if (!stat_path(path).is_file()) return "sql: no database at " + path; // never creates one
    Db d;
    if (sqlite3_open_v2(path.c_str(), &d.db, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) return sqlite_error(d.db, "sql: open " + path);
    sqlite3_busy_timeout(d.db, 10000);
    if (sqlite3_exec(d.db, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) != SQLITE_OK) return sqlite_error(d.db, "sql: begin");
    auto rollback = [&](std::string why) {
        sqlite3_exec(d.db, "ROLLBACK", nullptr, nullptr, nullptr);
        return why;
    };
    JsonValue changes = JsonValue::array();
    int index = 0;
    for (const JsonValue& st : op["statements"].items()) {
        const std::string sql = st["sql"].str();
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(d.db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
            return rollback(sqlite_error(d.db, "sql: statement " + std::to_string(index)));
        int n = 1;
        std::string bad;
        for (const JsonValue& p : st["params"].items()) {
            int rc = SQLITE_OK;
            switch (p.type()) {
            case JsonValue::Type::null: rc = sqlite3_bind_null(stmt, n); break;
            case JsonValue::Type::boolean: rc = sqlite3_bind_int(stmt, n, p.boolean() ? 1 : 0); break;
            case JsonValue::Type::number:
                if (static_cast<double>(p.integer()) == p.number()) rc = sqlite3_bind_int64(stmt, n, p.integer());
                else rc = sqlite3_bind_double(stmt, n, p.number());
                break;
            case JsonValue::Type::string: rc = sqlite3_bind_text(stmt, n, p.str().c_str(), -1, SQLITE_TRANSIENT); break;
            case JsonValue::Type::object: {
                std::vector<std::uint8_t> blob;
                if (!hex_bytes(p["blob_hex"].str(), blob)) bad = "a parameter object needs blob_hex";
                else rc = sqlite3_bind_blob(stmt, n, blob.data(), static_cast<int>(blob.size()), SQLITE_TRANSIENT);
                break;
            }
            default: bad = "parameters are null, numbers, strings or {blob_hex}";
            }
            if (!bad.empty() || rc != SQLITE_OK) break;
            ++n;
        }
        if (!bad.empty()) {
            sqlite3_finalize(stmt);
            return rollback("sql: statement " + std::to_string(index) + ": " + bad);
        }
        int rc;
        while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        }
        sqlite3_finalize(stmt);
        if (rc != SQLITE_DONE) return rollback(sqlite_error(d.db, "sql: statement " + std::to_string(index)));
        const int changed = sqlite3_changes(d.db);
        changes.push(changed);
        if (st.has("changes") && st["changes"].integer(-1) != changed)
            return rollback("sql: statement " + std::to_string(index) + " changed " + std::to_string(changed) + " rows, not " +
                            std::to_string(st["changes"].integer(-1)));
        ++index;
    }
    result.set("changes", std::move(changes));
    if (dry) {
        rollback("");
        result.set("rolled_back", true);
        return {};
    }
    if (sqlite3_exec(d.db, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK) return rollback(sqlite_error(d.db, "sql: commit"));
    return {};
}

std::string run_backup(const JsonValue& op, bool dry) {
    const std::string from = op["db"].str(), to = op["to"].str();
    if (!stat_path(from).is_file()) return "backup_db: no database at " + from;
    if (stat_path(to, false).kind != PathKind::missing) return "backup_db: " + to + " exists already";
    if (dry) return {};
    if (auto s = make_parent_directories(to); !s.ok()) return s.error().message;
    Db src, dst;
    if (sqlite3_open_v2(from.c_str(), &src.db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) return sqlite_error(src.db, "backup_db: open " + from);
    sqlite3_busy_timeout(src.db, 10000);
    if (sqlite3_open_v2(to.c_str(), &dst.db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK)
        return sqlite_error(dst.db, "backup_db: create " + to);
    sqlite3_backup* b = sqlite3_backup_init(dst.db, "main", src.db, "main");
    if (!b) return sqlite_error(dst.db, "backup_db");
    int rc;
    while ((rc = sqlite3_backup_step(b, -1)) == SQLITE_BUSY || rc == SQLITE_LOCKED) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    sqlite3_backup_finish(b);
    if (rc != SQLITE_DONE) return sqlite_error(dst.db, "backup_db: copy");
    return {};
}

std::string run_copy(const JsonValue& op, bool dry, JsonValue& result) {
    const std::string path = op["path"].str(), from = op["from"].str();
    if (!is_absolute_path(path) || from.empty()) return "copy: path and from";
    if (stat_path(path, false).kind != PathKind::missing) return "copy: " + path + " exists already";
    auto in = File::open(from, File::Mode::read);
    if (!in.ok()) return "copy: " + in.error().message;
    auto size = in->size();
    if (!size.ok()) return "copy: " + size.error().message;
    const std::uint64_t offset = static_cast<std::uint64_t>(op["offset"].integer(0));
    const std::uint64_t length = op.has("length") ? static_cast<std::uint64_t>(op["length"].integer(0)) : *size - std::min(offset, *size);
    if (offset > *size || length > *size - offset) return "copy: " + from + " is shorter than offset + length";
    result.set("bytes", length);
    if (dry) return {};
    auto out = AtomicOutput::create(path);
    if (!out.ok()) return "copy: " + out.error().message;
    std::vector<std::uint8_t> buffer(4u << 20);
    for (std::uint64_t done = 0; done < length;) {
        const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), length - done));
        if (auto s = in->read_at(offset + done, MutableBytes(buffer.data(), n)); !s.ok()) return "copy: " + s.error().message;
        if (auto s = out->file().write_at(done, Bytes(buffer.data(), n)); !s.ok()) return "copy: " + s.error().message;
        done += n;
        set_current("copy " + path, done);
    }
    if (auto s = out->file().sync(); !s.ok()) return "copy: " + s.error().message;
    if (auto s = out->commit(); !s.ok()) return "copy: " + s.error().message;
    if (op.has("mode"))
        if (auto s = set_mode(path, mode_of(op, 0644)); !s.ok()) return "copy: " + s.error().message;
    return {};
}

std::string run_mkdir(const JsonValue& op, bool dry, JsonValue& result) {
    const std::string path = op["path"].str();
    if (!is_absolute_path(path)) return "mkdir: an absolute path";
    PathStat st = stat_path(path, false);
    if (st.is_directory()) {
        result.set("existed", true);
        return {};
    }
    if (st.kind != PathKind::missing) return "mkdir: " + path + " is not a folder";
    if (dry) return {};
    if (auto s = make_directory(path); !s.ok()) return "mkdir: " + s.error().message;
    if (op.has("mode"))
        if (auto s = set_mode(path, mode_of(op, 0755)); !s.ok()) return "mkdir: " + s.error().message;
    return {};
}

std::string run_remove(const JsonValue& op, bool dry, JsonValue& result) {
    const std::string path = op["path"].str();
    if (!is_absolute_path(path)) return "remove: an absolute path";
    PathStat st = stat_path(path, false);
    if (st.kind == PathKind::missing) {
        result.set("missing", true);
        return {};
    }
    if (dry) return {};
    Status s = st.is_directory() ? remove_directory(path) : remove_file(path);
    return s.ok() ? std::string() : "remove: " + s.error().message;
}

// sceAppInstUtilAppInstallAll: what the system then shows (NOTES: the
// home screen after a replay). A dry run calls nothing.
std::string run_install_all(bool dry, JsonValue& result) {
    if (dry) return {};
    auto rc = platform::experiment_install_all();
    if (!rc.ok()) return "install_all: " + rc.error().message;
    result.set("rc", static_cast<std::int64_t>(static_cast<std::uint32_t>(rc.value())));
    return {};
}

// sceAppInstUtilAppInstallPkg on a package on the console.
std::string run_install_pkg(const JsonValue& op, bool dry, JsonValue& result) {
    const std::string path = op["path"].str();
    if (!is_absolute_path(path) || stat_path(path, true).kind != PathKind::file) return "install_pkg: no package at " + path;
    if (dry) return {};
    auto r = platform::experiment_install_pkg(path, op["shellcore_auth"].boolean());
    if (!r.ok()) return "install_pkg: " + r.error().message;
    result.set("rc", static_cast<std::int64_t>(static_cast<std::uint32_t>(r.value().rc)));
    result.set("content_id", r.value().content_id);
    result.set("type", static_cast<std::int64_t>(r.value().type));
    result.set("platform", static_cast<std::int64_t>(r.value().platform));
    return {};
}

// {"op":"symbols", "module", "path", "names": [...]}: which of them it exports.
std::string run_symbols(const JsonValue& op, JsonValue& result) {
    std::vector<std::string> names;
    for (const JsonValue& n : op["names"].items()) names.push_back(n.str());
    auto found = platform::experiment_symbols(op["module"].str(), op["path"].str(), names);
    if (!found.ok()) return "symbols: " + found.error().message;
    JsonValue have = JsonValue::object();
    for (std::size_t i = 0; i < names.size(); ++i) have.set(names[i], static_cast<bool>(found.value()[i]));
    result.set("found", std::move(have));
    return {};
}

// An image (any convert input) written straight into the installed form
// (fpkg_install.hpp) and registered with sceAppInstUtilAppInstallPkg on its
// own app.pkg: the package is never a file of its own.
std::string run_install_image(const JsonValue& op, bool dry, JsonValue& result) {
    const std::string input = op["input"].str();
    const std::string keys = op.has("keys") ? op["keys"].str() : platform::default_state_dir() + "/keys";
    const std::string root = op["root"].str(); // tests on a PC: every target under this folder
    const std::string staging = root + "/user/app/.alembic-install";
    if (!is_absolute_path(input) || stat_path(input, true).kind == PathKind::missing) return "install_image: no input at " + input;
    if (!is_regular_file(keys + "/passcode.bin")) return "install_image: no package keys in " + keys;
    if (dry) return {};
    if (stat_path(staging, false).kind != PathKind::missing) (void)remove_tree(staging);
    if (auto s = make_directory(staging); !s.ok()) return "install_image: " + s.error().message;
    convert::Options options;
    options.overwrite = true;
    options.fpkg.keys_folder = keys;
    options.fpkg.license_keys_file = keys + "/ps4/Keys.cs";
    options.fpkg_ps4.keys_file = options.fpkg.license_keys_file;
    options.fpkg.si_path = staging + "/app.si";
    auto report = convert::run(input, staging + "/app.pkg", convert::Format::fpkg, options, [](const ProgressEvent& e) {
        set_current(std::string("install_image: ") + std::string(e.stage), e.done);
        return true;
    });
    if (!report.ok()) return "install_image: " + report.error().message;
    auto in = fpkg::install::read_inputs(staging + "/app.pkg", staging + "/app.si");
    if (!in.ok()) return "install_image: " + in.error().message;
    in->title_name = op["title_name"].str();
    const std::string app = root + "/user/app/" + in->title_id, cid = in->content_id;
    const std::string playgo = root + "/user/playgo/app/" + cid;
    auto files = fpkg::install::make_files(*in, op.has("url") ? op["url"].str() : "/user/app/" + in->title_id + "/app.pkg");
    if (!files.ok()) return "install_image: " + files.error().message;
    result.set("title_id", in->title_id);
    result.set("content_id", cid);
    result.set("app_pkg", in->app_pkg_size);
    result.set("app_si", in->si_size);
    if (stat_path(app, false).kind != PathKind::missing || stat_path(playgo, false).kind != PathKind::missing)
        return "install_image: " + app + " or " + playgo + " is already there (uninstall it first)";
    for (const std::string& d : {app, playgo, playgo + "/00", playgo + "/00/config", playgo + "/00/config/" + cid}) {
        if (auto s = make_directory(d); !s.ok()) return "install_image: " + s.error().message;
        if (auto s = set_mode(d, 0770); !s.ok()) return "install_image: " + s.error().message;
    }
    for (auto [from, to] : {std::pair<std::string, std::string>{staging + "/app.pkg", app + "/app.pkg"},
                            {staging + "/app.si", playgo + "/00/app.si"}}) {
        if (auto s = rename_path(from, to); !s.ok()) return "install_image: " + s.error().message;
        if (auto s = set_mode(to, 0770); !s.ok()) return "install_image: " + s.error().message;
    }
    (void)remove_directory(staging);
    const std::vector<std::uint8_t> none;
    for (auto [path, data] : {std::pair<std::string, const std::vector<std::uint8_t>*>{app + "/app.json", &files->app_json},
                              {app + "/app.pbm", &files->app_pbm}, {app + "/app.pbm.backup", &files->app_pbm},
                              {app + "/app.xml", &files->app_xml}, {app + "/app.crc", &files->app_crc},
                              {playgo + "/00/d0.pdb", &files->d0_pdb}, {playgo + "/00/d1.pdb", &files->d1_pdb},
                              {playgo + "/00/f0.pdb", &none}}) {
        auto out = AtomicOutput::create(path, false);
        if (!out.ok()) return "install_image: " + out.error().message;
        Status s = data->empty() ? Status{} : out->file().write_at(0, *data);
        if (s.ok()) s = out->file().sync();
        if (s.ok()) s = out->commit();
        if (s.ok()) s = set_mode(path, 0770);
        if (!s.ok()) return "install_image: " + s.error().message;
    }
    if (op.has("register") && !op["register"].boolean()) return {};
    auto r = platform::experiment_install_pkg(app + "/app.pkg", op["shellcore_auth"].boolean());
    if (!r.ok()) return "install_image: " + r.error().message;
    result.set("rc", static_cast<std::int64_t>(static_cast<std::uint32_t>(r.value().rc)));
    return {};
}

void sleep_ms(const JsonValue& op) {
    std::this_thread::sleep_for(std::chrono::milliseconds(op["ms"].integer(0)));
}

} // namespace

JsonValue run(const JsonValue& body) {
    const bool dry = body["dry_run"].boolean();
    JsonValue out = JsonValue::object();
    out.set("dry_run", dry);
    JsonValue done = JsonValue::array();
    std::string error;
    int i = 0;
    for (const JsonValue& op : body["ops"].items()) {
        const std::string kind = op["op"].str();
        JsonValue r = JsonValue::object();
        r.set("i", i);
        r.set("op", kind);
        if (op.has("path")) r.set("path", op["path"].str());
        if (op.has("db")) r.set("db", op["db"].str());
        set_current(kind + " " + op["path"].str() + op["db"].str(), 0);
        if (kind == "sql") error = run_sql(op, dry, r);
        else if (kind == "backup_db") error = run_backup(op, dry);
        else if (kind == "copy") error = run_copy(op, dry, r);
        else if (kind == "mkdir") error = run_mkdir(op, dry, r);
        else if (kind == "remove") error = run_remove(op, dry, r);
        else if (kind == "install_all") error = run_install_all(dry, r);
        else if (kind == "install_image") error = run_install_image(op, dry, r);
        else if (kind == "install_pkg") error = run_install_pkg(op, dry, r);
        else if (kind == "symbols") error = run_symbols(op, r);
        else if (kind == "sleep") sleep_ms(op);
        else error = "unknown op '" + kind + "'";
        if (!error.empty()) r.set("error", error);
        done.push(std::move(r));
        if (!error.empty()) break;
        ++i;
    }
    out.set("ops", std::move(done));
    out.set("ok", error.empty());
    if (!error.empty()) out.set("error", error);
    return out;
}

std::string start(const JsonValue& body) {
    if (!body.is_object() || body["ops"].type() != JsonValue::Type::array) return "a body {\"dry_run\", \"ops\": [...]}";
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_running) return "an experiment is running";
        g_running = true;
        g_last = JsonValue::object();
    }
    std::thread([body] {
        JsonValue r = run(body);
        std::lock_guard<std::mutex> lock(g_mutex);
        g_last = std::move(r);
        g_running = false;
        g_current.clear();
    }).detach();
    return {};
}

JsonValue status() {
    std::lock_guard<std::mutex> lock(g_mutex);
    JsonValue v = JsonValue::object();
    v.set("running", g_running);
    if (g_running) {
        v.set("current", g_current);
        v.set("bytes", g_bytes);
    } else {
        v.set("last", g_last);
    }
    return v;
}

} // namespace alembic::engine::experiment
