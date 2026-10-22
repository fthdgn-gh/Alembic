// SPDX-License-Identifier: GPL-3.0-or-later
#include "jobs.hpp"

#include "alembic/io.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>

namespace alembic::engine {

namespace {

constexpr std::size_t kKeepFinished = 200;

bool finished(JobState s) { return s != JobState::queued && s != JobState::running && s != JobState::paused; }
// Jobs still to run, in their order.
bool waiting(JobState s) { return s == JobState::queued || s == JobState::paused; }
constexpr std::uint32_t kMaxAutoResumes = 3; // a job that keeps stopping the engine is left alone

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
    auto names = list_directory(dir);
    if (!names.ok()) return;
    for (const std::string& name : *names) {
        if (name.size() != prefix.size() + 16 + 4 || name.compare(0, prefix.size(), prefix) != 0 ||
            name.compare(name.size() - 4, 4, ".tmp") != 0)
            continue;
        std::string path = dir + "/" + name;
        if (remove_file(path).ok() && log) log("removed " + path + " left by an interrupted job");
    }
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
    case JobState::paused: return "paused";
    }
    return "failed";
}

JsonValue JobSpec::to_json() const {
    JsonValue j = JsonValue::object();
    j.set("kind", kind);
    j.set("input", input);
    j.set("output", output);
    if (!output_dir.empty()) j.set("output_dir", output_dir);
    if (!output_name.empty()) j.set("output_name", output_name);
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
    j.set("ampr_emu", ampr_emu);
    j.set("delete_source", delete_source);
    j.set("verify", verify);
    if (hide_source) j.set("hide_source", true);
    if (!origin.empty()) j.set("origin", origin);
    return j;
}

Result<JobSpec> JobSpec::from_json(const JsonValue& j) {
    if (!j.is_object()) return make_error(Errc::invalid_argument, "a job must be a JSON object");
    JobSpec s;
    s.kind = j["kind"].str("convert");
    if (s.kind != "convert" && s.kind != "copy" && s.kind != "move" && s.kind != "install" && s.kind != "ampr_emu")
        return make_error(Errc::invalid_argument, "kind must be convert, copy, move, install or ampr_emu");
    s.input = j["input"].str();
    s.output = j["output"].str();
    s.output_dir = j["output_dir"].str();
    s.output_name = j["output_name"].str();
    if (s.output_name.find('/') != std::string::npos || s.output_name == "." || s.output_name == "..")
        return make_error(Errc::invalid_argument, "output_name must be a plain file name");
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
    s.ampr_emu = j["ampr_emu"].str("auto");
    if (s.ampr_emu != "auto" && s.ampr_emu != "always" && s.ampr_emu != "off")
        return make_error(Errc::invalid_argument, "ampr_emu must be auto, always or off");
    s.delete_source = j["delete_source"].boolean();
    s.verify = j["verify"].boolean(true);
    s.hide_source = j["hide_source"].boolean();
    s.origin = j["origin"].str();
    if ((s.kind == "copy" || s.kind == "move") && s.output.empty())
        return make_error(Errc::invalid_argument, "copy and move need an output");
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
    if (ran_before) j.set("ran_before", ran_before);
    j.set("error", error);
    j.set("result", result);
    if (auto_resumes) j.set("auto_resumes", static_cast<std::uint64_t>(auto_resumes));
    return j;
}

Result<Job> Job::from_json(const JsonValue& j) {
    Job job;
    job.id = static_cast<std::uint64_t>(j["id"].integer(0));
    ALEMBIC_ASSIGN(job.spec, JobSpec::from_json(j["spec"]));
    std::string state = j["state"].str();
    for (JobState s : {JobState::queued, JobState::running, JobState::done, JobState::failed, JobState::cancelled,
                       JobState::interrupted, JobState::paused})
        if (state == state_name(s)) job.state = s;
    job.stage = j["stage"].str();
    job.done = static_cast<std::uint64_t>(j["done"].integer(0));
    job.total = static_cast<std::uint64_t>(j["total"].integer(0));
    job.created = j["created"].integer(0);
    job.started = j["started"].integer(0);
    job.finished = j["finished"].integer(0);
    job.ran_before = j["ran_before"].integer(0);
    job.error = j["error"].str();
    job.result = j["result"];
    job.auto_resumes = static_cast<std::uint32_t>(j["auto_resumes"].integer(0));
    if (job.id == 0) return make_error(Errc::invalid_data, "a saved job has no id");
    return job;
}

namespace {
// A run that stopped and will continue: its time joins ran_before (or, for
// a job that starts over, the time is dropped).
void set_aside(Job& job, bool continues = true) {
    job.ran_before = continues ? job.ran_before + std::max<std::int64_t>(0, job.finished - job.started) : 0;
    job.finished = 0;
}
} // namespace

std::int64_t Job::elapsed(std::int64_t now) const {
    if (state == JobState::running) return ran_before + std::max<std::int64_t>(0, now - started);
    if (waiting(state)) return ran_before ? ran_before : -1; // the latest run is in ran_before
    if (!started) return -1;
    return ran_before + std::max<std::int64_t>(0, (finished ? finished : now) - started);
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
                    remove_stale_temp_files(job->spec.output, hooks_.log);
                    if (hooks_.has_checkpoint && hooks_.has_checkpoint(job->spec) && job->auto_resumes < kMaxAutoResumes) {
                        // Continue it where its last checkpoint left off.
                        job->state = JobState::queued;
                        job->stage = "Continues from its checkpoint";
                        ++job->auto_resumes;
                        if (hooks_.log) hooks_.log("job " + std::to_string(job->id) + " continues from its checkpoint");
                    } else {
                        job->state = JobState::interrupted;
                        job->error = "the engine stopped while this job ran";
                        job->finished = unix_now();
                    }
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

void JobQueue::discard(const JobSpec& spec) {
    if (!hooks_.discard_checkpoint) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const Job& j : jobs_)
            if ((waiting(j.state) || j.state == JobState::running) && j.spec.output == spec.output) return;
    }
    hooks_.discard_checkpoint(spec);
}

