// SPDX-License-Identifier: GPL-3.0-or-later
#include "job_steps.hpp"

#include "jobs.hpp"
#include "runner.hpp"

#include <algorithm>
#include <map>

namespace alembic::engine {

namespace {

// Every step, in the order a job takes them.
const std::vector<std::string>& catalogue() {
    static const std::vector<std::string> ids = {"wait",  "check",    "decrypt", "pictures", "pack",   "write", "compress",
                                                 "packs", "compare",  "install", "delete",   "hide"};
    return ids;
}

std::size_t order_of(const std::string& id) {
    const auto& c = catalogue();
    return static_cast<std::size_t>(std::find(c.begin(), c.end(), id) - c.begin());
}

} // namespace

std::vector<std::string> plan_steps(const JobSpec& spec) {
    std::vector<std::string> plan;
    if (spec.kind == "copy" || spec.kind == "move" || spec.kind == "ampr_emu") return {"write"};
    if (spec.kind == "install") return {"install"};
    if (spec.kind == "compare") return {"compare"};
    if (!spec.game.empty()) plan.push_back("wait");
    if (spec.decrypt_self) plan.push_back("decrypt");
    if (spec.bc1_pictures) plan.push_back("pictures");
    if (spec.lz4 || spec.zstd) plan.push_back("pack");
    plan.push_back("write");
    if (spec.delete_source && spec.verify) plan.push_back("compare");
    if (spec.install_after) plan.push_back("install");
    if (spec.delete_source || (spec.install_after && !spec.keep_package)) plan.push_back("delete");
    if (spec.hide_source) plan.push_back("hide");
    return plan;
}

std::string step_label(const std::string& id, const JobSpec& spec) {
    if (id == "wait") return "Wait for the game";
    if (id == "check") return "Check the package";
    if (id == "decrypt") return "Decrypt executables";
    if (id == "pictures") return "Re-encode pictures";
    if (id == "pack") return "Pack assets";
    if (id == "compress") return "Compress";
    if (id == "packs") return "Check the packs";
    if (id == "install") return "Install";
    if (id == "hide") return "Hide the source";
    if (id == "compare") return spec.kind == "compare" ? "Compare" : "Compare with the source";
    if (id == "delete") return spec.install_after && !spec.delete_source ? "Delete the package" : "Delete the source";
    // write
    if (spec.kind == "copy") return "Copy";
    if (spec.kind == "move") return "Move";
    if (spec.kind == "ampr_emu") return "Add the AMPR emulator";
    auto target = resolve_target(spec);
    if (target.ok() && *target == "folder") return "Write the folder";
    if (target.ok() && *target == "fpkg") return "Build the package";
    return "Write the image";
}

std::optional<std::string> step_for_stage(const std::string& stage) {
    static const std::map<std::string, std::string> stages = {
        {"Waiting for the game to start", "wait"},
        {"Waiting for a game to start", "wait"},
        {"Game closed: continues at its next launch", "wait"},
        {"Game closed: starts over at its next launch", "wait"},
        {"Checking the package", "check"},
        {"Checking the package (on the other engine)", "check"},
        {"Decrypting executables", "decrypt"},
        {"Re-encoding pictures", "pictures"},
        {"LZ4 pass 1", "pack"},
        {"zstd pass 1", "pack"},
        {"Extracting", "write"},
        {"Building exFAT", "write"},
        {"Writing UFS2 image", "write"},
        {"Writing PFS image", "write"},
        {"Building the inner image", "write"},
        {"Writing the package", "write"},
        {"Hashing", "write"},
        {"Copying image", "write"},
        {"Copying", "write"},
        {"Packing dump", "write"},
        {"Uploading", "write"},
        {"Decoding PFSC", "write"},
        {"Writing the emulator", "write"},
        {"Compressing PFSC", "compress"},
        {"Compressing", "compress"},
        {"Verifying LZ4 packs", "packs"},
        {"Verifying zstd packs", "packs"},
        {"Comparing", "compare"},
        {"Handing it to the system installer", "install"},
        {"Installing (the console reads the package)", "install"},
        {"Hashing the package", "install"},
        {"Deleting the source", "delete"},
    };
    auto it = stages.find(stage);
    if (it == stages.end()) return std::nullopt;
    return it->second;
}

std::size_t place_step(std::vector<std::string>& plan, const std::string& id) {
    auto it = std::find(plan.begin(), plan.end(), id);
    if (it != plan.end()) return static_cast<std::size_t>(it - plan.begin());
    const std::size_t order = order_of(id);
    auto at = std::find_if(plan.begin(), plan.end(), [&](const std::string& s) { return order_of(s) > order; });
    return static_cast<std::size_t>(plan.insert(at, id) - plan.begin());
}

std::uint32_t overall_permille(std::size_t index, std::size_t count, std::uint64_t done, std::uint64_t total) {
    if (count == 0) return 0;
    if (index >= count) return 1000;
    const double within = total ? std::min(1.0, double(done) / double(total)) : 0.0;
    return static_cast<std::uint32_t>((double(index) + within) * 1000.0 / double(count));
}

} // namespace alembic::engine
