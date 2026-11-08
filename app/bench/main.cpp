// SPDX-License-Identifier: GPL-3.0-or-later
// Probe Bench (dev) (PPSA99595): a development title, not a release. It runs the
// engine's process checks (src/engine/probe.hpp) inside a game process, asks
// the engine payload for the same report (GET /api/probe), and shows the two
// side by side: what would change if the engine's work moved into the app.
// It posts its report to the engine (GET /api/probe/app then reads it from a
// PC) and keeps both under /data/alembic-bench (else /download0).
//   app/build.sh with ALEMBIC_APP_VARIANT=bench builds it.
// It carries the engine payload it was built with (assets/alembic.elf): when
// no engine answers it sends that one to the ELF loader; when the running one
// has no /api/probe, □ sends it (on the console a new engine takes over the
// port from the old one, whatever their versions).
// After a crash the next start shows the check that crashed and skips it
// (breadcrumbs in /download0/alembic-bench; OPTIONS forgets them).
// △ runs it again, □ replaces an old engine, ○ closes it; ↑ ↓ (L1 R1 by pages) scroll.

#include "app_version.h"
#include "core/input.hpp"
#include "core/save_file.hpp"
#include "engine/http.hpp"
#include "gfx/renderer.hpp"
#include "json_value.hpp"
#include "platform/ps5/display_egl.hpp"
#include "platform/ps5/pad.hpp"
#include "platform/ps5/system.hpp"
#include "probe.hpp"
#include "ui/draw.hpp"
#include "ui/fonts.hpp"
#include "ui/style.hpp"
#include "ui/ui.hpp"

#include <GL/glcorearb.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <cstdio>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace {

using alembic::engine::JsonValue;
namespace draw = alembic::app::draw;
namespace style = alembic::app::style;

constexpr const char* kAssets = "/app0/assets";
constexpr std::uint16_t kEnginePort = 5940;
constexpr std::uint16_t kLoaderPort = 9021;

// Sends the bench's engine to the ELF loader and waits until an engine with
// /api/probe answers; "" when one does, else why not.
std::string send_engine(const std::function<void(const std::string&)>& status) {
    std::string elf;
    if (!hui::save::read_file(std::string(kAssets) + "/alembic.elf", &elf, 64u << 20) || elf.size() < 64)
        return "this bench carries no engine (assets/alembic.elf)";
    status("Sending the bench's engine to the ELF loader\xE2\x80\xA6");
    std::string error;
    if (!alembic::app::send_bytes("127.0.0.1", kLoaderPort, elf, &error)) return "no ELF loader on 9021: " + error;
    status("Waiting for the new engine\xE2\x80\xA6");
    const std::int64_t until = hui::sys::monotonic_us() + 30000000;
    while (hui::sys::monotonic_us() < until) {
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
        // A new engine answers 200 (a kept report) or its own 404 text; an
        // old one answers its generic 404.
        const alembic::app::HttpReply r = alembic::app::http_request("127.0.0.1", kEnginePort, "GET", "/api/probe/app", "", 1500);
        if (r.status == 200 || (r.status == 404 && r.body.find("Bench") != std::string::npos)) return "";
    }
    return "the new engine did not answer within 30 s";
}

struct Row {
    std::string key, engine, app;
    bool header = false;
};

// One line per value: "cpu.thread_cpus" -> "9,11,13-15".
void flatten(const JsonValue& v, const std::string& prefix, std::vector<std::pair<std::string, std::string>>* out) {
    switch (v.type()) {
    case JsonValue::Type::object:
        for (const auto& [k, f] : v.fields()) flatten(f, prefix.empty() ? k : prefix + "." + k, out);
        break;
    case JsonValue::Type::number: {
        char text[48];
        const double d = v.number();
        if (d == static_cast<double>(static_cast<long long>(d))) std::snprintf(text, sizeof text, "%lld", static_cast<long long>(d));
        else std::snprintf(text, sizeof text, "%.2f", d);
        out->emplace_back(prefix, text);
        break;
    }
    case JsonValue::Type::boolean: out->emplace_back(prefix, v.boolean() ? "yes" : "no"); break;
    case JsonValue::Type::string: out->emplace_back(prefix, v.str()); break;
    default: out->emplace_back(prefix, v.dump()); break;
    }
}

