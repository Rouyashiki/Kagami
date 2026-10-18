#pragma once

#include <cstdint>
#include <iosfwd>
#include <map>
#include <string>
#include <vector>
#include "core/directory.hpp"

namespace kagami::mount::module_runtime {

struct Assignment {
    std::string id;
    std::string backend;  // overlay, magic, kasumi, none
};

// Serializes cooperating writers across processes. Nested calls, including in
// a forked mount child, reuse the ancestor lock. Observation failures never
// change mount behavior. An unfinished operation remains visibly unknown.
class Operation {
public:
    Operation();
    ~Operation();
    Operation(const Operation&) = delete;
    Operation& operator=(const Operation&) = delete;
    Operation(Operation&&) = delete;
    Operation& operator=(Operation&&) = delete;
    void finish(bool success = true);

private:
    bool outer_ = false;
    bool finished_ = false;
};

// Reassign only `scope` when nonempty. Previously assigned modules in that
// scope remain listed with an empty, complete contribution set (deselected).
// Call within an Operation; do not call on teardown, whose old evidence is
// deliberately retained and compared with the live kernel after removal.
// An empty scope is an explicit fresh boot inventory and may replace an old
// schema or corrupt record. Only the successfully claimed boot path uses it.
bool assign_modules(const std::vector<Assignment>& assignments, const std::string& scope = "");

struct MountIdentity {
    std::uint64_t mount_id = 0;
    std::uint64_t parent_id = 0;  // live mountinfo ancestry, not persisted identity
    std::uint64_t device = 0;
    std::uint64_t inode = 0;
    std::string root, path, filesystem, source, namespace_id;
};

struct MountContribution {
    std::string path;
    std::vector<std::string> owners;
    bool committed = false;
    bool captured = false;
    MountIdentity identity;
};

// Keeps the attachment alive until its mountinfo tuple can be read at a safe
// backend boundary. The descriptor pins its mount ID even after a detach.
class PendingMountCapture {
public:
    ~PendingMountCapture();
    PendingMountCapture(const PendingMountCapture&) = delete;
    PendingMountCapture& operator=(const PendingMountCapture&) = delete;
    PendingMountCapture(PendingMountCapture&& other) noexcept;
    PendingMountCapture& operator=(PendingMountCapture&& other) noexcept;

private:
    PendingMountCapture() = default;
    int fd_ = -1;
    MountContribution contribution_;
    MountIdentity observed_;
    std::string resolved_;
    friend PendingMountCapture capture_mount_id(const std::string&,
                                                const std::vector<std::string>&);
    friend std::vector<MountContribution> complete_mount_captures(std::vector<PendingMountCapture>);
};

// Call immediately after attachment. Final symlinks and failed observations
// remain uncaptured. Complete in the same namespace, before backend rollback.
PendingMountCapture capture_mount_id(const std::string& path,
                                     const std::vector<std::string>& owners);
// Consumes all pending descriptors and returns one contribution per input in
// input order. Failed captures retain the original path and captured == false.
std::vector<MountContribution> complete_mount_captures(std::vector<PendingMountCapture> captures);

// Call immediately after the actual attachment in the mount child, before
// another backend can obscure it. A failed capture is retained as unknown.
MountContribution capture_mount(const std::string& path, const std::vector<std::string>& owners);
// Capture a completed backend walk before teardown or another backend runs.
// Only committed entries are sampled; mountinfo is read and indexed once.
void capture_mounts(std::vector<MountContribution>& contributions);
bool record_mounts(const std::string& backend, const std::vector<MountContribution>& contributions,
                   bool complete);

struct RuleEvidence {
    std::string kind;  // ADD, MERGE, HIDE
    std::string path;
    std::string source;
    std::vector<std::string> owners;
};

// Supply the final rule winners, including failed expected rules. Live kernel
// readback determines presence; an ioctl return alone does not prove activity.
bool record_rules(const std::vector<RuleEvidence>& rules, bool complete);

struct KasumiObservation {
    bool ok = false;
    bool enabled = false;
    std::vector<RuleEvidence> rules;
    std::string error_code;
    std::string error;
    std::string text;  // optional strict client LIST_RULES snapshot
};

struct Summary {
    std::string state = "unknown";
    std::string backend;
    std::string reason = "no_record";
    std::string error_code;
    std::string error;
};

struct Snapshot {
    std::map<std::string, Summary, std::less<>> modules;
    // Also applies to installed modules absent from modules.
    Summary missing;
};

Snapshot read_snapshot(const KasumiObservation& observation = {});
namespace detail {
struct Record {
    std::string backend;
    bool complete = false;
    std::vector<MountContribution> mounts;
    std::vector<RuleEvidence> rules;
};
struct Database {
    std::string boot;
    std::uint64_t generation = 0;
    std::string namespace_id;
    std::map<std::string, Record> modules;
};

struct SnapshotContext {
    Database database;
    posix::Fd lock;
    std::vector<MountIdentity> mounts;
    std::string mount_error;
    bool mounts_ok = false;
    bool needs_kasumi = false;
};
bool prepare_snapshot(SnapshotContext& context, Snapshot& result);
void finish_snapshot(SnapshotContext& context, const KasumiObservation& observation,
                     Snapshot& result);
}  // namespace detail

// The concrete callback runs under the shared snapshot lock. Its returned
// observation can be borrowed; an injected observation is no longer deep-copied.
template <typename Observer>
Snapshot read_snapshot_observed(Observer&& observe) {
    Snapshot result;
    detail::SnapshotContext context;
    if (!detail::prepare_snapshot(context, result))
        return result;
    if (context.needs_kasumi)
        detail::finish_snapshot(context, observe(), result);
    else
        detail::finish_snapshot(context, KasumiObservation{}, result);
    return result;
}
std::string summary_json(const Summary& summary);

// Pure parsing/comparison seams also used by native fixture tests. Full tuple
// matching preserves obscured mounts without accepting recycled mount IDs.
bool parse_mountinfo(const std::string& text, std::vector<MountIdentity>& mounts,
                     std::string& error);
bool same_mount(const MountIdentity& recorded, const MountIdentity& live);
bool read_complete(int fd, std::string& output);

}  // namespace kagami::mount::module_runtime
