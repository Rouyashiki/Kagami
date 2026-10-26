#pragma once

#include "mount/backend.hpp"

#include <algorithm>
#include <string_view>
#include "kagami/fixed_array.hpp"

namespace kagami::mount::kasumi::detail {

[[nodiscard]] inline bool matches_path(std::string_view path, std::string_view prefix) {
    if (path == prefix)
        return true;
    return !prefix.empty() && path.size() > prefix.size() &&
           path.compare(0, prefix.size(), prefix) == 0 &&
           (prefix.back() == '/' || path[prefix.size()] == '/');
}

// One immutable module rule vector outlives this index and its returned views.
// Only rule ordinals are copied; filesystem state is never retained here.
class RuleIndex {
public:
    [[nodiscard]] bool prepare_index(ModuleRuleSpan rules) noexcept {
        rules_ = rules;
        if (rules.size() <= linear_limit)
            return true;
        if (!storage_.prepare_array(rules.size(), json::Document::maximum_nodes))
            return false;
        for (std::size_t i = 0; i < rules.size(); ++i) {
            if (!rules[i].path.empty() && rules[i].path.front() == '/' && !storage_.append(i))
                return false;
        }
        auto* first = storage_.data();
        auto* last = first + storage_.size();
        std::sort(first, last, [&](std::size_t a, std::size_t b) noexcept {
            const std::string_view lhs = rules_[a].path, rhs = rules_[b].path;
            return lhs != rhs ? lhs < rhs : a > b;
        });
        const auto* unique = std::unique(first, last, [&](std::size_t a, std::size_t b) noexcept {
            return rules_[a].path == std::string_view(rules_[b].path);
        });
        ordered_ = {first, static_cast<std::size_t>(unique - first)};
        return true;
    }

    [[nodiscard]] const ModuleRule* effective_rule(std::string_view path) const {
        if (rules_.size() <= linear_limit) {
            const ModuleRule* selected = nullptr;
            std::size_t longest = 0;
            for (const auto& rule : rules_) {
                if (!rule.path.empty() && rule.path.front() == '/' && rule.path.size() >= longest &&
                    matches_path(path, rule.path)) {
                    longest = rule.path.size();
                    selected = &rule;
                }
            }
            return selected;
        }
        auto prefix = path;
        while (!prefix.empty()) {
            if (const auto* rule = exact(prefix))
                return rule;
            // A trailing slash is itself a valid rule prefix. Check it before
            // the shorter spelling, without normalizing repeated slashes.
            if (prefix.back() == '/') {
                prefix.remove_suffix(1);
            } else {
                const auto slash = prefix.rfind('/');
                if (slash == std::string_view::npos)
                    break;
                prefix = prefix.substr(0, slash + 1);
            }
        }
        return nullptr;
    }

    [[nodiscard]] std::string_view effective_mode(std::string_view path) const {
        const auto* rule = effective_rule(path);
        return rule ? std::string_view(rule->mode) : "kasumi";
    }

    [[nodiscard]] bool effective_bypass(std::string_view path) const {
        const auto* rule = effective_rule(path);
        return rule && rule->bypass;
    }

    [[nodiscard]] bool has_nested_rule(std::string_view path) const {
        if (rules_.size() <= linear_limit) {
            return std::any_of(rules_.begin(), rules_.end(), [&](const ModuleRule& rule) {
                return rule.path.size() > path.size() && matches_path(rule.path, path);
            });
        }
        // lower_bound(path + '/') without allocating a concatenated key. If
        // path already ends in '/', skip only its exact equal rule.
        const auto it = std::lower_bound(
            ordered_.begin(), ordered_.end(), path, [&](std::size_t i, std::string_view key) {
                const std::string_view candidate = rules_[i].path;
                const int order = candidate.compare(0, key.size(), key);
                if (order != 0)
                    return order < 0;
                if (candidate.size() == key.size())
                    return true;
                return !key.empty() && key.back() != '/' && candidate[key.size()] < '/';
            });
        return it != ordered_.end() && rules_[*it].path.size() > path.size() &&
               matches_path(rules_[*it].path, path);
    }

private:
    ModuleRuleSpan rules_;
    bounded::Array<std::size_t> storage_;
    bounded::Span<std::size_t> ordered_;
    static constexpr std::size_t linear_limit = 8;

    [[nodiscard]] const ModuleRule* exact(std::string_view path) const {
        const auto it = std::lower_bound(ordered_.begin(), ordered_.end(), path,
                                         [&](std::size_t i, std::string_view key) {
                                             return std::string_view(rules_[i].path) < key;
                                         });
        return it != ordered_.end() && rules_[*it].path == path ? &rules_[*it] : nullptr;
    }
};

}  // namespace kagami::mount::kasumi::detail
