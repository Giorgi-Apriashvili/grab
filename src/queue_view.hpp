#pragma once

// What grab-gui's page is shown of the download queue (DOWNLOADS-UI.md 6), without the window:
// every file of a folder download is a row, sent once in full ("files", in chunks) and then only
// the rows that changed ("fileDelta"), found by comparing each file with what was last sent (its
// shadow) in the same pass that counts the folder's totals. The queue's items go the same way:
// whole ("queue") when they come, go or move, else only those whose text changed ("queueDelta").
// app.cpp holds the lock, posts the messages and answers the page's actions by index.

#include "fetch.hpp"
#include "folder.hpp"
#include "folder_job.hpp"
#include "json.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace grab::queue_view {

// A listed folder's file names: immutable, so the page's rows, queue.json and the actions read
// them without the queue's lock. A new copy (a new generation, "gen") is made when the folder is
// listed or restored, and when its files' local names change (Keep both); only then.
struct Names {
    std::vector<std::string> path;   // folder::File::path, by the file's index
    std::vector<std::uint64_t> size; // as listed; a done file's bytes are its size as fetched
    std::unordered_map<std::size_t, std::filesystem::path> local; // File::local, where grab gave one
    // The local path the page shows ('\'-separated), where it isn't the server's path: a name
    // Windows can't take (local::relative), or the one grab gave it.
    std::unordered_map<std::size_t, std::string> shown;
};
[[nodiscard]] Names names_of(const std::vector<folder::File>& files);
// Where the folder's files land, as folder::reserved_names says it from the tree.
[[nodiscard]] fetch::Reserved reserved_names(const std::filesystem::path& dir, const Names& names);
// Where file `i` lands (folder::target_of).
[[nodiscard]] std::filesystem::path target_of(const std::filesystem::path& dir, const Names& names, std::size_t i);

// A file as the page last got it. Small: a 177,000-file folder's shadow is compared at every flush.
struct Shadow {
    std::uint64_t bytes = 0;
    float speed = 0;
    std::uint32_t error = 0; // a hash of a failed file's error; 0 without one
    folder::State status = folder::State::queued;
    std::uint8_t weight = 2; // its priority in the folder (File::weight): high 4, normal 2, low 1
    bool verified = false;
};

// A file that changed since its shadow, for "fileDelta".
struct Row {
    std::size_t index = 0;
    Shadow now;
    std::optional<double> eta;
    std::string error; // a failed file's
};

// One pass over a folder's files: its totals and the files in each state (the folder's row), and
// with `shadow` (one per file) the files changed since it, which it then holds.
struct Pass {
    folder::Totals totals;
    std::array<std::size_t, 6> counts{}; // by folder::State
    std::vector<Row> changed;
};
[[nodiscard]] Pass pass(const std::vector<folder::File>& files, std::vector<Shadow>* shadow);

// What the page is sent first: every file as it is now, with the errors of failed ones. Its speed
// is 0, so a running file's comes with the first "fileDelta".
struct Snapshot {
    std::vector<Shadow> rows;
    std::unordered_map<std::size_t, std::string> errors;
};
[[nodiscard]] Snapshot snapshot(const std::vector<folder::File>& files);

// A folder's rows from `from` (at most `count`) of all its files, as one "files" message:
// {type, id, gen, from, total, rows: [[path, size, status, bytes, priority, local, error, verified,
// named], ...]}. Status (folder::State) and priority (the weight) are numbers; the last four only
// where set: local (Names::shown) and error a string or null, verified and named (grab gave it
// that name: Keep both, or a name another file has on Windows) 1 or 0.
[[nodiscard]] std::string files_message(int id, int gen, std::size_t from, std::size_t count, const Names& names,
                                        const Snapshot& snap);
// The rows that changed: {type: "fileDelta", id, gen, rows: [[index, status, bytes, speed, eta,
// priority, error, verified], ...]}; eta a number or null, error and verified only where set.
[[nodiscard]] std::string file_delta_message(int id, int gen, const std::vector<Row>& rows);

// The page's ranges of file indices, [[from, to], ...] with `to` included, as indices below
// `total`, sorted and each once. Anything else in them is left out.
[[nodiscard]] std::vector<std::size_t> indices_of(const json::Value* ranges, std::size_t total);

// The items whose text differs from what `sent` holds for their id, by their place in `items`;
// `sent` then holds `items` (ids gone are dropped).
[[nodiscard]] std::vector<std::size_t> changed_items(std::map<int, std::string>& sent,
                                                     const std::vector<std::pair<int, std::string>>& items);

} // namespace grab::queue_view
