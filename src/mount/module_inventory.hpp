#pragma once

#include "core/arguments.hpp"
#include "core/directory.hpp"

#include <algorithm>
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
    };
    [[nodiscard]] bool prepare_inventory(const char* root, std::size_t runtime_count,
                                         int& error) noexcept {
        constexpr std::size_t maximum = 65536;
        error = 0;
        directory_ = posix::Fd(open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC));
        std::size_t count = 0;
        if (directory_.get() < 0) {
            if (errno != ENOENT && errno != ENOTDIR) {
                error = errno;
                return false;
            }
        } else {
            char bytes[4096];
            posix::Directory scan(directory_.get(), bytes, sizeof(bytes));
            posix::DirectoryEntry item;
            while (scan.next(item, error)) {
                if (++count > maximum) {
                    error = EOVERFLOW;
                    return false;
                }
            }
            if (error)
                return false;
            if (lseek(directory_.get(), 0, SEEK_SET) < 0) {
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
        return true;
    }
    [[nodiscard]] bool read_directory(int& error) noexcept {
        error = 0;
        if (directory_.get() < 0)
            return true;
        char bytes[4096];
        posix::Directory scan(directory_.get(), bytes, sizeof(bytes));
        posix::DirectoryEntry item;
        while (scan.next(item, error)) {
            bool is_directory = item.type == 4;
            if (item.type == 0 || item.type == 10) {
                // fstatat fully initializes the fields read on success.
                // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
                struct stat info;
                if (fstatat(directory_.get(), item.c_str(), &info, 0) != 0) {
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
            entries_[count_++] = {Argument(std::string_view(name, item.name.size())), false};
        }
        return error == 0;
    }
    [[nodiscard]] bool append_runtime(std::string_view id) noexcept {
        if (count_ == capacity_)
            return false;
        entries_[count_++] = {Argument(id), false};
        return true;
    }
    void sort() noexcept {
        std::sort(entries_.get(), entries_.get() + count_, [](const Entry& a, const Entry& b) {
            return std::string_view(a.id) < std::string_view(b.id);
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
    posix::Fd directory_;
    std::unique_ptr<Entry[]> entries_;
    bounded::Storage names_;
    std::size_t capacity_ = 0, count_ = 0, used_ = 0;
};

}  // namespace kagami::mount
