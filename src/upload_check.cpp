#include "upload_check.hpp"

#include "folder_job.hpp"
#include "local_name.hpp"
#include "rclone_encoding.hpp"
#include "sftp.hpp"
#include "ssh.hpp"
#include "ssh_pool.hpp"
#include "upload.hpp"
#include "util.hpp"

#include <algorithm>
#include <cstddef>
#include <expected>
#include <format>
#include <mutex>
#include <optional>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

namespace grab::upload {

namespace fs = std::filesystem;

namespace {

// A connection's failure, as the dialog says it: naming the server.
std::string unreached(std::string_view server, std::string why) {
    if (why.starts_with(server)) return why;
    return std::format("Couldn't reach {}: {}", server, why);
}

// A name Win32 can't open as it is: a last dot or space is dropped from it (local_walk's
// lost_on_open). Only Windows: elsewhere such names are plain.
bool lost_on_open([[maybe_unused]] std::string_view name) {
#ifdef _WIN32
    return name.ends_with('.') || name.ends_with(' ');
#else
    return false;
#endif
}

std::int64_t seconds_of(std::int64_t ns) {
    return ns >= 0 ? ns / 1'000'000'000 : -((-ns + 999'999'999) / 1'000'000'000);
}

// A file: its size and time.
void look_at_file(Picked& p) {
    auto file = LocalFile::open(p.source);
    if (!file) {
        p.error = file.error();
        return;
    }
    p.files = 1;
    p.bytes = file->size();
    p.mtime_ns = file->mtime_ns();
}

// A folder: walked, its links and grab's own partial downloads left out.
void walk_folder(Picked& p, Check& check, const std::stop_token& stop) {
    p.tree = std::make_shared<Tree>();
    FolderJob job;
    job.server = check.server;
    job.source = p.source;
    job.remote_path = p.target;
    std::mutex m;
    if (auto walked = walk(job, *p.tree, m, stop); !walked) {
        p.error = walked.error();
        p.tree.reset();
        return;
    }
    if (stop.stop_requested()) return;
    const Tree& t = *p.tree;
    p.files = t.files.size();
    for (const auto& f : t.files) {
        p.bytes += f.size;
        // Only its own name: a folder's too-long name is counted once, as the folder.
        if (rclone_enc::raw_path(split_remote(f.path).second).size() > name_max_bytes) ++check.long_names;
    }
    for (const auto& d : t.folders) {
        if (rclone_enc::raw_path(split_remote(d).second).size() > name_max_bytes) ++check.long_names;
    }
    check.links += t.links;
    check.unreadable += t.unreadable.size();
}

// The picked files' names on the server (upload::look_at, many on their way at once: one round
// trip a batch, not a file), each one taken counted. False when the server couldn't be reached
// (check.why says so) or on a stop.
bool look_files(Check& check, ssh::Pool& pool, const std::stop_token& stop) {
    std::vector<Picked*> files;
    for (auto& p : check.items) {
        if (!p.folder && p.error.empty()) files.push_back(&p);
    }
    if (files.empty()) return true;
    struct Asked {
        Picked* p = nullptr;
        std::string raw;
        std::optional<sftp::Attrs> there;
        std::optional<sftp::Status> failed; // other than "no such file"
    };
    constexpr std::size_t batch = 256; // requests on their way at once, as make_folders
    std::optional<ssh::Pool::Lease> lease;
    try {
        lease = pool.acquire(stop, ssh::interactive_retry);
        if (!lease) return false;
        sftp::Client& c = (*lease)->sftp();
        for (std::size_t at = 0; at < files.size(); at += batch) {
            if (stop.stop_requested()) return false;
            std::vector<Asked> asked;
            asked.reserve(std::min(batch, files.size() - at));
            for (std::size_t k = at; k < files.size() && k < at + batch; ++k) {
                asked.push_back(Asked{.p = files[k], .raw = rclone_enc::raw_path(files[k]->target), .there = {}, .failed = {}});
            }
            for (Asked& a : asked) {
                c.lstat(a.raw, [&a](std::expected<sftp::Attrs, sftp::Status> r) {
                    if (r) a.there = *r;
                    else if (r.error().code != sftp::Code::no_such_file) a.failed = r.error();
                });
            }
            while (c.pending() > 0) c.pump();
            // A link: what it leads to (look_at), or a plain file when that can't be seen.
            for (Asked& a : asked) {
                if (!a.there || !a.there->is_link()) continue;
                c.stat(a.raw, [&a](std::expected<sftp::Attrs, sftp::Status> r) {
                    a.there = r ? *r : sftp::Attrs{.size = {}, .permissions = 0100644, .mtime = {}};
                });
            }
            while (c.pending() > 0) c.pump();
            for (Asked& a : asked) {
                Picked& p = *a.p;
                if (a.failed) {
                    p.error = std::format("cannot look at {}:{}: {}", check.server, p.target, sftp::describe(*a.failed));
                    continue;
                }
                if (!a.there) continue;
                p.clash = clash_with(p.name, p.bytes, seconds_of(p.mtime_ns), *a.there);
                check.taken.add(*p.clash);
            }
        }
        return true;
    } catch (const ssh::Error& e) {
        check.writable.reset();
        check.why = unreached(check.server, e.what());
        return false;
    } catch (const sftp::Broken& e) {
        if (lease) lease->broken();
        check.writable.reset();
        check.why = unreached(check.server, e.what());
        return false;
    }
}

} // namespace

