#include "mount/module_scan.hpp"

#include <algorithm>

namespace kagami::mount {

bool ModuleScanner::partition(std::string_view name, bool visible) const noexcept {
    if ((!visible || config_->partitions.empty()) &&
        std::find(builtin.begin(), builtin.end(), name) != builtin.end())
        return true;
    return std::any_of(config_->partitions.begin(), config_->partitions.end(),
                       [name](const auto& part) { return name == part; });
}

bool ModuleScanner::direct_files(int fd, std::size_t buffer_index,
                                 bool scan_subpartitions) noexcept {
    posix::Directory entries(fd, storage_.data() + (buffer_index * directory_bytes),
                             directory_bytes);
    posix::DirectoryEntry entry;
    int error = 0;
    while (entries.next(entry, error)) {
        if (!posix::is_directory(fd, entry))
            return true;
        if (scan_subpartitions && entry.name != "system" && partition(entry.name, false)) {
            const posix::Fd child(openat(fd, entry.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
            if (child.get() >= 0 && direct_files(child.get(), 2, false))
                return true;
        }
    }
    // Previous backend probing treated unreadable directory contents as empty.
    return false;
}

bool ModuleScanner::read_properties(int fd, ModuleProperties& props, int& error) noexcept {
    if (storage_.size() < storage_bytes) {
        error = ENOBUFS;
        return false;
    }
    const posix::Fd file(openat(fd, "module.prop", O_RDONLY | O_CLOEXEC));
    if (file.get() < 0)
        return true;  // Metadata fields were empty for unreadable module.prop.
    char* bytes = storage_.data() + (directory_bytes * 3);
    std::size_t used = 0;
    for (;;) {
        const auto count = read(file.get(), bytes + used, property_bytes + 1 - used);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            break;
        used += static_cast<std::size_t>(count);
        if (used > property_bytes) {
            error = EOVERFLOW;
            return false;
        }
    }
    std::string_view text(bytes, used);
    while (!text.empty()) {
        const auto newline = text.find('\n');
        const std::string_view line(text.data(),
                                    newline == std::string_view::npos ? text.size() : newline);
        const auto equal = line.find('=');
        if (equal != std::string_view::npos) {
            auto key = line;
            key.remove_suffix(line.size() - equal);
            auto value = line;
            value.remove_prefix(equal + 1);
            if (key == "name") {
                props.name = value;
                props.has_name = true;
            } else if (key == "version")
                props.version = value;
            else if (key == "author")
                props.author = value;
            else if (key == "description")
                props.description = value;
        }
        if (newline == std::string_view::npos)
            break;
        text.remove_prefix(newline + 1);
    }
    return true;
}

void ModuleScanner::probe_unlisted(int fd, ModuleFacts& facts) noexcept {
    // fstatat initializes all fields consumed on its success branch.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
    struct stat st;
    facts.has_prop = fstatat(fd, "module.prop", &st, 0) == 0;
    facts.inactive = fstatat(fd, "disable", &st, 0) == 0 || fstatat(fd, "remove", &st, 0) == 0 ||
                     fstatat(fd, "skip_mount", &st, 0) == 0;
    for (const auto& part : builtin) {
        if (fstatat(fd, part.c_str(), &st, 0) != 0 || !S_ISDIR(st.st_mode))
            continue;
        facts.backend_content = true;
        facts.visible_content = facts.visible_content || partition(part, true);
        const posix::Fd directory(openat(fd, part.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
        if (inspect_root_files_ && !facts.needs_magic && directory.get() >= 0)
            facts.needs_magic =
                direct_files(directory.get(), 1, std::string_view(part) == "system");
    }
}

bool ModuleScanner::observe(const char* path, ModuleFacts& facts, ModuleProperties& props,
                            int& error, bool inspect_root_files, bool load_properties) noexcept {
    observed_ = posix::Fd{};
    inspect_root_files_ = inspect_root_files;
    facts = {};
    props = {};
    error = 0;
    posix::Fd module(open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    bool listed = module.get() >= 0;
    if (!listed)
        module = posix::Fd(open(path, O_PATH | O_DIRECTORY | O_CLOEXEC));
    if (module.get() < 0)
        return true;
    posix::Directory entries(module.get(), storage_.data(), directory_bytes);
    posix::DirectoryEntry entry;
    while (listed && entries.next(entry, error)) {
        if (entry.name == "module.prop")
            facts.has_prop = posix::exists(module.get(), entry);
        if (entry.name == "disable" || entry.name == "remove" || entry.name == "skip_mount")
            facts.inactive = facts.inactive || posix::exists(module.get(), entry);
        if (partition(entry.name, false) && posix::is_directory(module.get(), entry)) {
            facts.visible_content = facts.visible_content || partition(entry.name, true);
            facts.backend_content = true;
            if (inspect_root_files_ && !facts.needs_magic) {
                const posix::Fd child(
                    openat(module.get(), entry.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
                if (child.get() >= 0)
                    facts.needs_magic = direct_files(child.get(), 1, entry.name == "system");
            }
        }
    }
    if (!listed || error) {
        facts = {};
        probe_unlisted(module.get(), facts);
        listed = false;
        error = 0;
    }
    // Config historically accepts paths, not just partition component names.
    // Keep those semantics without probing absent ordinary partitions per file.
    for (const auto& part : config_->partitions) {
        const std::string_view part_view(part);
        const bool component = !part.empty() && part != "." && part != ".." &&
                               part_view.find('/') == std::string_view::npos &&
                               part_view.find('\0') == std::string_view::npos;
        if (listed && component)
            continue;
        const char* name = part.empty() ? "." : part.c_str();
        // fstatat initializes all fields consumed on its success branch.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
        struct stat st;
        const bool exists = fstatat(module.get(), name, &st, 0) == 0 && S_ISDIR(st.st_mode);
        const posix::Fd custom(
            exists ? openat(module.get(), name, O_RDONLY | O_DIRECTORY | O_CLOEXEC) : -1);
        if (exists) {
            facts.visible_content = true;
            facts.backend_content = facts.backend_content || !part.empty();
        }
        if (custom.get() >= 0) {
            if (!part.empty() && inspect_root_files_ && !facts.needs_magic)
                facts.needs_magic = direct_files(custom.get(), 1, false);
        }
        if (!part.empty() && part != "system" && inspect_root_files_ && !facts.needs_magic) {
            const posix::Fd system(
                openat(module.get(), "system", O_PATH | O_DIRECTORY | O_CLOEXEC));
            const posix::Fd nested(
                system.get() >= 0 ? openat(system.get(), name, O_RDONLY | O_DIRECTORY | O_CLOEXEC)
                                  : -1);
            if (nested.get() >= 0)
                facts.needs_magic = direct_files(nested.get(), 1, false);
        }
    }
    if (!facts.has_prop)
        return true;
    observed_ = std::move(module);
    return !load_properties || observed_properties(props, error);
}

}  // namespace kagami::mount
