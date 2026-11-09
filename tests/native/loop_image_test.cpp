#include "core/runtime.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;
namespace {
constexpr const char* image_context = "u:object_r:kagami_image:s0";
constexpr const char* record_context = "u:object_r:adb_data_file:s0";
std::map<std::string, std::string> labels;
std::string fail_path;
unsigned label_writes = 0;

void require(bool condition, const std::string& message) {
    if (!condition)
        throw std::runtime_error(message);
}

class Fixture {
    fs::path root_;

public:
    Fixture() {
        char pattern[] = "/data/local/tmp/kagami-loop-image-XXXXXX";
        const auto* created = mkdtemp(pattern);
        require(created != nullptr, "create fixture");
        root_ = created;
        require(setenv("KAGAMI_DATA_DIR", root_.c_str(), 1) == 0, "set runtime directory");
    }
    ~Fixture() {
        std::error_code ec;
        fs::remove_all(root_, ec);
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
    Fixture(Fixture&&) = delete;
    Fixture& operator=(Fixture&&) = delete;
    [[nodiscard]] const fs::path& root() const noexcept { return root_; }
    [[nodiscard]] fs::path file(const char* name) const {
        const auto path = root_ / name;
        std::ofstream output(path);
        output << "fixture\n";
        require(output.good() && chmod(path.c_str(), 0644) == 0, "write fixture");
        labels[path.string()] = record_context;
        return path;
    }
};
}  // namespace

// Exercise the Android labeling path without changing the host SELinux policy.
extern "C" ssize_t test_lgetxattr(const char* path, const char* name, void* value,
                                  size_t size) __asm__("__wrap_lgetxattr");
extern "C" ssize_t test_lgetxattr(const char* path, const char* name, void* value, size_t size) {
    const auto label = labels.find(path);
    if (std::strcmp(name, "security.selinux") != 0 || label == labels.end()) {
        errno = ENODATA;
        return -1;
    }
    const auto length = label->second.size() + 1;
    if (size < length) {
        errno = ERANGE;
        return -1;
    }
    std::memcpy(value, label->second.c_str(), length);
    return static_cast<ssize_t>(length);
}

extern "C" int test_lsetxattr(const char* path, const char* name, const void* value, size_t size,
                              int flags) __asm__("__wrap_lsetxattr");
extern "C" int test_lsetxattr(const char* path, const char* name, const void* value, size_t size,
                              int /*flags*/) {
    if (fail_path == path) {
        errno = EACCES;
        return -1;
    }
    const auto* context = static_cast<const char*>(value);
    if (std::strcmp(name, "security.selinux") != 0 || size == 0 || context[size - 1] != '\0' ||
        std::strlen(context) + 1 != size) {
        errno = EINVAL;
        return -1;
    }
    labels[path] = context;
    ++label_writes;
    return 0;
}

int main() {
    try {
        const Fixture fixture;
        std::string error;
        const auto ext4 = fixture.file("mirror.img");
        const auto erofs = fixture.file("mirror.erofs");
        const auto config = fixture.file("config.json");
        require(kagami::prepare_runtime(error), error);
        for (const auto& path : {ext4, erofs}) {
            require(kagami::prepare_loop_image(path, error), error);
            struct stat st{};
            require(lstat(path.c_str(), &st) == 0 && (st.st_mode & 07777) == 0600 &&
                        st.st_uid == geteuid(),
                    "loop image stays private");
            require(labels.at(path.string()) == image_context, "set loop worker context");
            const auto writes = label_writes;
            require(kagami::prepare_loop_image(path, error) && label_writes == writes,
                    "keep an already labeled image");
        }
        require(kagami::prepare_runtime(error), error);
        require(
            labels.at(ext4.string()) == image_context && labels.at(erofs.string()) == image_context,
            "controller restart preserves mounted image labels");
        require(labels.at(config.string()) == record_context, "keep private controller context");

        const auto rejected = fixture.file("denied.img");
        fail_path = rejected;
        require(!kagami::prepare_loop_image(rejected, error) && error.find("label ") == 0,
                "propagate a denied label operation");
        fail_path.clear();
        fs::create_symlink(ext4, fixture.root() / "symlink.img");
        require(!kagami::prepare_loop_image(fixture.root() / "symlink.img", error),
                "reject symlink");
        fs::create_hard_link(ext4, fixture.root() / "hardlink.img");
        require(!kagami::prepare_loop_image(ext4, error), "reject a shared backing inode");
        require(!kagami::prepare_loop_image(fixture.root(), error), "reject directory");
        require(!kagami::prepare_loop_image(fixture.root() / "missing.img", error),
                "reject missing image");
        std::cout << "Loop image metadata fixtures passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
