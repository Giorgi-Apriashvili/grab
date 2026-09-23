#include "json.hpp"

#include <charconv>
#include <format>

namespace grab::json {

const Value* Value::find(std::string_view key) const {
    if (kind != Kind::object) return nullptr;
    for (std::size_t i = 0; i < keys.size(); ++i) {
        if (keys[i] == key) return &items[i];
    }
    return nullptr;
}

std::optional<std::string> Value::string_of(std::string_view key) const {
    const Value* v = find(key);
    if (v == nullptr || v->kind != Kind::string) return std::nullopt;
    return v->str;
}

namespace {

constexpr int max_depth = 128;

void append_utf8(std::string& out, char32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

class Parser {
public:
    explicit Parser(std::string_view text) : t_(text) {}

    std::expected<Value, std::string> document() {
        auto v = value(0);
        if (!v) return v;
        skip_ws();
        if (p_ != t_.size()) return fail("unexpected trailing characters");
        return v;
    }

private:
    std::string_view t_;
    std::size_t p_ = 0;

    [[nodiscard]] std::unexpected<std::string> fail(std::string_view what) const {
        return std::unexpected(std::format("invalid JSON: {} at offset {}", what, p_));
    }

    void skip_ws() {
        while (p_ < t_.size() &&
               (t_[p_] == ' ' || t_[p_] == '\t' || t_[p_] == '\n' || t_[p_] == '\r')) {
            ++p_;
        }
    }

    bool literal(std::string_view word) {
        if (t_.substr(p_, word.size()) != word) return false;
        p_ += word.size();
        return true;
    }

    std::expected<Value, std::string> value(int depth) {
        if (depth > max_depth) return fail("nesting too deep");
        skip_ws();
        if (p_ >= t_.size()) return fail("unexpected end of input");

        Value v;
        const char c = t_[p_];
        if (c == '{') return object(depth);
        if (c == '[') return array(depth);
        if (c == '"') {
            auto s = string();
            if (!s) return std::unexpected(s.error());
            v.kind = Value::Kind::string;
            v.str = std::move(*s);
            return v;
        }
        if (literal("true") || literal("false")) {
            v.kind = Value::Kind::boolean;
            v.boolean = t_[p_ - 1] == 'e' && t_.substr(p_ - 4, 4) == "true";
            return v;
        }
        if (literal("null")) return v;
        if (c == '-' || (c >= '0' && c <= '9')) return number();
        return fail("unexpected character");
    }

    std::expected<Value, std::string> number() {
        const std::size_t start = p_;
        while (p_ < t_.size()) {
            const char c = t_[p_];
            if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E') {
                ++p_;
            } else {
                break;
            }
        }
        Value v;
        v.kind = Value::Kind::number;
        const auto [ptr, ec] = std::from_chars(t_.data() + start, t_.data() + p_, v.number);
        if (ec != std::errc{} || ptr != t_.data() + p_) {
            p_ = start;
            return fail("malformed number");
        }
        return v;
    }

    std::expected<unsigned, std::string> hex4() {
        if (p_ + 4 > t_.size()) return fail("truncated \\u escape");
        unsigned value = 0;
        const auto [ptr, ec] = std::from_chars(t_.data() + p_, t_.data() + p_ + 4, value, 16);
        if (ec != std::errc{} || ptr != t_.data() + p_ + 4) return fail("bad \\u escape");
        p_ += 4;
        return value;
    }

    std::expected<std::string, std::string> string() {
        ++p_; // opening quote
        std::string out;
        while (p_ < t_.size()) {
            const char c = t_[p_++];
            if (c == '"') return out;
            if (static_cast<unsigned char>(c) < 0x20) return fail("control character in string");
            if (c != '\\') {
                out += c;
                continue;
            }
            if (p_ >= t_.size()) break;
            const char e = t_[p_++];
            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                auto hi = hex4();
                if (!hi) return std::unexpected(hi.error());
                char32_t cp = *hi;
                if (cp >= 0xD800 && cp <= 0xDBFF && t_.substr(p_, 2) == "\\u") {
                    p_ += 2;
                    auto lo = hex4();
                    if (!lo) return std::unexpected(lo.error());
                    if (*lo >= 0xDC00 && *lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (*lo - 0xDC00);
                    } else {
                        append_utf8(out, 0xFFFD);
                        cp = *lo;
                    }
                } else if (cp >= 0xD800 && cp <= 0xDFFF) {
                    cp = 0xFFFD; // lone surrogate
                }
                append_utf8(out, cp);
                break;
            }
            default: return fail("unknown escape");
            }
        }
        return fail("unterminated string");
    }

    std::expected<Value, std::string> array(int depth) {
        ++p_; // [
        Value v;
        v.kind = Value::Kind::array;
        skip_ws();
        if (p_ < t_.size() && t_[p_] == ']') {
            ++p_;
            return v;
        }
        for (;;) {
            auto item = value(depth + 1);
            if (!item) return item;
            v.items.push_back(std::move(*item));
            skip_ws();
            if (p_ >= t_.size()) return fail("unterminated array");
            if (t_[p_] == ',') {
                ++p_;
                continue;
            }
            if (t_[p_] == ']') {
                ++p_;
                return v;
            }
            return fail("expected ',' or ']'");
        }
    }

    std::expected<Value, std::string> object(int depth) {
        ++p_; // {
        Value v;
        v.kind = Value::Kind::object;
        skip_ws();
        if (p_ < t_.size() && t_[p_] == '}') {
            ++p_;
            return v;
        }
        for (;;) {
            skip_ws();
            if (p_ >= t_.size() || t_[p_] != '"') return fail("expected a member name");
            auto key = string();
            if (!key) return std::unexpected(key.error());
            skip_ws();
            if (p_ >= t_.size() || t_[p_] != ':') return fail("expected ':'");
            ++p_;
            auto member = value(depth + 1);
            if (!member) return member;
            v.keys.push_back(std::move(*key));
            v.items.push_back(std::move(*member));
            skip_ws();
            if (p_ >= t_.size()) return fail("unterminated object");
            if (t_[p_] == ',') {
                ++p_;
                continue;
            }
            if (t_[p_] == '}') {
                ++p_;
                return v;
            }
            return fail("expected ',' or '}'");
        }
    }
};

} // namespace

std::expected<Value, std::string> parse(std::string_view text) {
    if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
    return Parser(text).document();
}

} // namespace grab::json
