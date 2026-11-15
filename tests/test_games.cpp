// SPDX-License-Identifier: GPL-3.0-or-later
// Dumping a running game: which games run, when a dump may start, its
// progress toasts, and a waiting dump never holding up the queue.
// Title IDs here are invented.
#include "games.hpp"
#include "jobs.hpp"
#include "test.hpp"

#include "alembic/io.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

using namespace alembic;
using namespace alembic::engine;

namespace {

games::Paths fake_console(const std::string& dir) {
    games::Paths p;
    p.pfsmnt = dir + "/pfsmnt";
    p.appmeta = dir + "/appmeta";
    p.user_patch = dir + "/patch";
    for (const std::string& d : {p.pfsmnt, p.appmeta, p.user_patch}) test::make_dir(d);
    return p;
}

void write_text(const std::string& path, const std::string& text) {
    test::write_file(path, std::vector<std::uint8_t>(text.begin(), text.end()));
}

} // namespace

TEST_CASE(games_title_ids) {
    CHECK(games::valid_title_id("ABCD12345"));
    CHECK(!games::valid_title_id("abcd12345"));
    CHECK(!games::valid_title_id("ABCD1234"));
    CHECK(!games::valid_title_id("ABCD123456"));
    CHECK(!games::valid_title_id("ABCD1234/"));
    CHECK(!games::valid_title_id("../../etc"));
    CHECK(!games::valid_title_id(""));
}

TEST_CASE(games_running_and_settled) {
    const std::string dir = test::make_temp_dir();
    const games::Paths p = fake_console(dir);
    // A running game's base, its update's union and its nest; another game; junk.
    for (const char* d : {"ZZZZ00001-app0", "ZZZZ00001-app0-patch0-union", "ZZZZ00001-app0-nest", "ZZZZ00002-app0", "notagame-app0"})
        test::make_dir(p.pfsmnt + "/" + d);
    CHECK(games::mounted(p) == (std::vector<std::string>{"ZZZZ00001", "ZZZZ00002"}));

    games::Watcher w(p, 30);
    CHECK(w.poll(1000));            // two games appeared
    CHECK(w.running("ZZZZ00001"));
    CHECK(!w.settled("ZZZZ00001")); // just started: not yet
    CHECK(!w.poll(1010));           // nothing new
    CHECK(w.poll(1030));            // both settled
    CHECK(w.settled("ZZZZ00001"));
    CHECK(!w.running("ZZZZ00003"));

    // A game closes: no longer running; started again, it settles anew.
    CHECK(remove_directory(p.pfsmnt + "/ZZZZ00002-app0").ok());
    CHECK(w.poll(1040));
    CHECK(!w.running("ZZZZ00002"));
    test::make_dir(p.pfsmnt + "/ZZZZ00002-app0");
    CHECK(w.poll(1050));
    CHECK(!w.settled("ZZZZ00002"));
    CHECK(w.poll(1080));
    CHECK(w.settled("ZZZZ00002"));
}

TEST_CASE(games_source_tree_and_info) {
    const std::string dir = test::make_temp_dir();
    const games::Paths p = fake_console(dir);
    bool update = true;
    CHECK(games::source_tree(p, "ZZZZ00001", &update) == p.pfsmnt + "/ZZZZ00001-app0");
    CHECK(!update);
    // An installed update: the union of the update over the base.
    test::make_dir(p.user_patch + "/ZZZZ00001");
    CHECK(games::source_tree(p, "ZZZZ00001", &update) == p.pfsmnt + "/ZZZZ00001-app0-patch0-union");
    CHECK(update);

    test::make_dir(p.appmeta + "/ZZZZ00001");
    write_text(p.appmeta + "/ZZZZ00001/param.json",
               R"({"titleId":"ZZZZ00001","contentId":"XX0000-ZZZZ00001_00-TESTGAME00000000","contentVersion":"01.002.000",)"
               R"("localizedParameters":{"defaultLanguage":"en-US","en-US":{"titleName":"Test Game"}}})");
    write_text(p.appmeta + "/ZZZZ00001/icon0.png", "png");
    games::Info i = games::info(p, "ZZZZ00001");
    CHECK(i.title == "Test Game");
    CHECK(i.version == "01.002.000");
    CHECK(i.icon == p.appmeta + "/ZZZZ00001/icon0.png");
    games::Info none = games::info(p, "ZZZZ00002");
    CHECK(none.title_id == "ZZZZ00002");
    CHECK(none.title.empty());
    CHECK(none.icon.empty());
}

TEST_CASE(games_progress_every_fifteen_seconds) {
    games::ProgressSteps s(15);
    const std::uint64_t total = 1000;
    CHECK(!s.update(1, "Extracting", 30, total, 100));         // the stage starts: nothing yet
    CHECK(!s.update(1, "Extracting", 120, total, 110));        // 10 s in: wait
    CHECK(s.update(1, "Extracting", 230, total, 115) == 23);   // 15 s in: where it is now
    CHECK(!s.update(1, "Extracting", 300, total, 120));        // 5 s after: wait
    CHECK(s.update(1, "Extracting", 310, total, 130) == 31);   // 15 s after the last one
    CHECK(!s.update(1, "Extracting", 315, total, 200));        // no further: nothing new to say
    CHECK(!s.update(1, "Extracting", 10, 0, 300));             // no total: nothing
    CHECK(!s.update(1, "Compressing", 60, total, 301));        // a new stage waits its 15 s too
    CHECK(s.update(1, "Compressing", 90, total, 316) == 9);
    CHECK(!s.update(2, "Extracting", 500, total, 301));        // jobs apart
    CHECK(s.update(1, "Compressing", 1000, total, 317) == 100); // the end is never held back
}

