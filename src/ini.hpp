#pragma once

#include <expected>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace grab::ini {

// Minimal INI reader matching rclone.conf: [section], key = value, whole-line ; or # comments.
// A ; or # after a value is part of the value (rclone does the same). Later duplicate keys win;
// duplicate sections are merged. Keys before the first header land in a section named "".
struct Section {
    std::string name;
    std::map<std::string, std::string, std::less<>> values;

    [[nodiscard]] std::optional<std::string> get(std::string_view key) const;
};

struct Document {
    std::vector<Section> sections; // in file order

    [[nodiscard]] const Section* find(std::string_view name) const;
    [[nodiscard]] Section& section_or_create(std::string_view name);
    [[nodiscard]] std::vector<std::string> section_names() const;
};

[[nodiscard]] std::expected<Document, std::string> parse(std::string_view text);
[[nodiscard]] std::expected<Document, std::string> parse_file(const std::filesystem::path& p);

} // namespace grab::ini
