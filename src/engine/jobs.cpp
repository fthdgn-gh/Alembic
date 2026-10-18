// SPDX-License-Identifier: GPL-3.0-or-later
#include "jobs.hpp"

#include "alembic/io.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <dirent.h>
#include <unistd.h>

namespace alembic::engine {

namespace {

constexpr std::size_t kKeepFinished = 200;

bool finished(JobState s) { return s != JobState::queued && s != JobState::running; }

Result<std::string> read_text(const std::string& path) {
    ALEMBIC_ASSIGN(File file, File::open(path, File::Mode::read));
    ALEMBIC_ASSIGN(std::uint64_t size, file.size());
    if (size > 64u * 1024 * 1024) return make_error(Errc::too_large, path + " is too large");
    std::string text(static_cast<std::size_t>(size), '\0');
    ALEMBIC_TRY(file.read_at(0, MutableBytes(reinterpret_cast<std::uint8_t*>(text.data()), text.size())));
    return text;
}

Status write_text(const std::string& path, const std::string& text) {
    ALEMBIC_ASSIGN(AtomicOutput out, AtomicOutput::create(path, true));
    ALEMBIC_TRY(out.file().write_at(0, Bytes(reinterpret_cast<const std::uint8_t*>(text.data()), text.size())));
    return out.commit();
}

// AtomicOutput writes "<output>.<16 hex>.tmp" next to the output; a crash
// leaves it behind.
void remove_stale_temp_files(const std::string& output, const std::function<void(const std::string&)>& log) {
    if (output.empty() || output.back() == '/') return;
    std::size_t slash = output.rfind('/');
    std::string dir = slash == std::string::npos ? "." : output.substr(0, slash);
    std::string prefix = output.substr(slash == std::string::npos ? 0 : slash + 1) + ".";
    DIR* d = ::opendir(dir.c_str());
    if (!d) return;
    while (dirent* e = ::readdir(d)) {
        std::string name = e->d_name;
        if (name.size() != prefix.size() + 16 + 4 || name.compare(0, prefix.size(), prefix) != 0 ||
            name.compare(name.size() - 4, 4, ".tmp") != 0)
            continue;
        std::string path = dir + "/" + name;
        if (::unlink(path.c_str()) == 0 && log) log("removed " + path + " left by an interrupted job");
    }
    ::closedir(d);
}

} // namespace

std::int64_t unix_now() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

const char* state_name(JobState s) {
    switch (s) {
    case JobState::queued: return "queued";
    case JobState::running: return "running";
    case JobState::done: return "done";
    case JobState::failed: return "failed";
    case JobState::cancelled: return "cancelled";
    case JobState::interrupted: return "interrupted";
    }
    return "failed";
}

JsonValue JobSpec::to_json() const {
    JsonValue j = JsonValue::object();
    j.set("kind", kind);
    j.set("input", input);
    j.set("output", output);
    j.set("to", to);
    j.set("level", level);
    j.set("overwrite", overwrite);
    j.set("rebuild", rebuild);
    if (!passcode.empty()) j.set("passcode", passcode);
    j.set("cluster_size", cluster_size);
    j.set("lz4", lz4);
    j.set("lz4_profile", lz4_profile);
    j.set("lz4_level", lz4_level);
    j.set("lz4_runtime", lz4_runtime);
    j.set("lz4_unpack", lz4_unpack);
    j.set("delete_source", delete_source);
    j.set("verify", verify);
    return j;
}

Result<JobSpec> JobSpec::from_json(const JsonValue& j) {
    if (!j.is_object()) return make_error(Errc::invalid_argument, "a job must be a JSON object");
    JobSpec s;
    s.kind = j["kind"].str("convert");
    if (s.kind != "convert" && s.kind != "copy" && s.kind != "move")
        return make_error(Errc::invalid_argument, "kind must be convert, copy or move");
    s.input = j["input"].str();
    s.output = j["output"].str();
    s.to = j["to"].str();
    s.level = static_cast<int>(j["level"].integer(6));
    s.overwrite = j["overwrite"].boolean();
    s.rebuild = j["rebuild"].boolean();
    s.passcode = j["passcode"].str();
    std::int64_t cluster = j["cluster_size"].integer(0);
    s.lz4 = j["lz4"].boolean();
    s.lz4_profile = j["lz4_profile"].str("auto");
    s.lz4_level = static_cast<int>(j["lz4_level"].integer(9));
    s.lz4_runtime = j["lz4_runtime"].str();
    s.lz4_unpack = j["lz4_unpack"].boolean();
    s.delete_source = j["delete_source"].boolean();
    s.verify = j["verify"].boolean(true);
    if (s.kind != "convert" && s.output.empty()) return make_error(Errc::invalid_argument, "copy and move need an output");
    if (s.input.empty()) return make_error(Errc::invalid_argument, "the job has no input");
    if (s.level < 0 || s.level > 9) return make_error(Errc::invalid_argument, "level must be 0-9");
    if (s.lz4_level < 1 || s.lz4_level > 12) return make_error(Errc::invalid_argument, "lz4_level must be 1-12");
    if (cluster < 0 || cluster > 0x2000000) return make_error(Errc::invalid_argument, "bad cluster_size");
    s.cluster_size = static_cast<std::uint32_t>(cluster);
    if (s.lz4 && s.lz4_unpack) return make_error(Errc::invalid_argument, "lz4 and lz4_unpack exclude each other");
    return s;
}

JsonValue Job::to_json() const {
    JsonValue j = JsonValue::object();
    j.set("id", id);
    j.set("spec", spec.to_json());
    j.set("state", state_name(state));
    j.set("stage", stage);
    j.set("done", done);
    j.set("total", total);
    j.set("created", created);
    j.set("started", started);
    j.set("finished", finished);
    j.set("error", error);
    j.set("result", result);
    return j;
}

Result<Job> Job::from_json(const JsonValue& j) {
    Job job;
    job.id = static_cast<std::uint64_t>(j["id"].integer(0));
    ALEMBIC_ASSIGN(job.spec, JobSpec::from_json(j["spec"]));
    std::string state = j["state"].str();
    for (JobState s : {JobState::queued, JobState::running, JobState::done, JobState::failed, JobState::cancelled,
                       JobState::interrupted})
        if (state == state_name(s)) job.state = s;
    job.stage = j["stage"].str();
    job.done = static_cast<std::uint64_t>(j["done"].integer(0));
    job.total = static_cast<std::uint64_t>(j["total"].integer(0));
    job.created = j["created"].integer(0);
    job.started = j["started"].integer(0);
    job.finished = j["finished"].integer(0);
    job.error = j["error"].str();
    job.result = j["result"];
    if (job.id == 0) return make_error(Errc::invalid_data, "a saved job has no id");
    return job;
}

JobQueue::JobQueue(std::string state_dir, QueueHooks hooks)
    : state_dir_(std::move(state_dir)), hooks_(std::move(hooks)) {}

JobQueue::~JobQueue() { stop(); }

Status JobQueue::start() {
    std::string path = state_dir_ + "/jobs.json";
    if (path_exists(path)) {
        auto text = read_text(path);
        auto json = text.ok() ? parse_json(*text) : Result<JsonValue>(text.error());
        if (!json.ok()) {
            if (hooks_.log) hooks_.log("jobs.json is unreadable, starting empty: " + json.error().message);
        } else {
            next_id_ = static_cast<std::uint64_t>((*json)["next_id"].integer(1));
            for (const JsonValue& item : (*json)["jobs"].items()) {
                auto job = Job::from_json(item);
                if (!job.ok()) continue;
                if (job->state == JobState::running) {
                    job->state = JobState::interrupted;
                    job->error = "the engine stopped while this job ran";
                    job->finished = unix_now();
                    remove_stale_temp_files(job->spec.output, hooks_.log);
                }
                next_id_ = std::max(next_id_, job->id + 1);
                jobs_.push_back(std::move(*job));
            }
        }
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = false;
        save_locked();
    }
    thread_ = std::thread([this] { worker(); });
    return {};
}

void JobQueue::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!thread_.joinable()) return;
        stopping_ = true;
        cancel_running_ = true;
    }
    wake_.notify_all();
    thread_.join();
}

