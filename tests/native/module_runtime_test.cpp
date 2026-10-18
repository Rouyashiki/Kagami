#include "mount/module_runtime.hpp"
#include "core/runtime.hpp"

#include <fcntl.h>
#include <sched.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace runtime = kagami::mount::module_runtime;
namespace fs = std::filesystem;
namespace {
void require(bool value, const std::string& message) {
    if (!value)
        throw std::runtime_error(message);
}
struct Fixture {
    fs::path path;
    bool mounted_backing = false;
    explicit Fixture(bool mount_backing = false) {
        char pattern[] = "/data/local/tmp/kagami-runtime-test-XXXXXX";
        const auto* created = mkdtemp(pattern);
        require(created != nullptr, "create fixture");
        path = created;
        if (mount_backing) {
            require(mount("kagami-runtime-fixture", path.c_str(), "tmpfs", 0, "mode=0700") == 0,
                    "prepare private overlay-compatible fixture filesystem");
            mounted_backing = true;
        }
        require(setenv("KAGAMI_DATA_DIR", path.c_str(), 1) == 0, "set fixture directory");
        require(setenv("KAGAMI_MOUNT_HERE", "1", 1) == 0, "select private namespace");
    }
    ~Fixture() {
        if (mounted_backing)
            (void)umount2(path.c_str(), MNT_DETACH);
        std::error_code error;
        fs::remove_all(path, error);
    }
    fs::path state() const { return path / "run/module_runtime/state"; }
};
runtime::Summary summary(const std::string& id, const runtime::KasumiObservation& obs = {}) {
    const auto snapshot = runtime::read_snapshot(obs);
    const auto found = snapshot.modules.find(id);
    return found == snapshot.modules.end() ? snapshot.missing : found->second;
}
void require_state(const std::string& id, const std::string& expected,
                   const runtime::KasumiObservation& obs = {}) {
    const auto actual = summary(id, obs);
    require(actual.state == expected,
            id + ": expected " + expected + ", got " + runtime::summary_json(actual));
}
void write_text(const fs::path& path, const std::string& text) {
    std::ofstream out(path);
    out << text;
    require(out.good(), "write fixture " + path.string());
}
void test_parser() {
    int pipe_fds[2];
    require(pipe2(pipe_fds, O_CLOEXEC | O_NONBLOCK) == 0, "create read failure pipe");
    require(write(pipe_fds[1], "complete\n", 9) == 9, "write complete line before error");
    std::string partial;
    require(
        !runtime::read_complete(pipe_fds[0], partial) && partial == "complete\n" && errno == EAGAIN,
        "complete lines before an I/O error cannot become a complete snapshot");
    require(write(pipe_fds[1], "complete\n", 9) == 9, "write EOF fixture");
    close(pipe_fds[1]);
    require(runtime::read_complete(pipe_fds[0], partial) && partial == "complete\n",
            "read to clean EOF");
    close(pipe_fds[0]);
    std::vector<runtime::MountIdentity> mounts;
    std::string error;
    const std::string fixture =
        "41 21 0:50 / /system/etc rw,relatime shared:7 - tmpfs kagami rw\n"
        "42 21 0:51 / /system/etc ro,relatime - overlay kagami ro,lowerdir=/a:/b\n"
        "43 21 8:1 /source\\040dir /target\\040dir ro - ext4 /dev/block/dm-0 ro\n";
    require(runtime::parse_mountinfo(fixture, mounts, error), "parse layered mounts");
    require(mounts.size() == 3, "retain underlying mount at duplicate target");
    require(mounts[2].path == "/target dir" && mounts[2].root == "/source dir", "decode paths");
    const auto lower = mounts[0];
    require(runtime::same_mount(lower, mounts[0]), "underlying attachment remains identifiable");
    auto reused = lower;
    reused.root = "/another";
    require(!runtime::same_mount(lower, reused), "reject recycled ID with changed mount root");
    reused = lower;
    reused.device++;
    require(!runtime::same_mount(lower, reused), "reject changed mount device");
    reused = lower;
    reused.namespace_id = "other";
    require(!runtime::same_mount(lower, reused), "reject another namespace");
    require(!runtime::parse_mountinfo(fixture + "bad\n", mounts, error), "reject malformed row");
    require(!runtime::parse_mountinfo(fixture.substr(0, fixture.size() - 1), mounts, error),
            "reject truncated snapshot");
    require(!runtime::parse_mountinfo("", mounts, error), "empty snapshot is not absence proof");
}
void test_alias_capture() {
    Fixture fixture;
    const auto alias = fixture.path / "root";
    fs::create_directory_symlink("/", alias);
    const auto path = (alias / "proc/../proc").string();
    const auto direct = runtime::capture_mount("/proc", {"direct"});
    const auto single = runtime::capture_mount(path, {"single"});
    require(direct.captured && single.captured, "capture through an intermediate symlink");
    require(single.path == "/proc" && single.identity.path == single.path,
            "persist the resolved mount point");
    require(runtime::same_mount(direct.identity, single.identity) &&
                direct.identity.inode == single.identity.inode,
            "an alias identifies the same attachment");
    const auto leaf = fixture.path / "leaf";
    fs::create_directory_symlink("/proc", leaf);
    require(!runtime::capture_mount(leaf.string(), {"leaf"}).captured,
            "do not follow a final symlink when capturing an attachment");
    std::vector<runtime::MountContribution> batch = {
        {path, {"batch"}, true, false, {}},
        {(alias / "proc/version").string(), {"descendant"}, true, false, {}},
        {(alias / "proc/kagami-missing-mount").string(), {"missing"}, true, false, {}},
        {path, {"uncommitted"}, false, false, {}},
        {leaf.string(), {"leaf"}, true, false, {}},
    };
    runtime::capture_mounts(batch);
    require(batch[0].captured && batch[0].path == "/proc" &&
                runtime::same_mount(single.identity, batch[0].identity),
            "batch capture resolves the same attachment");
    for (std::size_t i = 1; i < batch.size(); ++i)
        require(!batch[i].captured, "do not infer an attachment from an invalid target");
    {
        runtime::Operation operation;
        require(runtime::assign_modules({{"single", "overlay"}, {"batch", "magic"}}),
                "assign alias fixture");
        require(runtime::record_mounts("overlay", {single}, true), "record single alias");
        require(runtime::record_mounts("magic", {batch[0]}, true), "record batch alias");
        operation.finish();
    }
    fs::remove(alias);
    require_state("single", "mounted");
    require_state("batch", "mounted");
}
void test_lifecycle() {
    Fixture fixture;
    require_state("new", "unknown");
    {
        runtime::Operation operation;
        require(runtime::assign_modules({{"magic", "magic"}, {"none", "none"}}), "assign");
        require_state("magic", "unknown");
        operation.finish();
    }
    require_state("magic", "unknown");
    require_state("none", "unmounted");
    {
        runtime::Operation outer;
        {
            runtime::Operation inner;
            require(runtime::record_mounts("magic", {}, true), "record empty native plan");
            inner.finish();
        }
        require(summary("magic").reason == "updating", "nested finish cannot publish parent");
        outer.finish();
    }
    require_state("magic", "unmounted");
    {
        runtime::Operation operation;
        // Emulate a mount process that exits before explicitly publishing.
        require(runtime::assign_modules({{"next", "overlay"}}), "assign interrupted");
    }
    require(summary("next").reason == "interrupted", "unfinished generation stays unknown");
    {
        runtime::Operation unrelated;
        unrelated.finish();
    }
    require_state("next", "unknown");
    require(summary("next").reason == "record_write_failed",
            "unrelated operation cannot repair interrupted evidence");
    {
        runtime::Operation operation;
        require(runtime::assign_modules({{"child", "magic"}}), "assign child plan");
        const auto child = fork();
        require(child >= 0, "fork fixture");
        if (child == 0) {
            runtime::Operation nested;
            const bool ok = runtime::record_mounts("magic", {}, true);
            nested.finish();
            _exit(ok ? 0 : 1);
        }
        int status = 0;
        require(
            waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
            "child publishes under inherited lock");
        operation.finish();
    }
    require_state("child", "unmounted");
    int signal_pipe[2];
    require(pipe(signal_pipe) == 0, "create inherited descriptor fixture");
    pid_t observer = -1;
    {
        runtime::Operation operation;
        observer = fork();
        require(observer >= 0, "fork idle inherited descriptor holder");
        if (observer == 0) {
            close(signal_pipe[1]);
            char value = 0;
            (void)read(signal_pipe[0], &value, 1);
            _exit(0);
        }
        close(signal_pipe[0]);
        operation.finish();
    }
    require_state("child", "unmounted");
    require(write(signal_pipe[1], "x", 1) == 1, "release idle child");
    close(signal_pipe[1]);
    int observer_status = 0;
    require(waitpid(observer, &observer_status, 0) == observer, "join idle child");
    write_text(fixture.state(), "KAGAMI_MODULE_RUNTIME_V2 \"different-boot\" 1 \"fixture\"\n");
    require(summary("child").reason == "stale_boot", "old boot is unknown");
    write_text(fixture.state(), "KAGAMI_MODULE_RUNTIME_V2 \"" + kagami::runtime_boot_id() +
                                    "\" 1 \"fixture\"\nmodule \"child\" magic 1\ninvalid\n");
    require(summary("child").state == "unknown", "corrupt tail cannot partially publish");
    require(summary("child").error_code == "invalid_record", "corruption carries read error");
    {
        runtime::Operation unrelated;
        unrelated.finish();
    }
    require(summary("child").error_code == "invalid_record",
            "unrelated operation cannot erase corruption");
    {
        runtime::Operation boot;
        require(runtime::assign_modules({{"recovered", "none"}}),
                "new boot inventory recovers corruption");
        boot.finish();
    }
    require_state("recovered", "unmounted");
    write_text(fixture.state(), "KAGAMI_MODULE_RUNTIME_V2 \"" + kagami::runtime_boot_id() +
                                    "\" 7 \"different-namespace\"\nmodule \"recovered\" none 1\n");
    require(summary("recovered").reason == "namespace_changed", "namespace guards all backends");
}
void test_rules() {
    Fixture fixture;
    const std::vector<runtime::RuleEvidence> expected = {
        {"ADD", "/system/a", "/modules/one/system/a", {"one"}},
        {"MERGE", "/system/etc", "/modules/one/system/etc", {"one"}},
        {"MERGE", "/system/etc", "/modules/two/system/etc", {"two"}},
        {"HIDE", "/system/hidden", "", {"one", "two"}},
    };
    {
        runtime::Operation operation;
        require(
            runtime::assign_modules({{"one", "kasumi"}, {"two", "kasumi"}, {"other", "overlay"}}),
            "assign rules");
        require(runtime::record_rules(expected, true), "record expected rules");
        require(runtime::record_mounts("overlay", {}, true), "record other backend");
        operation.finish();
    }
    runtime::KasumiObservation live;
    live.ok = true;
    live.enabled = true;
    live.rules = expected;
    require_state("one", "mounted", live);
    require_state("two", "mounted", live);
    write_text(fixture.path / "module_mode.json", "{\"one\":\"magic\"}\n");
    require_state("one", "mounted", live);
    require(summary("one", live).backend == "kasumi", "saved mode cannot rewrite runtime backend");
    live.rules.erase(live.rules.begin());
    require_state("one", "partial", live);
    require_state("two", "mounted", live);
    live.rules.clear();
    require_state("one", "unmounted", live);
    live.rules = expected;
    live.enabled = false;
    require_state("one", "unmounted", live);
    live.ok = false;
    live.error_code = "EIO";
    live.error = "rule read failed";
    require_state("one", "unknown", live);
    require(summary("one", live).error_code == "EIO", "preserve read failure");
    require_state("other", "unmounted", live);
    live = {};
    live.ok = true;
    live.enabled = true;
    live.text = "Kasumi Protocol: 17\nKasumi Enabled: 1\n"
                "add /system/a /modules/one/system/a 0\n"
                "merge /system/etc /modules/one/system/etc\n"
                "merge /system/etc /modules/two/system/etc\n"
                "hide /system/hidden\n";
    require_state("one", "mounted", live);
    require_state("two", "mounted", live);
    live.text += "add /ambiguous path /source 0\n";
    require_state("one", "unknown", live);
    int calls = 0;
    const auto callback =
        runtime::read_snapshot_observed([&]() -> const runtime::KasumiObservation& {
            ++calls;
            return live;
        });
    require(calls == 1 && callback.modules.at("one").state == "unknown", "query callback");
    {
        runtime::Operation operation;
        require(runtime::assign_modules({{"one", "kasumi"}}, "kasumi"), "refresh scope");
        require(runtime::record_rules({}, true), "clear refreshed rules");
        operation.finish();
    }
    require_state("one", "unmounted");
    require_state("two", "unmounted");
    require_state("other", "unmounted");
    calls = 0;
    (void)runtime::read_snapshot_observed([&]() -> const runtime::KasumiObservation& {
        ++calls;
        return live;
    });
    require(calls == 0, "no unnecessary Kasumi query without rules");
}
void test_failure() {
    Fixture fixture;
    {
        runtime::Operation operation;
        require(runtime::assign_modules({{"failed", "magic"}}), "assign capture failure");
        runtime::MountContribution bad{"/does-not-exist", {"failed"}, true, false, {}};
        require(runtime::record_mounts("magic", {bad}, true), "persist capture failure");
        operation.finish();
    }
    require(summary("failed").reason == "capture_failed", "capture failure cannot be unmounted");
    write_text(fixture.path / "run/module_runtime/invalid", "observation_write_failed\n");
    {
        runtime::Operation unrelated;
        unrelated.finish();
    }
    require(summary("failed").reason == "record_write_failed",
            "unrelated successful metadata write cannot repair missing evidence");
    {
        runtime::Operation new_boot;
        require(runtime::assign_modules({{"failed", "none"}}), "rebuild invalid inventory");
        new_boot.finish();
    }
    require_state("failed", "unmounted");
    {
        runtime::Operation operation;
        fs::remove(fixture.state());
        fs::create_directory(fixture.state());
        require(!runtime::record_mounts("magic", {}, true), "report persistence failure");
        operation.finish();
    }
    require_state("failed", "unknown");
}
void test_namespace_observation(const fs::path& target, std::uint64_t mount_id) {
    const auto parent_proc = "/proc/" + std::to_string(getpid());
    const auto child = fork();
    require(child >= 0, "fork namespace reader fixture");
    if (child == 0) {
        try {
            require(unshare(CLONE_NEWNS) == 0, "separate reader mount namespace");
            require(mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) == 0,
                    "separate reader propagation");
            require(umount2(target.c_str(), MNT_DETACH) == 0,
                    "reader does not see the parent's module mount");
            const auto before = fs::read_symlink("/proc/self/ns/mnt");
            std::string text;
            std::string error;
            require(runtime::detail::read_namespace_mountinfo(parent_proc, text, error), error);
            require(fs::read_symlink("/proc/self/ns/mnt") == before,
                    "query must not move the caller into the mount namespace");
            std::vector<runtime::MountIdentity> mounts;
            require(runtime::parse_mountinfo(text, mounts, error), error);
            require(std::any_of(mounts.begin(), mounts.end(),
                                [&](const auto& entry) {
                                    return entry.mount_id == mount_id &&
                                           entry.path == target.string();
                                }),
                    "read the mounted target from the original namespace");
            require(
                !runtime::detail::read_namespace_mountinfo("/proc/kagami-missing", text, error) &&
                    !error.empty(),
                "namespace read failure is explicit");
            _exit(0);
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            _exit(1);
        }
    }
    int status = 0;
    require(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
            "isolated namespace reader checks");
}
int test_mounts() {
    if (unshare(CLONE_NEWNS) != 0) {
        std::cerr << "mount fixture requires an isolated mount namespace\n";
        return 77;
    }
    require(mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) == 0,
            "make namespace private");
    // Android /data can use casefold; keep OverlayFS fixtures on private tmpfs.
    Fixture fixture(true);
    const auto target = fixture.path / "target";
    const auto source = fixture.path / "source";
    const auto alias = fixture.path / "alias";
    fs::create_directory_symlink(fixture.path, alias);
    fs::create_directory(target);
    fs::create_directory(source);
    require(mount("kagami-fixture", target.c_str(), "tmpfs", 0, nullptr) == 0,
            "mount shared skeleton");
    write_text(source / "payload", "module content\n");
    write_text(target / "file", "stock content\n");
    require(mount((source / "payload").c_str(), (target / "file").c_str(), nullptr, MS_BIND,
                  nullptr) == 0,
            "direct module bind");
    std::vector<runtime::MountContribution> batch = {
        {(alias / "target").string(), {"one", "two"}, true, false, {}},
        {(alias / "target/file").string(), {"one"}, true, false, {}},
        {target.string(), {"not-committed"}, false, false, {}},
        {(target / "missing").string(), {"capture-failed"}, true, false, {}},
    };
    runtime::capture_mounts(batch);
    const auto lower = batch[0];
    const auto direct = batch[1];
    require(lower.captured && direct.captured, "batch captures committed attachments");
    require(lower.path == target.string() && direct.path == (target / "file").string(),
            "batch records resolved shared and bind mount points");
    require(!batch[2].captured && batch[2].identity.mount_id == 0,
            "batch does not infer an uncommitted attachment from a matching path");
    require(!batch[3].captured, "batch retains failed capture independently");
    test_namespace_observation(target, lower.identity.mount_id);
    {
        runtime::Operation operation;
        require(runtime::assign_modules({{"one", "magic"}, {"two", "magic"}, {"upper", "overlay"}}),
                "assign mounted fixture");
        require(runtime::record_mounts("magic", {lower, direct}, true),
                "record shared skeleton and bind");
        operation.finish();
    }
    require_state("one", "mounted");
    require_state("two", "mounted");
    {
        runtime::detail::SnapshotContext context;
        runtime::Snapshot filtered;
        require(runtime::detail::prepare_snapshot(context, filtered), "prepare filtered view");
        // Mount hiding can omit a live attachment from an otherwise valid proc
        // table. The real statx identity must not be discarded in that case.
        context.mounts.erase(std::remove_if(context.mounts.begin(), context.mounts.end(),
                                            [&](const auto& entry) {
                                                return entry.mount_id == direct.identity.mount_id ||
                                                       entry.mount_id == lower.identity.mount_id;
                                            }),
                             context.mounts.end());
        runtime::detail::finish_snapshot(context, {}, filtered);
        require(filtered.modules.at("one").state == "mounted" &&
                    filtered.modules.at("two").state == "mounted",
                "omitted live mountinfo entries must not turn actual attachments into unmounted");
    }
    const std::string overlay_options = "lowerdir=" + source.string() + ":" + target.string();
    require(mount("kagami-fixture-overlay", target.c_str(), "overlay", MS_RDONLY,
                  overlay_options.c_str()) == 0,
            "overlay above lower skeleton");
    const auto upper = runtime::capture_mount((alias / "target").string(), {"upper"});
    require(upper.captured, "capture upper overlay");
    require(upper.path == target.string(), "record resolved overlay mount point");
    {
        runtime::Operation operation;
        require(runtime::record_mounts("overlay", {upper}, true), "record upper overlay");
        operation.finish();
    }
    require_state("one", "mounted");
    require_state("two", "mounted");
    require_state("upper", "mounted");
    require(umount2(target.c_str(), MNT_DETACH) == 0, "detach upper overlay");
    require_state("upper", "unmounted");
    require_state("one", "mounted");
    const auto empty = fixture.path / "empty";
    fs::create_directory(empty);
    const std::string hiding_options = "lowerdir=" + source.string() + ":" + empty.string();
    require(mount("kagami-fixture-overlay", target.c_str(), "overlay", MS_RDONLY,
                  hiding_options.c_str()) == 0,
            "overlay hides lower bind path completely");
    require(!fs::exists(target / "file"), "lower bind is inaccessible through upper overlay");
    {
        runtime::Operation operation;
        const auto hiding_upper = runtime::capture_mount(target.string(), {"upper"});
        require(runtime::record_mounts("overlay", {hiding_upper}, true), "record hiding overlay");
        operation.finish();
    }
    require_state("one", "mounted");
    require_state("two", "mounted");
    require_state("upper", "mounted");
    require(umount2(target.c_str(), MNT_DETACH) == 0, "detach hiding overlay");
    require(umount2((target / "missing").c_str(), MNT_DETACH) != 0, "simulate rejected unmount");
    require_state("one", "mounted");
    require(umount2((target / "file").c_str(), MNT_DETACH) == 0, "detach individual contribution");
    require_state("one", "partial");
    require_state("two", "mounted");
    require(umount2(target.c_str(), MNT_DETACH) == 0, "detach shared skeleton");
    require_state("one", "unmounted");
    require_state("two", "unmounted");
    return 0;
}
}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc > 1 && std::string(argv[1]) == "--mounts")
            return test_mounts();
        test_parser();
        test_alias_capture();
        test_lifecycle();
        test_rules();
        test_failure();
        std::cout << "module runtime fixtures passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
