#include "ini.hpp"

#include "util.hpp"

namespace grab::ini {

std::optional<std::string> Section::get(std::string_view key) const {
    const auto it = values.find(key);
    if (it == values.end()) return std::nullopt;
    return it->second;
}

const Section* Document::find(std::string_view name) const {
    for (const auto& s : sections) {
        if (s.name == name) return &s;
    }
    return nullptr;
}

Section& Document::section_or_create(std::string_view name) {
    for (auto& s : sections) {
        if (s.name == name) return s;
    }
    sections.push_back(Section{std::string(name), {}});
    return sections.back();
}

std::vector<std::string> Document::section_names() const {
    std::vector<std::string> out;
    out.reserve(sections.size());
    for (const auto& s : sections) out.push_back(s.name);
    return out;
}

std::expected<Document, std::string> parse(std::string_view text) {
    Document doc;
    Section* current = nullptr;
    int lineno = 0;
    std::size_t pos = 0;

    while (pos <= text.size()) {
        const auto nl = text.find('\n', pos);
        const std::string_view raw =
            text.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
        pos = (nl == std::string_view::npos) ? text.size() + 1 : nl + 1;
        ++lineno;

        const auto line = util::trim(raw);
        if (line.empty() || line.front() == ';' || line.front() == '#') continue;

        if (line.front() == '[') {
            if (line.back() != ']') {
                return util::failf("line {}: unterminated section header '{}'", lineno, line);
            }
            const auto name = util::trim(line.substr(1, line.size() - 2));
            if (name.empty()) return util::failf("line {}: empty section name", lineno);
            current = &doc.section_or_create(name);
            continue;
        }

        const auto eq = line.find('=');
        if (eq == std::string_view::npos) {
            return util::failf("line {}: expected 'key = value', got '{}'", lineno, line);
        }
        const auto key = util::trim(line.substr(0, eq));
        const auto value = util::trim(line.substr(eq + 1));
        if (key.empty()) return util::failf("line {}: empty key", lineno);

        if (current == nullptr) current = &doc.section_or_create("");
        current->values[std::string(key)] = std::string(value);
    }
    return doc;
}

std::expected<Document, std::string> parse_file(const std::filesystem::path& p) {
    auto text = util::read_file(p);
    if (!text) return util::fail(text.error());
    auto doc = parse(*text);
    if (!doc) return util::failf("{}: {}", util::path_to_utf8(p), doc.error());
    return doc;
}

} // namespace grab::ini