void JobQueue::save_locked() const {
    JsonValue root = JsonValue::object();
    root.set("next_id", next_id_);
    JsonValue list = JsonValue::array();
    for (const Job& job : jobs_) list.push(job.to_json());
    root.set("jobs", std::move(list));
    Status s = make_parent_directories(state_dir_ + "/jobs.json");
    if (s.ok()) s = write_text(state_dir_ + "/jobs.json", root.dump());
    if (!s.ok() && hooks_.log) hooks_.log("cannot save jobs.json: " + s.error().message);
}

Job* JobQueue::find_locked(std::uint64_t id) {
    for (Job& job : jobs_)
        if (job.id == id) return &job;
    return nullptr;
}

Result<Job> JobQueue::add(JobSpec spec) {
    std::lock_guard<std::mutex> lock(mutex_);
    Job job;
    job.id = next_id_++;
    job.spec = std::move(spec);
    job.created = unix_now();
    jobs_.push_back(job);
    // Keep the newest finished jobs only.
    std::size_t finished_count = std::count_if(jobs_.begin(), jobs_.end(), [](const Job& j) { return finished(j.state); });
    for (auto it = jobs_.begin(); finished_count > kKeepFinished && it != jobs_.end();) {
        if (finished(it->state)) {
            it = jobs_.erase(it);
            --finished_count;
        } else {
            ++it;
        }
    }
    save_locked();
    wake_.notify_all();
    return job;
}

