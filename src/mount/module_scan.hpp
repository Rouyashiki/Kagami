#pragma once

#include "core/arguments.hpp"
#include "core/bounded.hpp"
#include "core/directory.hpp"
#include "kagami/config.hpp"

#include <array>
#include <string_view>

namespace kagami::mount {

struct ModuleFacts {
    bool has_prop = false;
    bool inactive = false;
    bool visible_content = false;
    bool backend_content = false;
    bool needs_magic = false;
};

struct ModuleProperties {
    std::string_view name, version, author, description;
    bool has_name = false;
};

// One request's scanner; no facts survive into the next request. Config is
// borrowed until the scanner is destroyed. Each observe call invalidates the
// previous property views, but never reallocates its explicit working storage.
class ModuleScanner {
public:
    ModuleScanner() noexcept = default;
    ModuleScanner(const ModuleScanner&) = delete;
    ModuleScanner& operator=(const ModuleScanner&) = delete;
    ModuleScanner(ModuleScanner&&) = delete;
    ModuleScanner& operator=(ModuleScanner&&) = delete;
    ~ModuleScanner() = default;

    [[nodiscard]] bool prepare_scan(const Config& config, bool with_properties = true) noexcept {
        config_ = &config;
        return storage_.prepare_storage(with_properties ? storage_bytes : directory_bytes * 3,
                                        storage_bytes);
    }
    [[nodiscard]] bool observe(const char* path, ModuleFacts& facts, ModuleProperties& props,
                               int& error, bool inspect_root_files = true,
                               bool load_properties = true) noexcept;
    // Valid until the next observe call; read only after filtering the facts.
    [[nodiscard]] bool observed_properties(ModuleProperties& props, int& error) noexcept {
        return observed_.get() < 0 || read_properties(observed_.get(), props, error);
    }

private:
    static constexpr std::size_t directory_bytes = 8192;
    static constexpr std::size_t property_bytes = 1024UL * 1024;
    static constexpr std::size_t storage_bytes = directory_bytes * 3 + property_bytes + 1;
    static constexpr std::array<Argument, 6> builtin = {"system",     "vendor", "product",
                                                        "system_ext", "odm",    "oem"};
    bounded::Storage storage_;
    posix::Fd observed_;
    const Config* config_ = nullptr;
    bool inspect_root_files_ = true;
    bool partition(std::string_view name, bool visible) const noexcept;
    bool direct_files(int fd, std::size_t buffer_index, bool scan_subpartitions) noexcept;
    bool read_properties(int fd, ModuleProperties& props, int& error) noexcept;
    void probe_unlisted(int fd, ModuleFacts& facts) noexcept;
};

}  // namespace kagami::mount
