#include "queue_row.hpp"

#include "folder_job.hpp"
#include "local_name.hpp"
#include "local_walk.hpp"
#include "remote.hpp"
#include "util.hpp"

namespace grab::queue_row {

std::filesystem::path local_of(const Row& r) {
    if (r.direction == Direction::up) return r.source;
    return r.local_name.empty() ? r.dest / local::relative(remote_basename(r.path)) : r.dest / r.local_name;
}

std::filesystem::path local_file(const Row& r, std::string_view rel, const std::filesystem::path& local_name) {
    if (r.direction == Direction::up) return local_walk::local_path(r.source, rel);
    const auto dir = folder::local_dir(r.dest, r.path);
    return local_name.empty() ? dir / local::relative(rel) : dir / local_name;
}

std::filesystem::path::string_type key(const Row& r) {
    if (r.direction == Direction::down) return local::key(local_of(r));
    return util::path_from_utf8("up|" + r.server + "|" + r.path).native();
}

bool allows(Direction direction, Act act) {
    switch (act) {
    case Act::open:
    case Act::open_folder:
    case Act::discard_local:
    case Act::resolve_existing: return direction == Direction::down;
    case Act::discard_server: return direction == Direction::up;
    }
    return false;
}

Act undo_of(const Row& r) { return r.direction == Direction::up ? Act::discard_server : Act::discard_local; }

} // namespace grab::queue_row
