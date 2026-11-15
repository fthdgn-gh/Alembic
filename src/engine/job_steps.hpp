// SPDX-License-Identifier: GPL-3.0-or-later
// The steps a job takes, for the Queue: worked out from its spec, each stage
// it reports mapped onto one, and one overall figure where every step is an
// equal share (5 steps: 20% each; owner). Page, app and toasts show the same.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace alembic::engine {

struct JobSpec;

// The steps a job is expected to take, as catalogue ids, in order.
std::vector<std::string> plan_steps(const JobSpec& spec);

// A step's name for people (English; the page and app translate it).
std::string step_label(const std::string& id, const JobSpec& spec);

// The step a reported stage belongs to; nothing for brief or unknown stages
// (they leave the current step as it is).
std::optional<std::string> step_for_stage(const std::string& stage);

// Puts `id` into `plan` in catalogue order when it is not there (a stage the
// spec did not predict, such as checking a package input). Its index.
std::size_t place_step(std::vector<std::string>& plan, const std::string& id);

// The overall progress in thousandths: step `index` of `count`, `done` of
// `total` through it (0 when the stage has no total).
std::uint32_t overall_permille(std::size_t index, std::size_t count, std::uint64_t done, std::uint64_t total);

} // namespace alembic::engine
