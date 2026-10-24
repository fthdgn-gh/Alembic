// SPDX-License-Identifier: GPL-3.0-or-later
// The app on a PC, off-screen (Mesa's surfaceless EGL, llvmpipe): runs the
// same code as the console against an engine on this computer and writes
// PNG pictures of what it draws, driven by a script of button presses.
// Adapted from ps5-homebrew-ui's host/snapshot_main.cpp (GPL-3.0).
//
//   alembic_preview <fonts dir> <icon0.png> <out dir> <host:port> <script>
//
// Script: words separated by spaces or ';' — up down left right x o s t l1
// r1 l2 r2 options, wait:<seconds>, shot:<name>.

#include "app/app.hpp"
#include "app/image.hpp"
#include "core/save_file.hpp"
#include "engine/client.hpp"
#include "gfx/canvas.hpp"
#include "gfx/renderer.hpp"
#include "ui/fonts.hpp"
#include "ui/ui.hpp"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/glcorearb.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../../../third_party/ps5-homebrew-ui/third_party/stb/stb_image_write.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace {

bool open_context() {
    auto get_platform_display =
        reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
    EGLDisplay display = get_platform_display != nullptr
                             ? get_platform_display(EGL_PLATFORM_SURFACELESS_MESA, nullptr, nullptr)
                             : eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint major = 0, minor = 0;
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, &major, &minor) || !eglBindAPI(EGL_OPENGL_API))
        return false;
    const EGLint attributes[] = {EGL_CONTEXT_MAJOR_VERSION, 4, EGL_CONTEXT_MINOR_VERSION, 5,
                                 EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE};
    EGLContext context = eglCreateContext(display, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, attributes);
    return context != EGL_NO_CONTEXT && eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context);
}

bool load_font(hui::gfx::Renderer& renderer, const std::string& path, hui::gfx::Font* font, hui::ui::FontRef* ref) {
    std::string data;
    if (!hui::save::read_file(path, &data, 16u << 20) || !font->load(data)) {
        std::fprintf(stderr, "cannot load font %s\n", path.c_str());
        return false;
    }
    ref->font = font;
    ref->texture = renderer.batch().create_font_texture(*font);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 6) {
        std::fprintf(stderr, "usage: %s <fonts dir> <icon0.png> <out dir> <host:port> <script>\n", argv[0]);
        return 2;
    }
    const std::string fonts_dir = argv[1], icon_path = argv[2], out = argv[3], address = argv[4];
    std::string script;
    for (int i = 5; i < argc; ++i) script += std::string(argv[i]) + " ";
    const int width = 1920, height = 1080;
    if (!open_context()) {
        std::fprintf(stderr, "no OpenGL context (Mesa surfaceless EGL)\n");
        return 1;
    }
    hui::gfx::Renderer renderer;
    hui::gfx::Font regular, semibold, mono;
    hui::ui::Fonts fonts;
    if (!renderer.init() || !load_font(renderer, fonts_dir + "/inter-regular.huifont", &regular, &fonts.regular) ||
        !load_font(renderer, fonts_dir + "/inter-semibold.huifont", &semibold, &fonts.semibold) ||
        !load_font(renderer, fonts_dir + "/dejavu-sans-mono.huifont", &mono, &fonts.mono))
        return 1;
    fonts.display = fonts.semibold;
    fonts.pixel = fonts.regular;
    fonts.hand = fonts.regular;

    const std::size_t colon = address.rfind(':');
    alembic::app::Engine engine;
    engine.start(address.substr(0, colon), static_cast<std::uint16_t>(std::atoi(address.c_str() + colon + 1)));

    std::map<std::string, std::string> files;  // preferences, in memory
    alembic::app::Platform platform;
    platform.version = "preview";
    platform.make_texture = [&renderer](int w, int h, const std::uint8_t* rgba) {
        return renderer.batch().create_texture(w, h, rgba);
    };
    platform.free_texture = [](std::uint32_t texture) {
        GLuint name = texture;
        glDeleteTextures(1, &name);
    };
    platform.read_file = [&files](const std::string& name, std::string* data) {
        auto it = files.find(name);
        if (it == files.end()) return false;
        *data = it->second;
        return true;
    };
    platform.write_file = [&files](const std::string& name, const std::string& data) {
        files[name] = data;
        return true;
    };
    {
        std::string png;
        alembic::app::Image logo;
        if (hui::save::read_file(icon_path, &png) && alembic::app::decode_png(png, 128, &logo))
            platform.logo = renderer.batch().create_texture(logo.width, logo.height, logo.rgba.data());
    }
    alembic::app::App app(engine, platform);
    alembic::app::Ui ui;
    ui.fonts = &fonts;
    hui::ui::Feedback feedback;
    ui.feedback = &feedback;
    hui::gfx::DrawList list;
    ui.list = &list;
    hui::gfx::Canvas canvas;
    canvas.create(width, height, 1);

    // The script, word by word.
    std::vector<std::string> words;
    {
        std::string w;
        for (char c : script) {
            if (c == ' ' || c == ';' || c == '\n') {
                if (!w.empty()) words.push_back(w);
                w.clear();
            } else {
                w += c;
            }
        }
    }
    const std::map<std::string, hui::Action> actions = {
        {"x", hui::Action::confirm},     {"o", hui::Action::back},         {"s", hui::Action::west},
        {"t", hui::Action::north},       {"l1", hui::Action::page_prev},   {"r1", hui::Action::page_next},
        {"l2", hui::Action::jump_prev},  {"r2", hui::Action::jump_next},   {"options", hui::Action::menu}};
    const std::map<std::string, hui::Direction> dirs = {{"up", hui::Direction::up},
                                                       {"down", hui::Direction::down},
                                                       {"left", hui::Direction::left},
                                                       {"right", hui::Direction::right}};
    std::size_t at = 0;
    double wait = 1.5;  // let the first replies arrive
    const float dt = 1.0f / 60.0f;
    for (;;) {
        hui::InputFrame input;
        input.connected = true;
        if (wait <= 0.0) {
            if (at >= words.size()) break;
            const std::string w = words[at++];
            if (w.compare(0, 5, "wait:") == 0) {
                wait = std::atof(w.c_str() + 5);
            } else if (w.compare(0, 5, "shot:") == 0) {
                // Drawn below, after this frame.
                wait = -1.0;
                at--;
            } else if (actions.count(w)) {
                input.pressed = hui::action_bit(actions.at(w));
                wait = 0.25;
            } else if (dirs.count(w)) {
                input.nav = dirs.at(w);
                wait = 0.25;
            } else {
                std::fprintf(stderr, "unknown script word: %s\n", w.c_str());
            }
        }
        feedback.clear();
        list.clear();
        ui.begin_frame(input, dt);
        app.frame(ui);
        ui.end_frame();
        renderer.begin();
        renderer.draw(list);
        canvas.bind();
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        renderer.present(canvas.framebuffer(), width, height);
        if (wait < 0.0) {
            const std::string name = words[at++].substr(5);
            std::vector<unsigned char> pixels(static_cast<std::size_t>(width) * height * 4);
            glBindFramebuffer(GL_FRAMEBUFFER, canvas.framebuffer());
            glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
            stbi_flip_vertically_on_write(1);
            const std::string path = out + "/" + name + ".png";
            stbi_write_png(path.c_str(), width, height, 4, pixels.data(), width * 4);
            std::printf("wrote %s (%zu shapes, GL error 0x%x)\n", path.c_str(), list.instances().size(), glGetError());
            wait = 0.0;
        } else {
            wait -= dt;
        }
        // Real time, so the engine's replies arrive as they would.
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    engine.stop();
    return 0;
}
