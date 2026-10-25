// SPDX-License-Identifier: GPL-3.0-or-later
// Alembic's console app: opens the display, controller and sounds, then
// draws the app every frame. The work is done by the engine payload
// (alembic.elf), reached over HTTP on this console; when none answers, the
// app sends the one it carries to the ELF loader. Adapted from
// ps5-homebrew-ui's src/main.cpp (GPL-3.0).

#include "app/app.hpp"
#include "app/image.hpp"
#include "audio/cues.hpp"
#include "audio/mixer.hpp"
#include "core/input.hpp"
#include "core/save_file.hpp"
#include "app_version.h"
#include "engine/client.hpp"
#include "engine/http.hpp"
#include "gfx/renderer.hpp"
#include "platform/ps5/audio_out.hpp"
#include "platform/ps5/display_egl.hpp"
#include "platform/ps5/pad.hpp"
#include "platform/ps5/system.hpp"
#include "ui/fonts.hpp"
#include "ui/ui.hpp"

#include <GL/glcorearb.h>

#include <cstdint>
#include <span>
#include <string>

namespace {

constexpr const char* kAssets = "/app0/assets";
// The app's own small files (preferences) live in the title's save area.
constexpr const char* kDataRoot = "/download0/alembic";
constexpr std::uint16_t kEnginePort = 5940;
constexpr std::uint16_t kLoaderPort = 9021;

bool load_font(hui::gfx::Renderer& renderer, const char* name, hui::gfx::Font* font, hui::ui::FontRef* ref) {
    std::string data;
    const std::string path = std::string(kAssets) + "/fonts/" + name;
    if (!hui::save::read_file(path, &data, 16u << 20) || !font->load(data)) {
        hui::sys::log("[ALEMBIC] font %s failed: %s", name, font->error().c_str());
        return false;
    }
    ref->font = font;
    ref->texture = renderer.batch().create_font_texture(*font);
    return true;
}

}  // namespace

int main() {
    using namespace hui;
    sys::log("[ALEMBIC] entry");
    save::ensure_directory(kDataRoot);

    ps5::Display display;
    if (!display.open(1920, 1080)) {
        sys::log("[ALEMBIC] fatal: display open failed");
        sys::park();
    }
    gfx::Renderer renderer;
    gfx::Font regular, semibold, mono;
    ui::Fonts fonts;
    if (!renderer.init() || !load_font(renderer, "inter-regular.huifont", &regular, &fonts.regular) ||
        !load_font(renderer, "inter-semibold.huifont", &semibold, &fonts.semibold) ||
        !load_font(renderer, "dejavu-sans-mono.huifont", &mono, &fonts.mono)) {
        sys::log("[ALEMBIC] fatal: renderer init failed");
        sys::park();
    }
    // Faces the kit's components may ask for.
    fonts.display = fonts.semibold;
    fonts.pixel = fonts.regular;
    fonts.hand = fonts.regular;

    ps5::Pad pad;
    pad.open();
    InputTracker tracker;
    audio::Mixer mixer;
    ps5::AudioOut audio_out;
    audio_out.start(mixer);
    audio::SoundBank sounds;
    sounds.load(std::string(kAssets) + "/audio/sfx");

    alembic::app::Engine engine;
    engine.start("127.0.0.1", kEnginePort);

    alembic::app::Platform platform;
    platform.console = true;
    platform.version = ALEMBIC_APP_VERSION;
    alembic::app::set_user_agent(std::string("Alembic-app/") + ALEMBIC_APP_VERSION);
    platform.make_texture = [&renderer](int w, int h, const std::uint8_t* rgba) {
        return renderer.batch().create_texture(w, h, rgba);
    };
    platform.free_texture = [](std::uint32_t texture) {
        GLuint name = texture;
        glDeleteTextures(1, &name);
    };
    platform.start_engine = [](std::string* why) {
        std::string elf;
        if (!save::read_file(std::string(kAssets) + "/alembic.elf", &elf, 64u << 20) || elf.size() < 64) {
            *why = "This app carries no engine. Load alembic.elf with your payload loader.";
            return false;
        }
        std::string error;
        if (!alembic::app::send_bytes("127.0.0.1", kLoaderPort, elf, &error)) {
            *why = "No ELF loader answers on port 9021 (" + error + "). Load alembic.elf with your payload loader.";
            return false;
        }
        sys::log("[ALEMBIC] engine sent to the ELF loader (%zu bytes)", elf.size());
        return true;
    };
    platform.quit = [&pad] {
        sys::log("[ALEMBIC] closing");
        // The engine may now swap the app's image (an update waiting for it).
        (void)alembic::app::http_request("127.0.0.1", kEnginePort, "POST", "/api/app/closing", "{}", 800);
        pad.close();
        sys::quit();
    };
    platform.read_file = [](const std::string& name, std::string* data) {
        return save::read_file(std::string(kDataRoot) + "/" + name, data);
    };
    platform.write_file = [](const std::string& name, const std::string& data) {
        return save::write_atomic(std::string(kDataRoot) + "/" + name, data).empty();
    };
    {
        std::string png;
        alembic::app::Image logo;
        if (save::read_file("/app0/sce_sys/icon0.png", &png) && alembic::app::decode_png(png, 128, &logo))
            platform.logo = renderer.batch().create_texture(logo.width, logo.height, logo.rgba.data());
    }

    alembic::app::App app(engine, platform);
    alembic::app::Ui ui;
    ui.fonts = &fonts;
    ui::Feedback feedback;
    ui.feedback = &feedback;
    gfx::DrawList list;
    ui.list = &list;

    PadSample samples[64];
    std::int64_t last = sys::monotonic_us();
    std::uint64_t frames = 0;
    for (;;) {
        const std::int64_t now = sys::monotonic_us();
        float dt = frames == 0 ? 1.0f / 60.0f : static_cast<float>(now - last) / 1e6f;
        last = now;
        if (dt > 0.05f) dt = 0.05f;
        // Consoles set to confirm with ○: the engine reads the system setting.
        InputSettings settings = tracker.settings();
        settings.swap_confirm = app.info["enter_button"].str() == "circle";
        tracker.set_settings(settings);
        const std::size_t count = pad.read(samples);
        const InputFrame input = tracker.update(std::span<const PadSample>(samples, count), static_cast<std::uint64_t>(now));

        feedback.clear();
        list.clear();
        ui.begin_frame(input, dt);
        app.frame(ui);
        ui.end_frame();
        for (const audio::CueEvent& event : feedback.cues)
            sounds.play(mixer, event.set == audio::SoundSet::count ? audio::SoundSet::glass : event.set, event);
        if (feedback.rumble_strength > 0.0f) pad.rumble(feedback.rumble_strength, feedback.rumble_seconds);
        pad.tick(dt);

        renderer.begin();
        renderer.draw(list);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        renderer.present(0, display.width(), display.height());
        if (!display.swap()) {
            sys::log("[ALEMBIC] fatal: swap failed");
            sys::park();
        }
        if (++frames == 1) sys::hide_splash_screen();
    }
}
