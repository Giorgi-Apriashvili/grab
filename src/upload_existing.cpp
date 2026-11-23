#include "upload_existing.hpp"

namespace grab::upload {

std::string_view name(Existing e) {
    switch (e) {
    case Existing::replace: return "replace";
    case Existing::different: return "different";
    case Existing::newer: return "newer";
    case Existing::skip: return "skip";
    case Existing::keep_both: return "both";
    case Existing::ask: break;
    }
    return "ask";
}

std::optional<Existing> parse_existing(std::string_view text) {
    for (const auto e : {Existing::ask, Existing::replace, Existing::different, Existing::newer, Existing::skip,
                         Existing::keep_both}) {
        if (text == name(e)) return e;
    }
    return std::nullopt;
}

Kind kind(const Clash& clash) {
    if (clash.remote_mtime) {
        if (clash.local_mtime > *clash.remote_mtime + same_time_slack) return Kind::newer_here;
        if (clash.local_mtime < *clash.remote_mtime - same_time_slack) return Kind::older_here;
    }
    return clash.local_size == clash.remote_size ? Kind::same : Kind::different_size;
}

std::string_view kind_name(const Clash& clash) {
    if (clash.remote_is_dir) return "folder";
    switch (kind(clash)) {
    case Kind::same: return "same";
    case Kind::newer_here: return "newer";
    case Kind::older_here: return "older";
    case Kind::different_size: break;
    }
    return "differs";
}

void Taken::add(const Clash& clash) {
    ++count;
    if (clash.remote_is_dir) {
        ++folders;
        return;
    }
    switch (kind(clash)) {
    case Kind::same: ++same; break;
    case Kind::newer_here: ++newer; break;
    case Kind::older_here: ++older; break;
    case Kind::different_size: ++differs; break;
    }
}

Decision decide(Existing choice, const Clash& clash) {
    const auto replace = [&]() -> Decision {
        if (clash.remote_is_dir) return {.action = Action::skip, .note = "a folder of that name is there"};
        return {.action = Action::replace, .note = {}};
    };
    switch (choice) {
    case Existing::replace: return replace();
    case Existing::different:
        if (clash.remote_is_dir || kind(clash) != Kind::same) return replace();
        return {.action = Action::skip, .note = {}};
    case Existing::newer:
        if (clash.remote_is_dir || kind(clash) == Kind::newer_here) return replace();
        return {.action = Action::skip, .note = {}};
    case Existing::skip: return {.action = Action::skip, .note = {}};
    case Existing::keep_both: return {.action = Action::numbered, .note = {}};
    case Existing::ask: break;
    }
    return {.action = Action::ask, .note = {}};
}

} // namespace grab::upload