// Rows in the app's order, with the engine's keys it lacks after them.
std::vector<Row> compare(const JsonValue& engine, const JsonValue& app) {
    std::vector<std::pair<std::string, std::string>> e, a;
    flatten(engine, "", &e);
    flatten(app, "", &a);
    auto find = [](const std::vector<std::pair<std::string, std::string>>& in, const std::string& key) -> const std::string* {
        for (const auto& [k, v] : in)
            if (k == key) return &v;
        return nullptr;
    };
    std::vector<Row> rows;
    std::string section;
    auto add = [&](const std::string& key, const std::string* ev, const std::string* av) {
        const std::string top = key.substr(0, key.find('.'));
        if (top != section) {
            section = top;
            rows.push_back({top, "", "", true});
        }
        rows.push_back({key.find('.') == std::string::npos ? key : key.substr(key.find('.') + 1), ev ? *ev : "-",
                        av ? *av : "-", false});
    };
    for (const auto& [k, v] : a) add(k, find(e, k), &v);
    for (const auto& [k, v] : e)
        if (!find(a, k)) add(k, &v, nullptr);
    return rows;
}

// Breadcrumbs, in the title's own save area: step.txt holds the check under
// way (empty once a run ends), crashed.txt every check a run never came
// back from. A run skips those, so each crash moves the bench one step on.
constexpr const char* kCrumbs = "/download0/alembic-bench";

std::vector<std::string> lines(const std::string& text) {
    std::vector<std::string> out;
    for (std::size_t at = 0; at < text.size();) {
        std::size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        if (end > at) out.push_back(text.substr(at, end - at));
        at = end + 1;
    }
    return out;
}

struct Bench {
    std::mutex mutex;
    std::vector<std::string> crashed;  // checks to skip
    std::string status = "Starting";
    std::vector<Row> rows;
    std::string saved;
    std::atomic<bool> running{false};
    std::atomic<bool> old_engine{false};  // the running engine has no /api/probe
    std::thread worker;

    void set_status(const std::string& s) {
        std::lock_guard<std::mutex> lock(mutex);
        status = s;
        hui::sys::log("[BENCH] %s", s.c_str());
    }

    // Called once at start: the last run's unfinished check joins the list.
    void load_crumbs() {
        hui::save::ensure_directory(kCrumbs);
        std::string text, step;
        (void)hui::save::read_file(std::string(kCrumbs) + "/crashed.txt", &text);
        if (hui::save::read_file(std::string(kCrumbs) + "/step.txt", &step) && !step.empty()) {
            text += step + "\n";
            (void)hui::save::write_atomic(std::string(kCrumbs) + "/crashed.txt", text);
            (void)hui::save::write_atomic(std::string(kCrumbs) + "/step.txt", "");
        }
        std::lock_guard<std::mutex> lock(mutex);
        crashed = lines(text);
    }

    void clear_crumbs() {
        if (running) return;
        (void)hui::save::write_atomic(std::string(kCrumbs) + "/crashed.txt", "");
        std::lock_guard<std::mutex> lock(mutex);
        crashed.clear();
    }

    void start(bool replace = false) {
        if (running.exchange(true)) return;
        if (worker.joinable()) worker.join();
        worker = std::thread([this, replace] { run(replace); });
    }