TEST_CASE(games_waiting_dump_does_not_block_the_queue) {
    const std::string dir = test::make_temp_dir();
    std::atomic<bool> game_ready{false};
    std::mutex m;
    std::vector<std::string> ran;
    QueueHooks hooks;
    hooks.run = [&](const JobSpec& s, const ProgressFn&) -> Result<JsonValue> {
        std::lock_guard<std::mutex> lock(m);
        ran.push_back(s.game.empty() ? s.input : s.game);
        return JsonValue::object();
    };
    hooks.ready = [&](const JobSpec& s) { return s.game.empty() || game_ready.load(); };
    JobQueue q(dir, hooks);
    REQUIRE_OK(q.start());

    JobSpec dump;
    dump.game = "ZZZZ00001";
    dump.input = "/game";
    auto d = q.add(dump);
    REQUIRE_OK(d);
    CHECK(d->stage == "Waiting for the game to start");
    JobSpec other;
    other.input = "/other";
    auto o = q.add(other);
    REQUIRE_OK(o);

    auto wait_for = [&](auto pred) {
        for (int i = 0; i < 200 && !pred(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return pred();
    };
    // The job behind the waiting dump runs; the dump stays queued.
    CHECK(wait_for([&] { return q.get(o->id)->state == JobState::done; }));
    CHECK(q.get(d->id)->state == JobState::queued);
    CHECK(!q.busy());

    // Its game settles: it starts.
    game_ready = true;
    q.poke();
    CHECK(wait_for([&] { return q.get(d->id)->state == JobState::done; }));
    {
        std::lock_guard<std::mutex> lock(m);
        CHECK(ran == (std::vector<std::string>{"/other", "ZZZZ00001"}));
    }
    q.stop();
}

TEST_CASE(games_dump_spec_checks_the_title_id) {
    auto ok = parse_json(R"({"kind":"convert","game":"ZZZZ00001"})");
    REQUIRE_OK(ok);
    auto s = JobSpec::from_json(*ok);
    REQUIRE_OK(s);
    CHECK(s->game == "ZZZZ00001");
    CHECK(s->to_json()["game"].str() == "ZZZZ00001");
    auto next = parse_json(R"({"kind":"convert","game":"next"})");
    REQUIRE_OK(next);
    CHECK(JobSpec::from_json(*next).ok());
    for (const char* bad : {R"({"kind":"convert","game":"../x"})", R"({"kind":"convert","game":"zzzz00001"})",
                            R"({"kind":"copy","game":"ZZZZ00001"})"}) {
        auto j = parse_json(bad);
        REQUIRE_OK(j);
        CHECK(!JobSpec::from_json(*j).ok());
    }
}

TEST_CASE(games_closed_game_sends_the_dump_back_to_waiting) {
    const std::string dir = test::make_temp_dir();
    std::atomic<bool> running{true};
    std::atomic<int> runs{0}, closed{0};
    QueueHooks hooks;
    hooks.run = [&](const JobSpec&, const ProgressFn&) -> Result<JsonValue> {
        if (runs++ == 0) {
            running = false; // the game closes mid-dump: its tree is gone
            return make_error(Errc::io, "cannot read the game's files");
        }
        return JsonValue::object();
    };
    hooks.ready = [&](const JobSpec& s) { return s.game.empty() || running.load(); };
    hooks.game_running = [&](const JobSpec&) { return running.load(); };
    hooks.on_game_closed = [&](const Job&) { ++closed; };
    JobQueue q(dir, hooks);
    REQUIRE_OK(q.start());
    JobSpec dump;
    dump.game = "ZZZZ00001";
    dump.input = "/game";
    auto d = q.add(dump);
    REQUIRE_OK(d);
    auto wait_for = [&](auto pred) {
        for (int i = 0; i < 200 && !pred(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return pred();
    };
    // Not failed: queued again, saying why, until the game runs again.
    CHECK(wait_for([&] { return closed.load() == 1; }));
    auto j = q.get(d->id);
    CHECK(j->state == JobState::queued);
    CHECK(j->stage == "Game closed: starts over at its next launch"); // no checkpoint here
    CHECK(j->error.empty());
    running = true;
    q.poke();
    CHECK(wait_for([&] { return q.get(d->id)->state == JobState::done; }));
    CHECK_EQ(runs.load(), 2);
    q.stop();
}

TEST_CASE(games_next_game_dump_takes_the_game_that_started) {
    const std::string dir = test::make_temp_dir();
    std::atomic<bool> started{false};
    std::string ran_game;
    std::mutex m;
    QueueHooks hooks;
    hooks.run = [&](const JobSpec& s, const ProgressFn&) -> Result<JsonValue> {
        std::lock_guard<std::mutex> lock(m);
        ran_game = s.game;
        return JsonValue::object();
    };
    hooks.ready = [&](const JobSpec& s) { return s.game.empty() || started.load(); };
    hooks.prepare = [&](JobSpec& s) -> Status {
        if (s.game == "next") {
            s.game = "ZZZZ00009";
            s.input = "/game9";
        }
        return {};
    };
    JobQueue q(dir, hooks);
    REQUIRE_OK(q.start());
    JobSpec next;
    next.game = "next";
    auto d = q.add(next);
    REQUIRE_OK(d);
    CHECK(d->stage == "Waiting for a game to start");
    started = true;
    q.poke();
    auto wait_for = [&](auto pred) {
        for (int i = 0; i < 200 && !pred(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return pred();
    };
    CHECK(wait_for([&] { return q.get(d->id)->state == JobState::done; }));
    CHECK(q.get(d->id)->spec.game == "ZZZZ00009"); // kept with the job
    {
        std::lock_guard<std::mutex> lock(m);
        CHECK(ran_game == "ZZZZ00009");
    }
    q.stop();
}
