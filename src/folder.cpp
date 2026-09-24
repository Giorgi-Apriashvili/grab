#include "folder.hpp"

#include "json.hpp"
#include "util.hpp"

#include <algorithm>

namespace grab::folder {

std::expected<std::vector<FolderFile>, std::string> parse_listing(std::string_view json_text) {
    auto doc = json::parse(json_text);
    if (!doc || doc->kind != json::Value::Kind::array) return util::fail("unexpected folder listing from rclone");
    std::vector<FolderFile> files;
    for (const auto& v : doc->items) {
        const json::Value* dir = v.find("IsDir");
        if (dir != nullptr && dir->kind == json::Value::Kind::boolean && dir->boolean) continue;
        auto path = v.string_of("Path");
        if (!path || path->empty()) continue;
        FolderFile f;
        f.path = *path;
        if (const json::Value* size = v.find("Size"); size && size->kind == json::Value::Kind::number && size->number > 0) {
            f.size = static_cast<std::uint64_t>(size->number);
        }
        f.modtime = v.string_of("ModTime").value_or("");
        files.push_back(std::move(f));
    }
    std::ranges::sort(files, {}, &FolderFile::path);
    return files;
}

std::string files_from_text(const std::vector<std::string>& paths) {
    std::string out;
    for (const auto& p : paths) {
        out += p;
        out += '\n';
    }
    return out;
}

std::optional<std::string> parse_copied_line(std::string_view line) {
    if (line.find("\"Copied") == std::string_view::npos) return std::nullopt; // cheap pre-check
    auto doc = json::parse(line);
    if (!doc || doc->kind != json::Value::Kind::object) return std::nullopt;
    if (!doc->string_of("msg").value_or("").starts_with("Copied")) return std::nullopt;
    return doc->string_of("object");
}

std::vector<std::size_t> visible(const std::vector<State>& states, std::size_t queued_limit) {
    std::vector<std::size_t> out;
    std::size_t queued = 0;
    for (std::size_t i = 0; i < states.size(); ++i) {
        switch (states[i]) {
        case State::running:
        case State::paused:
        case State::failed:
            out.push_back(i);
            break;
        case State::queued:
            if (queued++ < queued_limit) out.push_back(i);
            break;
        case State::done:
        case State::skipped:
            break;
        }
    }
    return out;
}

} // namespace grab::folder