    void run(bool replace) {
        std::string engine_note;
        auto status_fn = [this](const std::string& s) { set_status(s); };
        if (replace) {
            engine_note = send_engine(status_fn);
            if (engine_note.empty()) engine_note = "replaced by the bench's engine";
        }
        alembic::engine::probe::Options o;
        o.process = "app";
        o.step = [this](const std::string& s) {
            set_status("This app: " + s + "\xE2\x80\xA6");
            (void)hui::save::write_atomic(std::string(kCrumbs) + "/step.txt", s);
        };
        std::vector<std::string> skip;
        {
            std::lock_guard<std::mutex> lock(mutex);
            skip = crashed;
        }
        o.skip = [skip](const std::string& s) { return std::find(skip.begin(), skip.end(), s) != skip.end(); };
        JsonValue app = alembic::engine::probe::run(o);
        (void)hui::save::write_atomic(std::string(kCrumbs) + "/step.txt", "");
        if (!skip.empty()) {
            std::string list;
            for (const std::string& c : skip) list += (list.empty() ? "" : ", ") + c;
            app.set("crashed_before", list);
        }
        const std::string app_json = app.dump();
        set_status("The engine: running its checks (up to a minute)\xE2\x80\xA6");
        JsonValue engine = JsonValue::object();
        alembic::app::HttpReply reply =
            alembic::app::http_request("127.0.0.1", kEnginePort, "GET", "/api/probe", "", 180000, 4u << 20);
        if (reply.status == 0 && !replace) {
            // No engine at all: start the bench's own.
            engine_note = send_engine(status_fn);
            if (engine_note.empty()) {
                engine_note = "started the bench's engine";
                set_status("The engine: running its checks (up to a minute)\xE2\x80\xA6");
                reply = alembic::app::http_request("127.0.0.1", kEnginePort, "GET", "/api/probe", "", 180000, 4u << 20);
            }
        }
        old_engine = reply.status == 404;
        if (reply.status == 200) {
            if (auto parsed = alembic::engine::parse_json(reply.body); parsed.ok()) engine = *parsed;
        } else if (reply.status == 404) {
            engine.set("error", "this engine has no /api/probe (older than the bench): \xE2\x96\xA1 replaces it");
        } else {
            engine.set("error", reply.status ? "HTTP " + std::to_string(reply.status) : "no engine on port 5940: " + reply.body);
        }
        if (!engine_note.empty()) engine.set("bench_engine", engine_note);
        (void)alembic::app::http_request("127.0.0.1", kEnginePort, "POST", "/api/probe/app", app_json, 5000);
        std::string where;
        for (const char* dir : {"/data/alembic-bench", "/download0/alembic-bench"}) {
            hui::save::ensure_directory(dir);
            if (hui::save::write_atomic(std::string(dir) + "/app.json", app_json).empty() &&
                hui::save::write_atomic(std::string(dir) + "/engine.json", engine.dump()).empty()) {
                where = dir;
                break;
            }
        }
        std::vector<Row> rows = compare(engine, app);
        {
            std::lock_guard<std::mutex> lock(mutex);
            this->rows = std::move(rows);
            saved = where.empty() ? "not saved (no writable folder)" : "saved in " + where;
            status = "Done";
        }
        running = false;
    }
};

bool load_font(hui::gfx::Renderer& renderer, const char* name, hui::gfx::Font* font, hui::ui::FontRef* ref) {
    std::string data;
    if (!hui::save::read_file(std::string(kAssets) + "/fonts/" + name, &data, 16u << 20) || !font->load(data)) return false;
    ref->font = font;
    ref->texture = renderer.batch().create_font_texture(*font);
    return true;
}

}  // namespace

