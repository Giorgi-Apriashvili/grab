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

    // Builders, for producing messages.
    [[nodiscard]] static Value make_string(std::string s);
    [[nodiscard]] static Value make_number(double n);
    [[nodiscard]] static Value make_bool(bool b);
    [[nodiscard]] static Value make_array();
    [[nodiscard]] static Value make_object();
    // Appends to an array.
    Value& push(Value v);
    // Sets an object member, replacing an existing one with the same key.
    Value& set(std::string key, Value v);
};

[[nodiscard]] std::expected<Value, std::string> parse(std::string_view text);

// Compact JSON text. Integral numbers print without a fraction; NaN and infinities as null.
[[nodiscard]] std::string stringify(const Value& v);

// A JSON string literal, quotes included: escapes ", \ and control characters; UTF-8 passes
// through unchanged.
[[nodiscard]] std::string quote(std::string_view s);

} // namespace grab::json
