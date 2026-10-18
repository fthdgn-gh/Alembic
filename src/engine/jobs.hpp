// SPDX-License-Identifier: GPL-3.0-or-later
// The engine's job queue: conversions run one at a time on a worker thread,
// in the order they were added. The queue is saved to <state>/jobs.json after
// every change, so it survives an engine restart (a job that was running is
// marked interrupted and can be retried).
#pragma once

#include "json_value.hpp"

#include "alembic/io.hpp"
#include "alembic/result.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace alembic::engine {

// What to do. "convert" mirrors the CLI's `convert` options; "copy" and
// "move" take `input` to `output` (the new path) for the file manager.
struct JobSpec {
    std::string kind = "convert";
    std::string input;
    std::string output;   // empty: <output_dir>/<output_name>
    std::string output_dir;  // empty: the input's folder
    std::string output_name; // empty: the input's name with the target's extension
    std::string to;       // folder, exfat, ffpfsc; empty: from the output name
    int level = 6;        // zlib level for FFPFSC
    bool overwrite = false;
    bool rebuild = false; // exFAT input → rebuilt exFAT
    std::string passcode; // FPKG; empty = 32 zeros
    std::uint32_t cluster_size = 0; // 0 = default (64 KiB)
    bool lz4 = false;
    std::string lz4_profile = "auto";
    int lz4_level = 9;
    std::string lz4_runtime; // empty: the engine's runtime file
    bool lz4_unpack = false;
    // After a successful conversion: compare the output with the input file
    // by file, then delete the input (only if they match).
    bool delete_source = false;
    bool verify = true; // the comparison before delete_source

    JsonValue to_json() const;
    static Result<JobSpec> from_json(const JsonValue& json);
};

enum class JobState { queued, running, done, failed, cancelled, interrupted };
const char* state_name(JobState state);

struct Job {
    std::uint64_t id = 0;
    JobSpec spec;
    JobState state = JobState::queued;
    std::string stage;
    std::uint64_t done = 0;
    std::uint64_t total = 0;
    std::int64_t created = 0; // Unix seconds
    std::int64_t started = 0;
    std::int64_t finished = 0;
    std::string error;
    JsonValue result; // the conversion report

    JsonValue to_json() const;
    static Result<Job> from_json(const JsonValue& json);
};

// Runs one job. `progress` returns false to cancel.
using JobRunner = std::function<Result<JsonValue>(const JobSpec&, const ProgressFn& progress)>;

struct QueueHooks {
    JobRunner run;
    std::function<void(const Job&)> on_finished; // notification, log
    std::function<void()> while_running;         // keep the console awake
    std::function<void(const std::string&)> log;
};

class JobQueue {
public:
    JobQueue(std::string state_dir, QueueHooks hooks);
    ~JobQueue();

    // Reads jobs.json (missing is fine) and starts the worker.
    Status start();
    void stop(); // cancels the running job and joins the worker

    Result<Job> add(JobSpec spec);
    // Queued: removed from the queue; running: asked to stop.
    Status cancel(std::uint64_t id);
    // Finished jobs only.
    Status remove(std::uint64_t id);
    // A failed, cancelled or interrupted job queued again (new id).
    Result<Job> retry(std::uint64_t id);
    // Removes every finished job.
    void clear_finished();

    std::vector<Job> list() const;
    std::optional<Job> get(std::uint64_t id) const;
    bool busy() const;

private:
    void worker();
    void save_locked() const;
    Job* find_locked(std::uint64_t id);

    std::string state_dir_;
    QueueHooks hooks_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::vector<Job> jobs_; // oldest first
    std::uint64_t next_id_ = 1;
    std::uint64_t running_id_ = 0;
    std::atomic<bool> cancel_running_{false};
    bool stopping_ = false;
    std::thread thread_;
};

std::int64_t unix_now();

} // namespace alembic::engine