int main() {
    using namespace hui;
    sys::log("[BENCH] entry");
    ps5::Display display;
    if (!display.open(1920, 1080)) sys::park();
    gfx::Renderer renderer;
    gfx::Font regular, semibold, mono;
    ui::Fonts fonts;
    if (!renderer.init() || !load_font(renderer, "inter-regular.huifont", &regular, &fonts.regular) ||
        !load_font(renderer, "inter-semibold.huifont", &semibold, &fonts.semibold) ||
        !load_font(renderer, "dejavu-sans-mono.huifont", &mono, &fonts.mono)) {
        sys::log("[BENCH] fatal: renderer init failed");
        sys::park();
    }
    fonts.display = fonts.semibold;
    fonts.pixel = fonts.regular;
    fonts.hand = fonts.regular;
    ps5::Pad pad;
    pad.open();
    InputTracker tracker;

    alembic::app::set_user_agent(std::string("Alembic-bench/") + ALEMBIC_APP_VERSION);
    Bench bench;
    bench.load_crumbs();

    alembic::app::Ui ui;
    ui.fonts = &fonts;
    ui::Feedback feedback;
    ui.feedback = &feedback;
    gfx::DrawList list;
    ui.list = &list;

    constexpr float kTop = 150, kRow = 34, kBottom = 1040;
    const int visible = static_cast<int>((kBottom - kTop) / kRow);
    int scroll = 0;
    PadSample samples[64];
    std::int64_t last = sys::monotonic_us();
    std::uint64_t frames = 0;
    for (;;) {
        const std::int64_t now = sys::monotonic_us();
        float dt = frames == 0 ? 1.0f / 60.0f : static_cast<float>(now - last) / 1e6f;
        last = now;
        if (dt > 0.05f) dt = 0.05f;
        const std::size_t count = pad.read(samples);
        const InputFrame input = tracker.update(std::span<const PadSample>(samples, count), static_cast<std::uint64_t>(now));

        std::string status, saved, crashed;
        std::vector<Row> rows;
        {
            std::lock_guard<std::mutex> lock(bench.mutex);
            status = bench.status;
            saved = bench.saved;
            rows = bench.rows;
            for (const std::string& c : bench.crashed) crashed += (crashed.empty() ? "" : ", ") + c;
        }
        // The checks start once the screen is up.
        if (frames == 2) bench.start();
        if (input.is_pressed(Action::back)) {
            pad.close();
            sys::quit();
        }
        if (input.is_pressed(Action::north)) bench.start();
        if (input.is_pressed(Action::menu)) bench.clear_crumbs();
        if (input.is_pressed(Action::west) && bench.old_engine) bench.start(true);
        if (input.nav == Direction::down) ++scroll;
        if (input.nav == Direction::up) --scroll;
        if (input.is_pressed(Action::page_next)) scroll += visible;
        if (input.is_pressed(Action::page_prev)) scroll -= visible;
        scroll = std::clamp(scroll, 0, std::max(0, static_cast<int>(rows.size()) - visible));

        feedback.clear();
        list.clear();
        ui.begin_frame(input, dt);
        list.rounded_rect({0, 0, 1920, 1080}, 0, style::bg);
        draw::text(ui, draw::Face::semibold, "Probe Bench (dev)", 60, 70, 40, style::warn);
        draw::text(ui, draw::Face::regular,
                   status + (saved.empty() ? "" : " \xC2\xB7 " + saved) + "   \xE2\x96\xB3 again  " +
                       (bench.old_engine ? "\xE2\x96\xA1 replace the engine  " : "") + "\xE2\x97\x8B close", 60,
                   110, 22, style::muted);
        if (!crashed.empty())
            draw::text_fit(ui, draw::Face::regular,
                           "Crashed here before, skipped: " + crashed + "   (OPTIONS forgets them)", 1000, 70, 22, 860,
                           style::bad);
        constexpr float kKey = 60, kEngine = 720, kApp = 1320, kWidth = 560;
        draw::caps(ui, "Check", kKey, kTop - 8, 18, style::dim);
        draw::caps(ui, "Engine (payload)", kEngine, kTop - 8, 18, style::dim);
        draw::caps(ui, "App (game process)", kApp, kTop - 8, 18, style::dim);
        for (int i = 0; i < visible && scroll + i < static_cast<int>(rows.size()); ++i) {
            const Row& r = rows[static_cast<std::size_t>(scroll + i)];
            const float base = kTop + kRow * (i + 1) - 8;
            if (r.header) {
                draw::text(ui, draw::Face::semibold, r.key, kKey, base, 24, style::accent);
                continue;
            }
            const bool same = r.engine == r.app;
            draw::text_fit(ui, draw::Face::regular, r.key, kKey + 24, base, 22, kEngine - kKey - 40, style::muted);
            draw::text_fit(ui, draw::Face::mono, r.engine, kEngine, base, 20, kWidth, same ? style::text : style::warn);
            draw::text_fit(ui, draw::Face::mono, r.app, kApp, base, 20, kWidth, same ? style::text : style::teal);
        }
        ui.end_frame();
        pad.tick(dt);

        renderer.begin();
        renderer.draw(list);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        renderer.present(0, display.width(), display.height());
        if (!display.swap()) sys::park();
        if (++frames == 1) sys::hide_splash_screen();
    }
}
