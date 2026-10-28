#include "preview_jobs.hpp"

#include "conn_budget.hpp"
#include "preview.hpp"
#include "rclone.hpp"
#include "sftp.hpp"
#include "ssh_read.hpp"
#include "util.hpp"

#include <algorithm>
#include <charconv>
#include <format>
#include <iterator>
#include <system_error>

namespace grab::preview {

namespace {

std::shared_ptr<const std::string> body_of(std::string text) {
    return std::make_shared<const std::string>(std::move(text));
}

// A whole decimal number, nothing around it.
template <class T>
std::optional<T> number_in(std::string_view text) {
    T n{};
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), n);
    if (ec != std::errc() || end != text.data() + text.size()) return std::nullopt;
    return n;
}

// A read that failed, in the window's words.
Answer read_failed(const ssh::ReadError& e, std::string_view remote, std::string_view name) {
    switch (e.kind) {
    case ssh::ReadFailure::stopped: return cancelled();
    case ssh::ReadFailure::login: {
        std::string text = std::format("Can't log in to {}: {}", remote, e.message);
        // A failure that can't pass by itself is kept until Test, Trust or a save (acquire_remembered).
        if (e.message.find("host key") != std::string::npos) text += ". If the key is right, use Trust… in Settings";
        else if (e.login == budget::Failure::fatal) text += ". Test the server in Settings to try again";
        return failed(502, text + ".");
    }
    case ssh::ReadFailure::status:
        if (e.code == sftp::Code::no_such_file) return failed(404, std::format("Not found on {}.", remote));
        return failed(502, std::format("Can't read {} on {}: {}.", name, remote, e.message));
    case ssh::ReadFailure::not_file: return failed(415, std::format("{} on {} isn't a regular file.", name, remote));
    case ssh::ReadFailure::lost: break;
    }
    return failed(502, std::format("Lost the connection to {}: {}.", remote, e.message));
}

} // namespace

Answer failed(int status, std::string message) {
    return Answer{.status = status,
                  .content_type = "text/plain; charset=utf-8",
                  .headers = {},
                  .body = body_of(std::move(message))};
}

Answer cancelled() { return failed(503, "cancelled"); }

std::string key_of(std::string_view server, std::string_view path) {
    std::string key(server);
    key += '\n';
    key += path;
    return key;
}

Fetched fetch(ssh::Reader& reader, std::string_view remote, std::string_view path) {
    Fetched f;
    const auto kind = kind_of(path);
    const std::uint64_t max = max_bytes(kind);
    if (max == 0) {
        f.answer = failed(415, "This kind of file has no preview.");
        return f;
    }
    const std::string name = remote_basename(path);
    // STAT first: only a regular file is opened, and its size decides before anything is read.
    const auto st = reader.stat();
    if (!st) {
        f.answer = read_failed(st.error(), remote, name);
        return f;
    }
    f.stamp.size = st->size;
    if (st->mtime) f.stamp.modified = *st->mtime;
    if (st->size && *st->size > max && kind != Kind::text) {
        f.answer = failed(413, std::format("Too big to preview ({}); download it instead.", util::format_size(*st->size)));
        return f;
    }
    auto body = reader.read(0, st->size ? std::min(*st->size, max) : max + 1);
    if (!body) {
        f.answer = read_failed(body.error(), remote, name);
        return f;
    }
    bool cut = st->size && *st->size > body->size();
    // Fewer bytes than STAT said: the file changed (was cut or rewritten) while it was read.
    const bool shrank = st->size && body->size() < std::min(*st->size, max);
    if (shrank && kind != Kind::text) {
        f.answer = failed(502, std::format("{} changed on {} while it was read.", name, remote));
        return f;
    }
    if (body->size() > max) { // no size from the server, and more than the cap
        if (kind != Kind::text) {
            f.answer = failed(413, std::format("Too big to preview (over {}); download it instead.", util::format_size(max)));
            return f;
        }
        body->resize(max);
        cut = true;
    }
    const std::string lower = util::to_lower(path);
    Answer answer{.status = 200, .content_type = content_type(path), .headers = {}, .body = {}};
    if (kind == Kind::text) {
        answer.body = body_of(text_utf8(*body, lower.ends_with(".nfo")));
        if (cut && st->size && !shrank) answer.headers = std::format("X-Grab-Total: {}", *st->size);
    } else {
        answer.body = body_of(std::move(*body));
    }
    // An SVG is shown as an image, where its scripts never run; if it were opened as a page
    // anyway, it gets no rights at all.
    if (lower.ends_with(".svg")) answer.headers = "Content-Security-Policy: sandbox";
    f.answer = std::move(answer);
    f.keep = !shrank; // text that changed is shown, not kept
    return f;
}

// ---- Cache -----------------------------------------------------------------------------------

std::optional<Answer> Cache::find(const std::string& key, const Stamp& row, Clock::time_point now) {
    const auto it = std::ranges::find(kept_, key, &Kept::key);
    if (it == kept_.end()) return std::nullopt;
    const bool fresh = now - it->fetched < fresh_;
    const bool same = row.size && row.modified && row == it->stamp;
    if (!fresh && !same) {
        kept_.erase(it);
        return std::nullopt;
    }
    kept_.splice(kept_.begin(), kept_, it);
    return kept_.front().answer;
}

