#pragma once

#include "core/arguments.hpp"
#include "core/flat_json.hpp"
#include "core/prepared_input.hpp"
#include "kagami/fixed_array.hpp"

namespace kagami::mount {

// Views borrow the owning table's final input buffer for the complete operation.
struct ModuleRule {
    Argument path;
    Argument mode;
};
using ModuleRuleSpan = bounded::Span<ModuleRule>;

class ModuleModes {
public:
    [[nodiscard]] bool prepare_modes(const char* path, int& error) noexcept;
    std::string_view find(std::string_view id, std::string_view fallback = "auto") const noexcept;
    [[nodiscard]] bool write_update(bounded::Writer& output, std::string_view id,
                                    std::string_view mode) const noexcept;

private:
    struct Entry {
        Argument id;
        Argument mode;
    };
    PreparedInput input_;
    bounded::Array<Entry> entries_;
};

class ModuleRules {
public:
    [[nodiscard]] bool prepare_rules(const char* path, int& error) noexcept;
    ModuleRuleSpan find(std::string_view id) const noexcept;
    [[nodiscard]] bool write_update(bounded::Writer& output, std::string_view id,
                                    std::string_view path, std::string_view mode,
                                    bool remove) const noexcept;

private:
    struct Entry {
        Argument id;
        std::size_t offset;
        std::size_t count;
    };
    ModuleRuleSpan rules(const Entry& entry) const noexcept {
        return {rules_.view().data() + entry.offset, entry.count};
    }
    PreparedInput input_;
    bounded::Array<Entry> entries_;
    bounded::Array<ModuleRule> rules_;
};

}  // namespace kagami::mount
