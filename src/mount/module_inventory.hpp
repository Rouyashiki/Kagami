#pragma once

#include "core/arguments.hpp"
#include "core/directory.hpp"

#include <algorithm>
#include <array>
#include <memory>

namespace kagami::mount {

// Prepared request storage, never resized. Filesystem names are captured once;
// retained runtime IDs borrow the immutable snapshot. Sorting moves only views.
class ModuleInventory {
public:
    ModuleInventory() noexcept = default;
    ModuleInventory(const ModuleInventory&) = delete;
    ModuleInventory& operator=(const ModuleInventory&) = delete;
    ModuleInventory(ModuleInventory&&) = delete;
    ModuleInventory& operator=(ModuleInventory&&) = delete;
    ~ModuleInventory() = default;
    struct Entry {
        Argument id;
        bool kasumi_boot;
        bool staged;
    };
    [[nodiscard]] bool read_inventory(const char* root, const char* update_root,
                                      std::size_t runtime_count, int& error) noexcept {
        constexpr std::size_t maximum = 65536;
        error = 0;
        const std::array roots{root, update_root};
        std::array<posix::Fd, 2> directories;
        std::size_t count = 0;
        for (std::size_t i = 0; i < roots.size(); ++i) {
            if (!roots[i])
                continue;
            auto& directory = directories[i];
            directory = posix::Fd(open(roots[i], O_RDONLY | O_DIRECTORY | O_CLOEXEC));
            if (directory.get() < 0) {
                if (errno == ENOENT || errno == ENOTDIR)
                    continue;
                error = errno;
                return false;
            }
            char bytes[4096];
            posix::Directory scan(directory.get(), bytes, sizeof(bytes));
            posix::DirectoryEntry item;
            while (scan.next(item, error)) {
                if (++count > maximum) {
                    error = EOVERFLOW;
                    return false;
                }
            }
            if (error)
                return false;
            if (lseek(directory.get(), 0, SEEK_SET) < 0) {
                error = errno;
                return false;
            }
        }
        if (runtime_count > maximum - count || maximum - count - runtime_count < 32) {
            error = EOVERFLOW;
            return false;
        }
        capacity_ = count + runtime_count + 32;
        // Entry is trivial, so unused slots are not initialized or copied.
        entries_.reset(new (std::nothrow) Entry[capacity_]);
        if (!entries_ || !names_.prepare_storage((count + 32) * 256, maximum * 256)) {
            error = ENOMEM;
            return false;
        }
        return read_directory(directories[0].get(), false, error) &&
               read_directory(directories[1].get(), true, error);
    }

    [[nodiscard]] bool append_runtime(std::string_view id) noexcept {
        if (count_ == capacity_)
            return false;
        entries_[count_++] = {Argument(id), false, false};
        return true;
    }
    void sort() noexcept {
        std::sort(entries_.get(), entries_.get() + count_, [](const Entry& a, const Entry& b) {
            const auto order = std::string_view(a.id).compare(b.id);
            return order == 0 ? a.staged > b.staged : order < 0;
        });
        const auto* last = std::unique(
            entries_.get(), entries_.get() + count_,
            [](const Entry& a, const Entry& b) { return a.id == std::string_view(b.id); });
        count_ = static_cast<std::size_t>(last - entries_.get());
    }
    void mark_kasumi(std::string_view id) noexcept {
        auto* found = std::lower_bound(entries_.get(), entries_.get() + count_, id,
                                       [](const Entry& entry, std::string_view key) {
                                           return std::string_view(entry.id) < key;
                                       });
        if (found != entries_.get() + count_ && found->id == id)
            found->kasumi_boot = true;
    }
    const Entry* begin() const noexcept { return entries_.get(); }
    const Entry* end() const noexcept { return entries_.get() + count_; }

private:
    [[nodiscard]] bool read_directory(int directory, bool staged, int& error) noexcept {
        error = 0;
        if (directory < 0)
            return true;
        char bytes[4096];
        posix::Directory scan(directory, bytes, sizeof(bytes));
        posix::DirectoryEntry item;
        while (scan.next(item, error)) {
            bool is_directory = item.type == 4;
            if (item.type == 0 || item.type == 10) {
                // fstatat fully initializes the fields read on success.
                // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
                struct stat info;
                if (fstatat(directory, item.c_str(), &info, 0) != 0) {
                    if (errno == ENOENT)
                        continue;
                    error = errno;
                    return false;
                }
                is_directory = S_ISDIR(info.st_mode);
            }
            if (!is_directory)
                continue;
            if (item.name.size() + 1 > names_.size() - used_ || count_ == capacity_) {
                error = ENOBUFS;
                return false;
            }
            char* name = names_.data() + used_;
            std::memcpy(name, item.name.data(), item.name.size() + 1);
            used_ += item.name.size() + 1;
            entries_[count_++] = {Argument(std::string_view(name, item.name.size())), false,
                                  staged};
        }
        return error == 0;
    }

    std::unique_ptr<Entry[]> entries_;
    bounded::Storage names_;
    std::size_t capacity_ = 0, count_ = 0, used_ = 0;
};

}  // namespace kagami::mount
