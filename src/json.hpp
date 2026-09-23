#pragma once

#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace grab::json {

// Minimal JSON document model, enough to read a GitHub release payload.
struct Value {
    enum class Kind { null, boolean, number, string, array, object };

    Kind kind = Kind::null;
    bool boolean = false;
    double number = 0;
    std::string str;
    std::vector<Value> items;      // array elements, or object member values
    std::vector<std::string> keys; // object member names, parallel to `items`

    // Object member by name (first match), or nullptr for a missing key or a non-object.
    [[nodiscard]] const Value* find(std::string_view key) const;
    // Object member that is a string.
    [[nodiscard]] std::optional<std::string> string_of(std::string_view key) const;
};

[[nodiscard]] std::expected<Value, std::string> parse(std::string_view text);

} // namespace grab::json
