#include "conn_budget.hpp"

#include "util.hpp"

#include <algorithm>
#include <string>

namespace grab::budget {

bool is_storage_box(std::string_view host) {
    return util::to_lower(util::trim(host)).ends_with(".your-storagebox.de");
}

int default_budget(std::string_view host) { return is_storage_box(host) ? 8 : 12; }

int effective_budget(std::optional<int> configured, int default_value, std::optional<int> learned) {
    if (configured && *configured >= 1) return *configured;
    int b = default_value;
    if (learned && *learned >= 1) b = std::min(b, *learned);
    return std::max(b, 1);
}

Failure classify_failure(std::string_view stderr_text) {
    const std::string t = util::to_lower(stderr_text);
    // Messages from Go's ssh library and the OS when a server sheds connections: too many
    // (Storage Box), MaxStartups dropping a login, or a reset before the handshake finished.
    static constexpr std::string_view refused_signs[] = {
        "connection refused",   "connection reset by peer", "too many",
        "handshake failed",     "kex_exchange_identification",
        "ssh: disconnect",      "max_startups",             "maxstartups",
        "forcibly closed",      "wsarecv",
    };
    for (auto sign : refused_signs) {
        if (t.find(sign) != std::string::npos) return Failure::refused;
    }
    return Failure::other;
}

const ServerPool::User* ServerPool::find(int id) const {
    auto it = std::ranges::find(users_, id, &User::id);
    return it == users_.end() ? nullptr : &*it;
}

void ServerPool::add_user(int id, int weight) {
    if (find(id) == nullptr) users_.push_back(User{id, 0, 0, std::max(weight, 1)});
}

void ServerPool::set_weight(int id, int weight) {
    auto it = std::ranges::find(users_, id, &User::id);
    if (it != users_.end()) it->weight = std::max(weight, 1);
}

void ServerPool::remove_user(int id) { std::erase_if(users_, [&](const User& u) { return u.id == id; }); }

int ServerPool::share(int id) const {
    const auto it = std::ranges::find(users_, id, &User::id);
    if (it == users_.end()) return 0;
    long long total_weight = 0;
    for (const auto& u : users_) total_weight += u.weight;
    // Rounded-down weighted shares; what they leave of the budget goes one each to the
    // heaviest users, oldest first among equals.
    int given = 0;
    for (const auto& u : users_) given += static_cast<int>(static_cast<long long>(budget_) * u.weight / total_weight);
    const int remainder = budget_ - given;
    int ahead = 0; // users that get a remainder connection before this one
    const auto index = it - users_.begin();
    for (auto u = users_.begin(); u != users_.end(); ++u) {
        if (u->weight > it->weight || (u->weight == it->weight && u - users_.begin() < index)) ++ahead;
    }
    const int base = static_cast<int>(static_cast<long long>(budget_) * it->weight / total_weight);
    return std::max(1, base + (ahead < remainder ? 1 : 0));
}

int ServerPool::active(int id) const {
    const auto* u = find(id);
    return u == nullptr ? 0 : u->active;
}

int ServerPool::total_active() const {
    int total = 0;
    for (const auto& u : users_) total += u.active;
    return total;
}

bool ServerPool::other_starved(int id) const {
    return std::ranges::any_of(users_, [&](const User& u) {
        return u.id != id && u.waiting > 0 && u.active < share(u.id);
    });
}

bool ServerPool::may_start(int id, Clock::time_point now) const {
    const auto* u = find(id);
    if (u == nullptr) return false;
    // More users than budget: everyone still gets one connection, so the total may exceed the
    // budget by design only when shares round up to 1.
    if (total_active() >= std::max(budget_, users())) return false;
    if (last_start_ && now - *last_start_ < stagger) return false;
    return u->active < share(id) || !other_starved(id);
}

void ServerPool::waiting(int id, int delta) {
    auto it = std::ranges::find(users_, id, &User::id);
    if (it != users_.end()) it->waiting = std::max(0, it->waiting + delta);
}

bool ServerPool::should_yield(int id) const {
    const auto* u = find(id);
    return u != nullptr && u->active > share(id) && other_starved(id);
}

ServerPool::Clock::time_point ServerPool::next_start(Clock::time_point now) const {
    if (!last_start_) return now;
    return std::max(now, *last_start_ + stagger);
}

void ServerPool::started(int id, Clock::time_point now) {
    auto it = std::ranges::find(users_, id, &User::id);
    if (it == users_.end()) return;
    ++it->active;
    last_start_ = now;
}

void ServerPool::finished(int id) {
    auto it = std::ranges::find(users_, id, &User::id);
    if (it != users_.end() && it->active > 0) --it->active;
}

bool ServerPool::refused() {
    if (total_active() < 2 || budget_ <= 1) return false;
    --budget_;
    return true;
}

} // namespace grab::budget