void look_local(Check& check, const std::vector<fs::path>& picked, const std::function<void()>& progress,
                std::stop_token stop) {
    for (const auto& source : picked) {
        if (stop.stop_requested()) return;
        Picked p;
        p.source = source;
        p.name = util::path_to_utf8(source.filename());
        std::error_code ec;
        p.folder = fs::is_directory(source, ec); // a link picked is followed
        if (p.name.empty()) {
            p.error = std::format("{} is a whole drive: pick a folder in it", util::path_to_utf8(source));
        } else if (lost_on_open(p.name)) {
            // Windows would open "a" for "a." (local_walk's rule for the names below a folder).
            p.error = std::format("{}: its name ends with a dot or a space, which Windows programs can't open", p.name);
        } else {
            p.target = remote_child(check.dir, p.name);
            if (p.name.size() > name_max_bytes) ++check.long_names;
            if (p.folder) walk_folder(p, check, stop);
            else look_at_file(p);
        }
        if (stop.stop_requested()) return;
        if (p.error.empty()) {
            check.files += p.files;
            check.bytes += p.bytes;
        }
        check.items.push_back(std::move(p));
        if (progress) progress();
    }
}

void look_server(Check& check, const std::shared_ptr<ssh::Pool>& pool, fetch::Connections& connections,
                 std::stop_token stop, bool write) {
    {
        // One connection of the server's budget, as a listing takes (fetch::Listing): transfers
        // using all of it hand one back at once instead of when a range ends (review, chunk 3).
        const fetch::Listing seat(connections, check.server, stop, 1);
        if (stop.stop_requested()) return;
        auto probed = probe(*pool, check.server, check.dir, stop, write);
        if (stop.stop_requested()) return;
        if (!probed) {
            check.writable.reset();
            check.why = unreached(check.server, probed.error());
            return;
        }
        check.free = probed->free;
        check.writable = probed->writable;
        check.why = probed->why;
        if (!probed->writable) return;
        if (!look_files(check, *pool, stop)) return;
    }
    // The folders: whether each is there (merged into), and the names taken below it.
    for (auto& p : check.items) {
        if (!p.folder || !p.error.empty() || !p.tree) continue;
        if (stop.stop_requested()) return;
        FolderJob job;
        job.connections = &connections;
        job.server = check.server;
        job.source = p.source;
        job.remote_path = p.target;
        job.pool = pool;
        std::mutex m;
        if (auto seen = look(job, *p.tree, m, stop); !seen) {
            p.error = seen.error();
            continue;
        }
        if (stop.stop_requested()) return;
        p.merging = p.tree->merging;
        for (const auto& [i, clash] : p.tree->clashes) check.taken.add(clash);
    }
}

std::string nothing_goes(const Check& check) {
    if (check.items.empty()) return "Nothing was picked";
    if (std::ranges::any_of(check.items, [](const Picked& p) { return p.error.empty(); })) return {};
    return check.items.front().error;
}

bool short_of_space(const Check& check) { return check.free && *check.free < check.bytes; }

std::vector<Listed> taken_list(const Check& check, std::size_t at_most) {
    std::vector<Listed> out;
    for (std::size_t k = 0; k < check.items.size() && out.size() < at_most; ++k) {
        const Picked& p = check.items[k];
        if (!p.error.empty()) continue;
        if (p.clash) {
            out.push_back(Listed{.item = k, .file = std::nullopt, .name = p.name, .clash = *p.clash});
            continue;
        }
        if (!p.tree) continue;
        for (const auto& [i, clash] : p.tree->clashes) {
            if (out.size() == at_most) break;
            // Its name as it is here (grab's form only escapes what the server's name needs).
            out.push_back(Listed{.item = k, .file = i, .name = p.name + "/" + rclone_enc::raw_path(p.tree->files[i].path), .clash = clash});
        }
    }
    return out;
}

std::vector<Existing> apply_answer(Check& check, const Answer& answer) {
    std::vector<Existing> choices(check.items.size(), answer.all);
    if (answer.unlisted) {
        for (std::size_t k = 0; k < check.items.size(); ++k) {
            const Picked& p = check.items[k];
            if (!p.clash && (!p.tree || p.tree->clashes.empty())) choices[k] = *answer.unlisted;
        }
    }
    if (answer.own.empty()) return choices;
    const std::size_t last = answer.own.rbegin()->first;
    const auto listed = taken_list(check, last + 1);
    for (const auto& [at, own] : answer.own) {
        if (at >= listed.size() || own == Existing::ask) continue;
        const Listed& l = listed[at];
        if (!l.file) choices[l.item] = own;
        else if (const auto& tree = check.items[l.item].tree) tree->choices[*l.file] = own;
    }
    return choices;
}

bool dropped_copy(const fs::path& path, const fs::path& temp) {
    if (temp.empty() || !path.has_parent_path()) return false;
    const fs::path parent = path.parent_path();
    std::error_code ec;
    if (fs::equivalent(parent, temp, ec)) return true;
    // Folders that can't be opened: by their names, as Windows compares them (a last separator
    // or none: "C:\T\" is "C:\T").
    const auto folder = [](const fs::path& p) { return local::key((p / "x").lexically_normal().parent_path()); };
    return folder(parent) == folder(temp);
}

std::string drop_refusal(const std::vector<fs::path>& paths, const fs::path& temp, bool url) {
    if (paths.empty()) return std::string(drop_nothing);
    // Each folder looked at once: dropped_copy opens it and `temp`, and this runs on the window's
    // thread (a drop of thousands of files from one share is one folder).
    std::unordered_set<fs::path::string_type> seen;
    for (const auto& p : paths) {
        if (!p.has_parent_path() || !seen.insert(local::key(p.parent_path())).second) continue;
        if (dropped_copy(p, temp)) return std::string(url ? drop_link : drop_extract_first);
    }
    return {};
}

} // namespace grab::upload
