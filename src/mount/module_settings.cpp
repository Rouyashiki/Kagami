#include "mount/module_settings.hpp"

namespace kagami::mount {
namespace {
bool prepare_settings(const char* path, PreparedInput& input, json::Document& document,
                      int& error) noexcept {
    if (!input.prepare_file(path, error)) {
        if (error != ENOENT)
            return false;
        error = 0;
        return true;
    }
    if (!document.prepare_document(input.view(), error) ||
        !document.parse(input.data(), input.size(), error))
        return false;
    if (!document.root().is_object()) {
        error = EINVAL;
        return false;
    }
    return true;
}
bool valid_rule(json::Value value) noexcept {
    return value.is_object() && value.find("path").is_string() && value.find("mode").is_string();
}
bool quote(bounded::Writer& output, std::string_view value) noexcept {
    // Preserve the established control-plane escaping, including raw controls.
    return output.append('"') && bounded::escape(output, value) && output.append('"');
}
bool write_rule(bounded::Writer& output, std::string_view path, std::string_view mode) noexcept {
    return output.append("{\"path\":") && quote(output, path) && output.append(",\"mode\":") &&
           quote(output, mode) && output.append('}');
}
}  // namespace

bool ModuleModes::prepare_modes(const char* path, int& error) noexcept {
    json::Document document;
    if (!prepare_settings(path, input_, document, error))
        return false;
    const auto root = document.root();
    std::size_t count = 0;
    for (std::size_t i = 0; i < root.size(); ++i)
        count += root.child(i).is_string();
    if (!entries_.prepare_array(count, json::Document::maximum_nodes)) {
        error = ENOMEM;
        return false;
    }
    for (std::size_t i = 0; i < root.size(); ++i) {
        const auto value = root.child(i);
        if (value.is_string() &&
            !entries_.append({Argument(value.key()), Argument(value.string_or())})) {
            error = ENOBUFS;
            return false;
        }
    }
    return true;
}

std::string_view ModuleModes::find(std::string_view id, std::string_view fallback) const noexcept {
    const auto entries = entries_.view();
    if (entries.empty())
        return fallback;
    const auto* found = std::lower_bound(entries.begin(), entries.end(), id,
                                         [](const Entry& entry, std::string_view key) noexcept {
                                             return std::string_view(entry.id) < key;
                                         });
    return found != entries.end() && found->id == id ? std::string_view(found->mode) : fallback;
}

bool ModuleModes::write_update(bounded::Writer& output, std::string_view id,
                               std::string_view mode) const noexcept {
    if (!output.append("{\n"))
        return false;
    bool first = true;
    const auto emit = [&](std::string_view key, std::string_view value) noexcept {
        if ((!first && !output.append(",\n")) || !output.append("  ") || !quote(output, key) ||
            !output.append(": ") || !quote(output, value))
            return false;
        first = false;
        return true;
    };
    bool inserted = mode == "auto";
    for (const auto& entry : entries_.view()) {
        if (!inserted && std::string_view(entry.id) >= id) {
            if (!emit(id, mode))
                return false;
            inserted = true;
        }
        if (entry.id != id && !emit(entry.id, entry.mode))
            return false;
    }
    if (!inserted && !emit(id, mode))
        return false;
    return (first || output.append('\n')) && output.append("}\n");
}

bool ModuleRules::prepare_rules(const char* path, int& error) noexcept {
    json::Document document;
    if (!prepare_settings(path, input_, document, error))
        return false;
    const auto root = document.root();
    std::size_t modules = 0;
    std::size_t rules = 0;
    for (std::size_t i = 0; i < root.size(); ++i) {
        const auto values = root.child(i);
        const auto previous = rules;
        if (values.is_array())
            for (std::size_t j = 0; j < values.size(); ++j)
                rules += valid_rule(values.child(j));
        modules += rules != previous;
    }
    if (!entries_.prepare_array(modules, json::Document::maximum_nodes) ||
        !rules_.prepare_array(rules, json::Document::maximum_nodes)) {
        error = ENOMEM;
        return false;
    }
    for (std::size_t i = 0; i < root.size(); ++i) {
        const auto values = root.child(i);
        if (!values.is_array())
            continue;
        const auto first = rules_.size();
        for (std::size_t j = 0; j < values.size(); ++j) {
            const auto value = values.child(j);
            if (valid_rule(value) && !rules_.append({Argument(value.find("path").string_or()),
                                                     Argument(value.find("mode").string_or())})) {
                error = ENOBUFS;
                return false;
            }
        }
        if (rules_.size() != first &&
            !entries_.append({Argument(values.key()), first, rules_.size() - first})) {
            error = ENOBUFS;
            return false;
        }
    }
    return true;
}

ModuleRuleSpan ModuleRules::find(std::string_view id) const noexcept {
    const auto entries = entries_.view();
    if (entries.empty())
        return {};
    const auto* found = std::lower_bound(entries.begin(), entries.end(), id,
                                         [](const Entry& entry, std::string_view key) noexcept {
                                             return std::string_view(entry.id) < key;
                                         });
    return found != entries.end() && found->id == id ? rules(*found) : ModuleRuleSpan{};
}

bool ModuleRules::write_update(bounded::Writer& output, std::string_view id, std::string_view path,
                               std::string_view mode, bool remove) const noexcept {
    if (!output.append("{\n"))
        return false;
    bool first = true;
    const auto emit = [&](std::string_view key, ModuleRuleSpan values, bool update) noexcept {
        bool has_remaining = !remove || !update;
        if (!has_remaining)
            for (const auto& value : values)
                has_remaining = has_remaining || value.path != path;
        if (!has_remaining)
            return true;
        if ((!first && !output.append(",\n")) || !output.append("  ") || !quote(output, key) ||
            !output.append(": ["))
            return false;
        first = false;
        bool first_rule = true;
        bool replaced = false;
        for (const auto& value : values) {
            const bool matches = update && value.path == path;
            if (matches && remove)
                continue;
            if ((!first_rule && !output.append(',')) ||
                !write_rule(output, value.path,
                            matches && !replaced ? mode : std::string_view(value.mode)))
                return false;
            first_rule = false;
            replaced = replaced || matches;
        }
        if (update && !remove && !replaced &&
            ((!first_rule && !output.append(',')) || !write_rule(output, path, mode)))
            return false;
        return output.append(']');
    };
    bool inserted = false;
    for (const auto& entry : entries_.view()) {
        if (!inserted && std::string_view(entry.id) >= id) {
            if (!emit(id, find(id), true))
                return false;
            inserted = true;
        }
        if (entry.id != id && !emit(entry.id, rules(entry), false))
            return false;
    }
    if (!inserted && !emit(id, {}, true))
        return false;
    return (first || output.append('\n')) && output.append("}\n");
}

}  // namespace kagami::mount