Status JobQueue::cancel(std::uint64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    Job* job = find_locked(id);
    if (!job) return make_error(Errc::invalid_argument, "no such job");
    if (job->state == JobState::queued) {
        job->state = JobState::cancelled;
        job->error = "cancelled";
        job->finished = unix_now();
        save_locked();
        if (hooks_.log) hooks_.log("job " + std::to_string(id) + " cancelled before it started");
        return {};
    }
    if (job->state == JobState::running) {
        cancel_running_ = true;
        return {};
    }
    return make_error(Errc::invalid_argument, "the job has already finished");
}

Status JobQueue::remove(std::uint64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find_if(jobs_.begin(), jobs_.end(), [&](const Job& j) { return j.id == id; });
    if (it == jobs_.end()) return make_error(Errc::invalid_argument, "no such job");
    if (!finished(it->state)) return make_error(Errc::invalid_argument, "cancel the job first");
    jobs_.erase(it);
    save_locked();
    return {};
}

Result<Job> JobQueue::retry(std::uint64_t id) {
    JobSpec spec;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        Job* job = find_locked(id);
        if (!job) return make_error(Errc::invalid_argument, "no such job");
        if (job->state != JobState::failed && job->state != JobState::cancelled && job->state != JobState::interrupted)
            return make_error(Errc::invalid_argument, "only failed, cancelled or interrupted jobs can be retried");
        spec = job->spec;
    }
    return add(std::move(spec));
}

void JobQueue::clear_finished() {
    std::lock_guard<std::mutex> lock(mutex_);
    jobs_.erase(std::remove_if(jobs_.begin(), jobs_.end(), [](const Job& j) { return finished(j.state); }), jobs_.end());
    save_locked();
}

std::vector<Job> JobQueue::list() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return jobs_;
}

std::optional<Job> JobQueue::get(std::uint64_t id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const Job& job : jobs_)
        if (job.id == id) return job;
    return std::nullopt;
}

bool JobQueue::busy() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return running_id_ != 0;
}

void JobQueue::worker() {
    for (;;) {
        JobSpec spec;
        std::uint64_t id = 0;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [&] {
                return stopping_ || std::any_of(jobs_.begin(), jobs_.end(), [](const Job& j) { return j.state == JobState::queued; });
            });
            if (stopping_) return;
            Job* job = nullptr;
            for (Job& j : jobs_)
                if (j.state == JobState::queued) {
                    job = &j;
                    break;
                }
            job->state = JobState::running;
            job->started = unix_now();
            job->stage = "Starting";
            job->done = job->total = 0;
            id = running_id_ = job->id;
            spec = job->spec;
            cancel_running_ = false;
            save_locked();
        }
        if (hooks_.log) hooks_.log("job " + std::to_string(id) + " started: " + spec.input);

        ProgressFn progress = [&](const ProgressEvent& e) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (Job* job = find_locked(id)) {
                    job->stage = std::string(e.stage);
                    job->done = e.done;
                    job->total = e.total;
                }
            }
            if (hooks_.while_running) hooks_.while_running();
            return !cancel_running_.load();
        };
        Result<JsonValue> result = hooks_.run(spec, progress);

        Job copy;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            running_id_ = 0;
            Job* job = find_locked(id);
            if (!job) continue;
            job->finished = unix_now();
            if (result.ok()) {
                job->state = JobState::done;
                job->result = std::move(*result);
                job->stage = "Done";
            } else if (result.error().code == Errc::cancelled || cancel_running_) {
                job->state = stopping_ ? JobState::interrupted : JobState::cancelled;
                job->error = stopping_ ? "the engine was stopped" : "cancelled";
            } else {
                job->state = JobState::failed;
                job->error = result.error().message;
            }
            save_locked();
            copy = *job;
        }
        if (hooks_.log)
            hooks_.log("job " + std::to_string(id) + " " + state_name(copy.state) +
                       (copy.error.empty() ? "" : ": " + copy.error));
        if (hooks_.on_finished) hooks_.on_finished(copy);
    }
}

} // namespace alembic::engine