void Cache::put(const std::string& key, Answer answer, Stamp stamp, Clock::time_point now) {
    std::erase_if(kept_, [&](const Kept& k) { return k.key == key; });
    kept_.push_front(Kept{.key = key, .answer = std::move(answer), .stamp = stamp, .fetched = now});
    std::size_t total = 0;
    for (auto it = kept_.begin(); it != kept_.end();) {
        total += it->answer.body ? it->answer.body->size() : 0;
        it = total > max_bytes_ && it != kept_.begin() ? kept_.erase(it) : std::next(it);
    }
}

void Cache::forget_server(std::string_view server) {
    const std::string prefix = key_of(server, "");
    std::erase_if(kept_, [&](const Kept& k) { return k.key.starts_with(prefix); });
}

std::size_t Cache::bytes() const {
    std::size_t total = 0;
    for (const auto& k : kept_) total += k.answer.body ? k.answer.body->size() : 0;
    return total;
}

// ---- Previews --------------------------------------------------------------------------------

std::shared_ptr<Previews::Job> Previews::ask(Request request, Reply reply) {
    std::string key = key_of(request.server, request.path);
    std::optional<Answer> kept;
    {
        std::scoped_lock lock(mutex_);
        kept = cache_.find(key, request.row);
        if (!kept) {
            if (const auto it = jobs_.find(key); it != jobs_.end()) {
                it->second->replies.push_back(std::move(reply));
                if (!request.ahead) it->second->ahead = false;
                return nullptr;
            }
            auto job = std::make_shared<Job>();
            job->key = key;
            job->server = std::move(request.server);
            job->path = std::move(request.path);
            job->row = request.row;
            job->ahead = request.ahead;
            job->replies.push_back(std::move(reply));
            jobs_.emplace(std::move(key), job);
            return job;
        }
    }
    reply(std::move(*kept));
    return nullptr;
}

bool Previews::begin(const std::shared_ptr<Job>& job) {
    std::vector<Reply> replies;
    std::optional<Answer> kept;
    {
        std::scoped_lock lock(mutex_);
        if (job->stop.stop_requested()) return false;
        kept = cache_.find(job->key, job->row);
        if (!kept) {
            job->running = true;
            return true;
        }
        replies = std::move(job->replies);
        job->replies.clear();
        if (const auto it = jobs_.find(job->key); it != jobs_.end() && it->second == job) jobs_.erase(it);
    }
    for (auto& reply : replies) reply(*kept);
    return false;
}

void Previews::end(const std::shared_ptr<Job>& job, const Fetched& fetched) {
    std::vector<Reply> replies;
    {
        std::scoped_lock lock(mutex_);
        if (const auto it = jobs_.find(job->key); it != jobs_.end() && it->second == job) jobs_.erase(it);
        replies = std::move(job->replies);
        job->replies.clear();
        if (fetched.keep) cache_.put(job->key, fetched.answer, fetched.stamp);
    }
    for (auto& reply : replies) reply(fetched.answer);
}

void Previews::cancel(const std::set<std::string>& keep) {
    std::vector<Reply> replies;
    {
        std::scoped_lock lock(mutex_);
        for (auto it = jobs_.begin(); it != jobs_.end();) {
            if (keep.contains(it->first)) {
                ++it;
                continue;
            }
            Job& job = *it->second;
            job.stop.request_stop();
            if (!job.running) {
                std::ranges::move(job.replies, std::back_inserter(replies));
                job.replies.clear();
            }
            it = jobs_.erase(it);
        }
    }
    for (auto& reply : replies) reply(cancelled());
}

void Previews::stop_all() {
    std::scoped_lock lock(mutex_);
    for (const auto& [key, job] : jobs_) job->stop.request_stop();
    jobs_.clear();
}

bool Previews::ahead(const Job& job) const {
    std::scoped_lock lock(mutex_);
    return job.ahead;
}

void Previews::forget_server(std::string_view server) {
    std::scoped_lock lock(mutex_);
    cache_.forget_server(server);
}

std::size_t Previews::jobs() const {
    std::scoped_lock lock(mutex_);
    return jobs_.size();
}

std::size_t Previews::cached_bytes() const {
    std::scoped_lock lock(mutex_);
    return cache_.bytes();
}

std::optional<Previews::Request> parse_url(std::string_view url) {
    constexpr std::string_view prefix = "https://grab.ui/preview/";
    if (!url.starts_with(prefix)) return std::nullopt;
    url.remove_prefix(prefix.size());
    url = url.substr(0, url.find('#'));
    const auto question = url.find('?');
    const std::string_view rest = url.substr(0, question);
    const auto slash = rest.find('/');
    if (slash == std::string_view::npos) return std::nullopt;
    auto server = url_decode(rest.substr(0, slash));
    auto path = url_decode(rest.substr(slash + 1));
    if (!server || !path || server->empty() || path->empty()) return std::nullopt;
    Previews::Request r{.server = std::move(*server), .path = std::move(*path), .row = {}, .ahead = false};
    std::string_view query = question == std::string_view::npos ? std::string_view() : url.substr(question + 1);
    while (!query.empty()) {
        const auto amp = query.find('&');
        const std::string_view part = query.substr(0, amp);
        query = amp == std::string_view::npos ? std::string_view() : query.substr(amp + 1);
        const auto eq = part.find('=');
        const std::string_view name = part.substr(0, eq);
        const std::string_view value = eq == std::string_view::npos ? std::string_view() : part.substr(eq + 1);
        if (name == "ahead") r.ahead = true;
        else if (name == "size") r.row.size = number_in<std::uint64_t>(value);
        else if (name == "modified") r.row.modified = number_in<std::int64_t>(value);
    }
    return r;
}

} // namespace grab::preview