Status JobQueue::cancel(std::uint64_t id) {
    JobSpec dropped;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        Job* job = find_locked(id);
        if (!job) return make_error(Errc::invalid_argument, "no such job");
        if (job->state == JobState::running) {
            pause_running_ = false;
            cancel_running_ = true;
            return {};
        }
        if (!waiting(job->state)) return make_error(Errc::invalid_argument, "the job has already finished");
        job->state = JobState::cancelled;
        job->error = "cancelled";
        job->finished = unix_now();
        save_locked();
        if (hooks_.log) hooks_.log("job " + std::to_string(id) + " cancelled before it finished");
        dropped = job->spec;
    }
    discard(dropped);
    return {};
}

Status JobQueue::pause(std::uint64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    Job* job = find_locked(id);
    if (!job) return make_error(Errc::invalid_argument, "no such job");
    if (job->state == JobState::running) {
        pause_running_ = true;
        cancel_running_ = true;
        job->stage = "Pausing";
        return {};
    }
    if (job->state != JobState::queued) return make_error(Errc::invalid_argument, "only queued or running jobs can be paused");
    job->state = JobState::paused;
    job->stage = "Paused";
    save_locked();
    return {};
}

Status JobQueue::resume(std::uint64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    Job* job = find_locked(id);
    if (!job) return make_error(Errc::invalid_argument, "no such job");
    if (job->state != JobState::paused) return make_error(Errc::invalid_argument, "the job is not paused");
    job->state = JobState::queued;
    job->stage = "";
    save_locked();
    wake_.notify_all();
    return {};
}

Status JobQueue::move(std::uint64_t id, int direction) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find_if(jobs_.begin(), jobs_.end(), [&](const Job& j) { return j.id == id; });
    if (it == jobs_.end()) return make_error(Errc::invalid_argument, "no such job");
    if (!waiting(it->state)) return make_error(Errc::invalid_argument, "only queued or paused jobs can be moved");
    std::ptrdiff_t i = it - jobs_.begin(), step = direction < 0 ? -1 : 1;
    for (std::ptrdiff_t k = i + step; k >= 0 && k < static_cast<std::ptrdiff_t>(jobs_.size()); k += step) {
        if (!waiting(jobs_[static_cast<std::size_t>(k)].state)) continue;
        // Keep the jobs in between (running or finished) where they are.
        std::swap(jobs_[static_cast<std::size_t>(i)], jobs_[static_cast<std::size_t>(k)]);
        save_locked();
        return {};
    }
    return make_error(Errc::invalid_argument, direction < 0 ? "it is already first" : "it is already last");
}

Status JobQueue::remove(std::uint64_t id) {
    JobSpec dropped;
    bool drop = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = std::find_if(jobs_.begin(), jobs_.end(), [&](const Job& j) { return j.id == id; });
        if (it == jobs_.end()) return make_error(Errc::invalid_argument, "no such job");
        if (!finished(it->state)) return make_error(Errc::invalid_argument, "cancel the job first");
        drop = it->state != JobState::done;
        dropped = it->spec;
        jobs_.erase(it);
        save_locked();
    }
    if (drop) discard(dropped); // a failed job's partial output
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
    std::vector<JobSpec> dropped;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const Job& j : jobs_)
            if (finished(j.state) && j.state != JobState::done) dropped.push_back(j.spec);
        jobs_.erase(std::remove_if(jobs_.begin(), jobs_.end(), [](const Job& j) { return finished(j.state); }), jobs_.end());
        save_locked();
    }
    for (const JobSpec& s : dropped) discard(s);
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
            pause_running_ = false;
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

        // Whether the job left a checkpoint (asked before taking the lock).
        bool stopped = !result.ok() && (result.error().code == Errc::cancelled || cancel_running_);
        bool kept = stopped && hooks_.has_checkpoint && hooks_.has_checkpoint(spec);
        Job copy;
        bool drop = false;
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
            } else if (stopped && stopping_) {
                // The engine is stopping: a job with a checkpoint continues at the next start.
                job->state = kept ? JobState::queued : JobState::interrupted;
                job->error = kept ? "" : "the engine was stopped";
                job->stage = kept ? "Continues when the engine starts" : job->stage;
                if (kept) set_aside(*job);
            } else if (stopped && pause_running_) {
                job->state = JobState::paused;
                job->stage = kept ? "Paused" : "Paused; starts over when resumed";
                set_aside(*job, kept);
            } else if (stopped) {
                job->state = JobState::cancelled;
                job->error = "cancelled";
                drop = true;
            } else {
                job->state = JobState::failed;
                job->error = result.error().message;
            }
            pause_running_ = false;
            save_locked();
            copy = *job;
        }
        if (drop) discard(copy.spec);
        if (copy.state == JobState::paused || copy.state == JobState::queued) {
            if (hooks_.log) hooks_.log("job " + std::to_string(id) + " " + state_name(copy.state));
            continue;
        }
        if (hooks_.log)
            hooks_.log("job " + std::to_string(id) + " " + state_name(copy.state) +
                       (copy.error.empty() ? "" : ": " + copy.error));
        if (hooks_.on_finished) hooks_.on_finished(copy);
    }
}

} // namespace alembic::engine
