// SPDX-License-Identifier: GPL-3.0-or-later
// Alembic Bench (PPSA99595): a development title, not a release. It runs the
// engine's process checks (src/engine/probe.hpp) inside a game process, asks
// the engine payload for the same report (GET /api/probe), and shows the two
// side by side: what would change if the engine's work moved into the app.
// It posts its report to the engine (GET /api/probe/app then reads it from a
// PC) and keeps both under /data/alembic-bench (else /download0).
//   app/build.sh with ALEMBIC_APP_VARIANT=bench builds it.
// △ runs it again, ○ closes it; ↑ ↓ (L1 R1 by pages) scroll.

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

struct Bench {
    std::mutex mutex;
    std::string status = "Starting";
    std::vector<Row> rows;
    std::string saved;
    std::atomic<bool> running{false};
    std::thread worker;

    void set_status(const std::string& s) {
        std::lock_guard<std::mutex> lock(mutex);
        status = s;
        hui::sys::log("[BENCH] %s", s.c_str());
    }

    void start() {
        if (running.exchange(true)) return;
        if (worker.joinable()) worker.join();
        worker = std::thread([this] { run(); });
    }

    void run() {
        alembic::engine::probe::Options o;
        o.process = "app";
        o.step = [this](const std::string& s) { set_status("This app: " + s + "\xE2\x80\xA6"); };
        const JsonValue app = alembic::engine::probe::run(o);
        const std::string app_json = app.dump();
        set_status("The engine: running its checks (up to a minute)\xE2\x80\xA6");
        JsonValue engine = JsonValue::object();
        const alembic::app::HttpReply reply =
            alembic::app::http_request("127.0.0.1", kEnginePort, "GET", "/api/probe", "", 180000, 4u << 20);
        if (reply.status == 200) {
            if (auto parsed = alembic::engine::parse_json(reply.body); parsed.ok()) engine = *parsed;
        } else if (reply.status == 404) {
            engine.set("error", "this engine has no /api/probe (older than the bench)");
        } else {
            engine.set("error", reply.status ? "HTTP " + std::to_string(reply.status) : "no engine on port 5940: " + reply.body);
        }
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
    bench.start();

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

        std::string status, saved;
        std::vector<Row> rows;
        {
            std::lock_guard<std::mutex> lock(bench.mutex);
            status = bench.status;
            saved = bench.saved;
            rows = bench.rows;
        }
        if (input.is_pressed(Action::back)) {
            pad.close();
            sys::quit();
        }
        if (input.is_pressed(Action::north)) bench.start();
        if (input.nav == Direction::down) ++scroll;
        if (input.nav == Direction::up) --scroll;
        if (input.is_pressed(Action::page_next)) scroll += visible;
        if (input.is_pressed(Action::page_prev)) scroll -= visible;
        scroll = std::clamp(scroll, 0, std::max(0, static_cast<int>(rows.size()) - visible));

        feedback.clear();
        list.clear();
        ui.begin_frame(input, dt);
        list.rounded_rect({0, 0, 1920, 1080}, 0, style::bg);
        draw::text(ui, draw::Face::semibold, "Alembic Bench", 60, 70, 40, style::text);
        draw::text(ui, draw::Face::regular,
                   status + (saved.empty() ? "" : " \xC2\xB7 " + saved) + "   \xE2\x96\xB3 again  \xE2\x97\x8B close", 60,
                   110, 22, style::muted);
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
