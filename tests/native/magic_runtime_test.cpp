#include "mount/magic_mount.hpp"
#include "mount/module_runtime.hpp"

#include <sched.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;
namespace runtime = kagami::mount::module_runtime;
namespace magic = kagami::mount::magic;
namespace {
void require(bool condition, const std::string& message) {
    if (!condition)
        throw std::runtime_error(message);
}
void file(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path);
    output << text;
    require(output.good(), "write " + path.string());
}
std::string contents(const fs::path& path) {
    std::ifstream input(path);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
void state(const runtime::Snapshot& snapshot, const std::string& id, const std::string& expected) {
    const auto& actual = snapshot.modules.at(id);
    require(actual.state == expected, id + ": " + runtime::summary_json(actual));
}
int isolated_test(const fs::path& root) {
    if (unshare(CLONE_NEWNS) != 0)
        return 77;
    require(mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) == 0, "isolate propagation");
    require(mount("kagami-fixture-root", root.c_str(), "tmpfs", 0, nullptr) == 0, "fixture root");
    fs::create_directories(root / "proc");
    require(mount("proc", (root / "proc").c_str(), "proc", 0, nullptr) == 0, "fixture proc");
    require(chroot(root.c_str()) == 0 && chdir("/") == 0, "enter temporary fixture root");
    require(setenv("KAGAMI_DATA_DIR", "/runtime", 1) == 0, "runtime fixture path");
    require(setenv("KAGAMI_MOUNT_HERE", "1", 1) == 0, "runtime private namespace");
    require(mkdir("/runtime", 0700) == 0, "private runtime directory");
    fs::create_directory("/work");
    file("/system/etc/shared", "stock");
    file("/system/etc/second", "stock second");
    file("/system/etc/hidden", "stock hidden");
    file("/system/etc/untouched", "stock untouched");
    file("/system/bin/tool", "stock executable");
    file("/system/replaced/stock", "stock to hide");
    file("/modules/a/system/etc/shared", "winner");
    file("/modules/a/system/etc/second", "second contribution");
    fs::create_directories("/modules/b/system/etc");
    require(symlink("shared", "/modules/b/system/etc/link") == 0, "module symlink");
    file("/modules/c/system/bin/tool", "replacement executable");
    file("/modules/d/system/etc/shared", "shadowed loser");
    fs::create_directories("/modules/e/system");
    fs::create_directories("/modules/h/system/etc");
    require(mknod("/modules/h/system/etc/hidden", S_IFCHR | 0600, makedev(0, 0)) == 0,
            "hide-only whiteout");
    file("/modules/o/system/replaced/.replace", "");
    std::vector<kagami::mount::ModuleEntry> modules;
    std::vector<runtime::Assignment> assignments;
    for (const auto* id : {"a", "b", "c", "d", "e", "h", "o"}) {
        modules.push_back({id, fs::path("/modules") / id});
        assignments.push_back({id, "magic"});
    }
    kagami::Config config;
    config.work_dir = "/work";
    config.mount_source = "kagami-magic-fixture";
    {
        runtime::Operation operation;
        require(runtime::assign_modules(assignments), "assign Magic fixture");
        require(magic::mount_modules(modules, config), "mount actual Magic plan");
        operation.finish();
    }
    require(contents("/system/etc/shared") == "winner", "priority winner visible");
    require(contents("/system/etc/link") == "winner", "new symlink visible");
    require(contents("/system/bin/tool") == "replacement executable", "direct bind visible");
    require(contents("/system/etc/untouched") == "stock untouched", "mirror stock preserved");
    require(!fs::exists("/system/etc/hidden"), "whiteout applied");
    require(!fs::exists("/system/replaced/stock"), "empty replace applied");
    auto snapshot = runtime::read_snapshot();
    for (const auto* id : {"a", "b", "c", "h", "o"})
        state(snapshot, id, "mounted");
    for (const auto* id : {"d", "e"})
        state(snapshot, id, "unmounted");
    require(umount2("/system/etc/shared", MNT_DETACH) == 0,
            "remove one payload from shared skeleton");
    snapshot = runtime::read_snapshot();
    state(snapshot, "a", "partial");
    require(umount2("/system/etc/second", MNT_DETACH) == 0,
            "remove final payload from surviving shared skeleton");
    snapshot = runtime::read_snapshot();
    state(snapshot, "a", "unmounted");
    state(snapshot, "b", "mounted");
    state(snapshot, "h", "mounted");
    {
        runtime::Operation operation;
        require(magic::unmount_all(config), "unmount actual Magic plan");
        operation.finish();
    }
    snapshot = runtime::read_snapshot();
    for (const auto& module : modules)
        state(snapshot, module.id, "unmounted");
    require(contents("/system/etc/shared") == "stock", "stock restored");
    require(contents("/system/etc/hidden") == "stock hidden", "hidden stock restored");
    require(contents("/system/replaced/stock") == "stock to hide", "replace stock restored");
    return 0;
}
}  // namespace

int main() {
    char pattern[] = "/tmp/kagami-magic-test-XXXXXX";
    const auto* created = mkdtemp(pattern);
    if (!created)
        return 1;
    const fs::path root(created);
    const auto child = fork();
    if (child == 0) {
        try {
            _exit(isolated_test(root));
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            _exit(1);
        }
    }
    int status = 0;
    const bool waited = child > 0 && waitpid(child, &status, 0) == child;
    fs::remove(root);  // The child's mounts died with its private namespace.
    if (!waited || !WIFEXITED(status))
        return 1;
    if (WEXITSTATUS(status) == 0)
        std::cout << "Magic backend runtime fixture passed\n";
    return WEXITSTATUS(status);
}
