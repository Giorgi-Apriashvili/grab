#include "queue_view.hpp"

#include "local_name.hpp"
#include "util.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <functional>
#include <iterator>
#include <memory>
#include <string_view>
#include <unordered_set>

namespace grab::queue_view {

namespace fs = std::filesystem;
using folder::State;

namespace {

// A failed file's error as a number, never 0 (0 is "none").
std::uint32_t error_id(std::string_view error) {
    return static_cast<std::uint32_t>(std::hash<std::string_view>{}(error)) | 1U;
}

int state_code(State s) { return static_cast<int>(s); }

} // namespace

Names names_of(const std::vector<folder::File>& files) {
    Names n;
    n.path.reserve(files.size());
    n.size.reserve(files.size());
    for (std::size_t i = 0; i < files.size(); ++i) {
        const auto& f = files[i];
        n.path.push_back(f.path);
        n.size.push_back(f.size);
        if (!f.local.empty()) n.local.emplace(i, f.local);
        auto shown = util::path_to_utf8(f.local.empty() ? local::relative(f.path) : f.local);
        std::string as_path = f.path;
        std::ranges::replace(as_path, '/', '\\');
        if (shown != as_path) n.shown.emplace(i, std::move(shown));
    }
    return n;
}

fetch::Reserved reserved_names(const fs::path& dir, const Names& names) {
    auto keys = std::make_shared<std::unordered_set<fs::path::string_type>>();
    keys->reserve(names.path.size() + names.local.size());
    for (const auto& p : names.path) keys->insert(local::key(dir / local::relative(p)));
    for (const auto& [i, l] : names.local) keys->insert(local::key(dir / l));
    return [keys](const fs::path& p) { return keys->contains(local::key(p)); };
}

fs::path target_of(const fs::path& dir, const Names& names, std::size_t i) {
    if (const auto it = names.local.find(i); it != names.local.end()) return dir / it->second;
    return dir / local::relative(names.path.at(i));
}

Pass pass(const std::vector<folder::File>& files, std::vector<Shadow>* shadow) {
    Pass p;
    if (shadow != nullptr && shadow->size() != files.size()) shadow = nullptr; // another generation's
    auto& t = p.totals;
    for (std::size_t i = 0; i < files.size(); ++i) {
        const auto& f = files[i];
        ++p.counts[static_cast<std::size_t>(f.status)];
        if (f.status != State::skipped) { // as folder::totals counts
            ++t.files;
            t.total += f.size;
            if (f.status == State::done) {
                ++t.files_done;
                t.bytes += f.size;
            } else {
                t.bytes += std::min(f.bytes, f.size);
            }
            if (f.status == State::running) t.speed += f.speed;
        }
        if (shadow == nullptr) continue;
        auto& s = (*shadow)[i];
        const bool running = f.status == State::running;
        Shadow now{.bytes = f.bytes,
                   .speed = running ? static_cast<float>(f.speed) : 0.0F,
                   .error = 0,
                   .status = f.status,
                   .weight = 2,
                   .verified = f.verified};
        // Hashed on every pass: a file queued again can fail with another error between two passes.
        if (f.status == State::failed) now.error = error_id(f.error);
        if (now.bytes == s.bytes && now.speed == s.speed && now.error == s.error && now.status == s.status &&
            now.weight == s.weight && now.verified == s.verified) {
            continue;
        }
        s = now;
        p.changed.push_back(Row{.index = i,
                                .now = now,
                                .eta = running ? f.eta : std::nullopt,
                                .error = f.status == State::failed ? f.error : std::string()});
    }
    if (t.speed > 0 && t.total > t.bytes) t.eta = static_cast<double>(t.total - t.bytes) / t.speed;
    return p;
}

Snapshot snapshot(const std::vector<folder::File>& files) {
    Snapshot snap;
    snap.rows.reserve(files.size());
    for (std::size_t i = 0; i < files.size(); ++i) {
        const auto& f = files[i];
        Shadow s{.bytes = f.bytes, .speed = 0, .error = 0, .status = f.status, .weight = 2, .verified = f.verified};
        if (f.status == State::failed) {
            s.error = error_id(f.error);
            snap.errors.emplace(i, f.error);
        }
        snap.rows.push_back(s);
    }
    return snap;
}

std::string files_message(int id, int gen, std::size_t from, std::size_t count, const Names& names,
                          const Snapshot& snap) {
    const std::size_t total = std::min(names.path.size(), snap.rows.size());
    from = std::min(from, total);
    const std::size_t end = std::min(total, from + count);
    std::string out = std::format(R"({{"type":"files","id":{},"gen":{},"from":{},"total":{},"rows":[)", id, gen, from, total);
    out.reserve(out.size() + (end - from) * 64);
    for (std::size_t i = from; i < end; ++i) {
        const auto& s = snap.rows[i];
        if (i > from) out += ',';
        out += '[';
        out += json::quote(names.path[i]);
        std::format_to(std::back_inserter(out), ",{},{},{},{}", names.size[i], state_code(s.status), s.bytes, s.weight);
        // The optional cells, as far as the last one set.
        const auto shown = names.shown.find(i);
        const auto error = snap.errors.find(i);
        const bool named = names.local.contains(i);
        const int last = named ? 4 : s.verified ? 3 : error != snap.errors.end() ? 2 : shown != names.shown.end() ? 1 : 0;
        if (last >= 1) out += ',' + (shown != names.shown.end() ? json::quote(shown->second) : std::string("null"));
        if (last >= 2) out += ',' + (error != snap.errors.end() ? json::quote(error->second) : std::string("null"));
        if (last >= 3) out += s.verified ? ",1" : ",0";
        if (last >= 4) out += ",1";
        out += ']';
    }
    out += "]}";
    return out;
}

std::string file_delta_message(int id, int gen, const std::vector<Row>& rows) {
    std::string out = std::format(R"({{"type":"fileDelta","id":{},"gen":{},"rows":[)", id, gen);
    out.reserve(out.size() + rows.size() * 48);
    bool first = true;
    for (const auto& r : rows) {
        if (!first) out += ',';
        first = false;
        const auto& s = r.now;
        std::format_to(std::back_inserter(out), "[{},{},{},{:.0f},", r.index, state_code(s.status), s.bytes, s.speed);
        if (r.eta && std::isfinite(*r.eta)) std::format_to(std::back_inserter(out), "{:.0f}", *r.eta);
        else out += "null";
        std::format_to(std::back_inserter(out), ",{}", s.weight);
        if (s.status == State::failed || s.verified) out += ',' + (s.status == State::failed ? json::quote(r.error) : std::string("null"));
        if (s.verified) out += ",1";
        out += ']';
    }
    out += "]}";
    return out;
}

std::vector<std::size_t> indices_of(const json::Value* ranges, std::size_t total) {
    std::vector<std::size_t> out;
    if (ranges == nullptr || ranges->kind != json::Value::Kind::array || total == 0) return out;
    std::vector<char> marked(total, 0);
    const auto top = static_cast<double>(total - 1);
    for (const auto& r : ranges->items) {
        if (r.kind != json::Value::Kind::array || r.items.size() != 2) continue;
        const auto& a = r.items[0];
        const auto& b = r.items[1];
        if (a.kind != json::Value::Kind::number || b.kind != json::Value::Kind::number || !std::isfinite(a.number) ||
            !std::isfinite(b.number) || a.number > b.number || b.number < 0 || a.number > top) {
            continue;
        }
        const auto lo = static_cast<std::size_t>(std::max(0.0, std::ceil(a.number)));
        const auto hi = static_cast<std::size_t>(std::min(top, std::floor(b.number)));
        for (std::size_t i = lo; i <= hi; ++i) marked[i] = 1;
    }
    for (std::size_t i = 0; i < total; ++i) {
        if (marked[i] != 0) out.push_back(i);
    }
    return out;
}

std::vector<std::size_t> changed_items(std::map<int, std::string>& sent,
                                       const std::vector<std::pair<int, std::string>>& items) {
    std::vector<std::size_t> changed;
    std::map<int, std::string> now;
    for (std::size_t k = 0; k < items.size(); ++k) {
        const auto& [id, text] = items[k];
        const auto it = sent.find(id);
        if (it == sent.end() || it->second != text) changed.push_back(k);
        now.emplace(id, text);
    }
    sent = std::move(now);
    return changed;
}

} // namespace grab::queue_view
