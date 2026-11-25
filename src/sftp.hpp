#pragma once

// grab's own SFTP client (protocol version 3, draft-ietf-secsh-filexfer-02, which OpenSSH and
// Storage Boxes speak), replacing rclone's. Its own rather than libssh2's SFTP so grab decides
// exactly which bytes are asked for: rclone's read-ahead fetched up to 3.4x the data when grab
// split files into pieces (RCLONE-REPLACEMENT.md, section 2).
//
// One Client per SSH channel, used by one thread. Requests are queued and sent together; replies
// come back in any order and run the callback given with each request. Failures of a single
// request (no such file, permission denied) come back as a Status; a broken stream or a server
// that breaks the protocol throws Broken, after which the Client is done.
//
// Writes (uploads, UPLOADS.md 5.1) never wait for a reply while there is room: each WRITE goes out
// as it is queued, the replies already here are taken without waiting (pump_ready), and only at a
// cap of WRITEs or bytes in flight does the writer wait for one (write_range).
//
// The server's own copies (SERVER-OPS.md): copy-data copies a file's bytes on the server, none
// through grab; copy_range does it in pieces, so a big file shows progress and can be stopped.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace grab::sftp {

// The byte stream SFTP runs on: an SSH channel, or a fake server in tests. write and read block;
// readable doesn't.
class Channel {
public:
    virtual ~Channel() = default;
    // Waits while the server's window is full (an SSH channel), so the server sets the pace.
    virtual void write(std::string_view bytes) = 0;
    // At least one byte into `buffer`, as many as are there; throws Broken when the stream ends.
    [[nodiscard]] virtual std::size_t read(std::span<char> buffer) = 0;
    // Bytes have arrived that read() would give without waiting for the server.
    [[nodiscard]] virtual bool readable() = 0;
};

struct Broken : std::runtime_error {
    using std::runtime_error::runtime_error;
};
// The server refused to start SFTP at all (no sftp subsystem): another connection can't help.
struct Unavailable : Broken {
    using Broken::Broken;
};
// The server broke the protocol (a malformed or unexpected reply, a version below 3), which a
// dropped link doesn't.
struct ProtocolError : Broken {
    using Broken::Broken;
};

// SSH_FX_* status codes (version 3 defines 0-8; servers may send more).
enum class Code : std::uint32_t {
    ok = 0,
    eof = 1,
    no_such_file = 2,
    permission_denied = 3,
    failure = 4,
    bad_message = 5,
    no_connection = 6,
    connection_lost = 7,
    op_unsupported = 8,
};

struct Status {
    Code code = Code::failure;
    std::string message; // the server's text, possibly empty
};

// What grab tells the user for a failed request ("permission denied", or the server's own text).
[[nodiscard]] std::string describe(const Status& status);

// A file's attributes as a server sends them, and as grab sets them (SETSTAT, a new file's or
// folder's): only those present. SFTP sends the two times together and the owner with the group:
// an mtime goes with the atime (the mtime itself when there is none), and an atime without an
// mtime isn't sent; uid and gid go only both together (one alone isn't sent). A server changes
// the owner only for root (OpenSSH: chown(2)), so a SETSTAT with someone else's uid fails.
struct Attrs {
    std::optional<std::uint64_t> size;
    std::optional<std::uint32_t> permissions; // includes the file type bits (S_IFMT)
    std::optional<std::uint32_t> mtime;       // seconds since 1970
    std::optional<std::uint32_t> uid;         // the owner's number (users_groups_by_id names it)
    std::optional<std::uint32_t> gid;         // the group's
    std::optional<std::uint32_t> atime;       // the last access, seconds since 1970
    [[nodiscard]] bool is_dir() const;
    [[nodiscard]] bool is_file() const;
    [[nodiscard]] bool is_link() const;
};

struct Entry {
    std::string name; // as the server sent it: bytes, not necessarily UTF-8
    Attrs attrs;
};

// An extension the server offered in its VERSION: its name and data (OpenSSH's: a version, "1").
struct Extension {
    std::string name;
    std::string data;
};

// OPEN's flags (SSH_FXF_*), or'ed together. OpenSSH maps them onto open(2): creat | excl fails on
// an existing name, trunc empties the file, append writes at its end whatever the offset.
struct OpenFlags {
    static constexpr std::uint32_t read = 0x01, write = 0x02, append = 0x04, creat = 0x08, trunc = 0x10, excl = 0x20;
};

// statvfs@openssh.com's answer: statvfs(3) of the file system a path is on.
struct Statvfs {
    std::uint64_t bsize = 0, frsize = 0, blocks = 0, bfree = 0, bavail = 0, files = 0, ffree = 0, favail = 0, fsid = 0,
                  flag = 0, namemax = 0;
    // The bytes this account may still write there: bavail blocks of frsize (of bsize when the
    // server gives no frsize); the most a uint64 holds when that is more.
    [[nodiscard]] std::uint64_t free_bytes() const;
    [[nodiscard]] bool read_only() const { return (flag & 0x1) != 0; } // ST_RDONLY
};

// limits@openssh.com's answer: the largest packet, READ and WRITE the server takes, and the
// handles one session may hold open (0: it doesn't say).
struct Limits {
    std::uint64_t packet = 0, read = 0, write = 0, handles = 0;
};

// users-groups-by-id@openssh.com's answer: the name of each number asked for, in the order asked;
// "" for one the server knows no name for. Bytes as the server sent them, not necessarily UTF-8.
struct IdNames {
    std::vector<std::string> users;
    std::vector<std::string> groups;
};

// The extensions grab's own copies and owners' names need, by the names servers offer them under.
inline constexpr std::string_view copy_data_extension = "copy-data";
inline constexpr std::string_view users_groups_extension = "users-groups-by-id@openssh.com";

// Requests one Client keeps in flight unless told otherwise, and the largest READ: OpenSSH's
// sftp-server answers at most 255 KiB per READ. Phase 0 measured that a Storage Box connection's
// speed follows the bytes in flight (16 READs: about 1 MiB/s; 64: as fast as rclone).
inline constexpr std::uint32_t max_read = 255 * 1024;
// The largest WRITE: OpenSSH's sftp-server takes packets of at most 256 KiB and says 255 KiB of
// data in limits@openssh.com; a bigger packet ends its session. And the bytes write_range keeps
// sent but not yet acknowledged unless told otherwise (UPLOADS.md section 7, until chunk 0's
// measurements set it, section 13).
inline constexpr std::uint32_t max_write = 255 * 1024;
inline constexpr std::uint64_t max_unacked = std::uint64_t{8} * 1024 * 1024;
// The most copy_range asks one copy-data for (SERVER-OPS.md, decision 1): the server answers only
// once a piece is copied, so this sets how often progress shows and a stop is seen. A piece must
// also be done within the connection's timeout (Target::timeout, 30 s unless set: OpenSSH's
// sftp-server sends nothing meanwhile, and the read waiting for the answer gives up), which at
// 256 MiB a server copying below about 8.5 MiB/s isn't: so copy_range starts with copy_first_piece
// and sizes each next piece to the pace the last one showed (next_copy_piece), never below
// copy_least_piece nor above this.
inline constexpr std::uint64_t copy_piece = std::uint64_t{256} * 1024 * 1024;
inline constexpr std::uint64_t copy_first_piece = std::uint64_t{8} * 1024 * 1024;
inline constexpr std::uint64_t copy_least_piece = std::uint64_t{1} * 1024 * 1024;

// The most entries grab takes from a server in one listing: one folder, a walk or a find (Main's
// whole file list is about 17,000), so a broken or hostile server can't exhaust memory or keep
// grab reading forever (review G168).
inline constexpr std::size_t max_listing = 1'000'000;
// ...and the most bytes of paths it keeps from one, since a name can be as long as a packet and a
// path grows with its depth.
inline constexpr std::uint64_t max_listing_bytes = std::uint64_t{1} << 30;

class Client {
public:
    explicit Client(Channel& channel);
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    // INIT and the server's VERSION. Throws Broken (ProtocolError for a version below 3).
    void start();
    [[nodiscard]] std::uint32_t version() const { return version_; }
    [[nodiscard]] bool has_extension(std::string_view name) const;
    [[nodiscard]] const std::vector<Extension>& extensions() const { return extensions_; }

    template <typename T> using Done = std::move_only_function<void(std::expected<T, Status>)>;
    // The data of a READ, valid only during the call; an empty view never comes (EOF is a Status
    // with Code::eof).
    using DataDone = std::move_only_function<void(std::expected<std::string_view, Status>)>;

    // Requests, queued until flush() or pump(). Paths and handles are raw bytes.
    void open(std::string_view path, Done<std::string> done); // for reading; gives the handle
    // OPEN with `flags` (OpenFlags); a file it creates gets `attrs` (permissions, which the
    // server's umask trims; none: the server's default).
    void open(std::string_view path, std::uint32_t flags, const Attrs& attrs, Done<std::string> done);
    void read(std::string_view handle, std::uint64_t offset, std::uint32_t length, DataDone done);
    // WRITE of `data` at `offset` (a file opened with OpenFlags::append: at its end). The bytes go
    // into the queue right behind the request's header, copied once; at most max_write of them.
    void write(std::string_view handle, std::uint64_t offset, std::string_view data, Done<void> done);
    void close(std::string_view handle, Done<void> done = {});
    void opendir(std::string_view path, Done<std::string> done);
    void readdir(std::string_view handle, Done<std::vector<Entry>> done); // Code::eof at the end
    void stat(std::string_view path, Done<Attrs> done);
    void lstat(std::string_view path, Done<Attrs> done);
    void fstat(std::string_view handle, Done<Attrs> done);
    void realpath(std::string_view path, Done<std::string> done);
    // SETSTAT and FSETSTAT: the attributes `attrs` holds (a size cuts or extends the file).
    void setstat(std::string_view path, const Attrs& attrs, Done<void> done);
    void fsetstat(std::string_view handle, const Attrs& attrs, Done<void> done);
    void remove(std::string_view path, Done<void> done);                       // a file
    void mkdir(std::string_view path, const Attrs& attrs, Done<void> done);    // an existing name fails it
    void rmdir(std::string_view path, Done<void> done);                        // an empty folder
    // RENAME. OpenSSH's never replaces anything: an existing `to` fails it (Code::failure).
    void rename(std::string_view from, std::string_view to, Done<void> done);
    // EXTENDED `name` with `payload` (the request's fields after the name): the EXTENDED_REPLY's
    // bytes, or none for a STATUS ok. A server without it answers Code::op_unsupported.
    void extended(std::string_view name, std::string_view payload, Done<std::string> done);
    // OpenSSH's extensions; send them only when offered (has_extension).
    // posix-rename@openssh.com: rename(2), which replaces an existing `to` in one step.
    void posix_rename(std::string_view from, std::string_view to, Done<void> done);
    // statvfs@openssh.com: the file system `path` is on.
    void statvfs(std::string_view path, Done<Statvfs> done);
    // limits@openssh.com.
    void limits(Done<Limits> done);
    // fsync@openssh.com: an open file's bytes onto the server's disk.
    void fsync(std::string_view handle, Done<void> done);
    // copy-data (copy_data_extension; offered by OpenSSH's sftp-server): the server copies
    // `length` bytes (0: up to the end of the file) of the file open as `from`, from
    // `from_offset`, into the file open for writing as `to`, at `to_offset` (a file opened with
    // OpenFlags::append: at its end); no byte comes through grab. OpenSSH's answers: the same
    // handle or the same name twice, or a folder's handle, "Failure"; `to` not open for writing
    // "no such file" (EBADF); a full disk "Failure"; the file ending before `length` bytes, EOF
    // (what came before it is written). It copies 64 KiB at a time and counts each piece as done
    // when it asks for it, so a write that fails, or the file ending, within the LAST 64 KiB is
    // answered ok: look at the target's size after (copy_range does).
    void copy_data(std::string_view from, std::uint64_t from_offset, std::uint64_t length, std::string_view to,
                   std::uint64_t to_offset, Done<void> done);
    // users-groups-by-id@openssh.com (users_groups_extension): the names of owners' and groups'
    // numbers (Attrs::uid, Attrs::gid), each list sent packed as OpenSSH's sftp-server reads it.
    void users_groups_by_id(std::span<const std::uint32_t> uids, std::span<const std::uint32_t> gids, Done<IdNames> done);
    // READLINK: where the link at `path` points, as stored (a relative target is from the link's
    // folder). OpenSSH answers a name that isn't a link with Code::bad_message (EINVAL).
    void readlink(std::string_view path, Done<std::string> done);
    // SYMLINK: a new link at `link` pointing to `target` (stored as given; an existing `link`
    // fails it). OpenSSH's sftp-server takes the two paths in the order opposite to the SFTP
    // draft's (linkpath, targetpath): the target first, then the link. This sends OpenSSH's order;
    // a server keeping to the draft would make a link at `target` instead, so a caller that needs
    // to know reads the link back (READLINK).
    void symlink(std::string_view link, std::string_view target, Done<void> done);

    void flush();
    // Requests sent or queued whose replies haven't come yet.
    [[nodiscard]] std::size_t pending() const { return pending_.size(); }
    // Flushes, waits for the next reply and runs its callback. Throws Broken (ProtocolError for a
    // reply that breaks the protocol); also when nothing is pending, as no reply would ever come.
    void pump();
    // Runs the callbacks of the replies that are here already (in this Client's buffer or the
    // channel's, Channel::readable), waiting for none: how many ran. Sends nothing. Throws as pump.
    std::size_t pump_ready();

private:
    struct Reply {
        std::uint8_t type;
        std::string_view body; // after the request id
    };
    using Handler = std::move_only_function<void(const Reply&)>;
    // The header of a request whose fields after the id take `size` bytes, into the queue; the
    // caller puts the fields behind it.
    void begin(std::uint8_t type, std::size_t size, Handler handler);
    void request(std::uint8_t type, std::string_view payload, Handler handler);
    void attrs_request(std::uint8_t type, std::string_view arg, Done<Attrs> done);
    // A reply that is only a STATUS: ok, or the Status.
    static Handler status_only(Done<void> done);
    void take_reply();
    [[nodiscard]] bool packet_buffered() const;
    void read_packet();
    void read_exact(char* to, std::size_t n);

    Channel& channel_;
    std::uint32_t version_ = 0;
    std::vector<Extension> extensions_;
    std::uint32_t next_id_ = 1;
    std::unordered_map<std::uint32_t, Handler> pending_;
    std::string out_;
    std::vector<char> in_;
    std::size_t in_pos_ = 0, in_len_ = 0;
    std::string packet_; // the last packet, reused
};

// Blocking helpers: send one request and pump until its reply (other replies run their callbacks
// on the way).
[[nodiscard]] std::expected<Attrs, Status> stat(Client& client, std::string_view path);
[[nodiscard]] std::expected<Attrs, Status> lstat(Client& client, std::string_view path);
[[nodiscard]] std::expected<Attrs, Status> fstat(Client& client, std::string_view handle);
// A file opened for reading: its handle.
[[nodiscard]] std::expected<std::string, Status> open_file(Client& client, std::string_view path);
// A file opened with `flags` (OpenFlags; a file it creates gets `attrs`): its handle.
[[nodiscard]] std::expected<std::string, Status> open_file(Client& client, std::string_view path, std::uint32_t flags,
                                                           const Attrs& attrs = {});
[[nodiscard]] std::expected<void, Status> close_file(Client& client, std::string_view handle);
[[nodiscard]] std::expected<std::string, Status> realpath(Client& client, std::string_view path);
[[nodiscard]] std::expected<void, Status> setstat(Client& client, std::string_view path, const Attrs& attrs);
[[nodiscard]] std::expected<void, Status> fsetstat(Client& client, std::string_view handle, const Attrs& attrs);
[[nodiscard]] std::expected<void, Status> remove(Client& client, std::string_view path);
[[nodiscard]] std::expected<void, Status> mkdir(Client& client, std::string_view path, const Attrs& attrs = {});
[[nodiscard]] std::expected<void, Status> rmdir(Client& client, std::string_view path);
[[nodiscard]] std::expected<void, Status> rename(Client& client, std::string_view from, std::string_view to);
[[nodiscard]] std::expected<void, Status> posix_rename(Client& client, std::string_view from, std::string_view to);
[[nodiscard]] std::expected<Statvfs, Status> statvfs(Client& client, std::string_view path);
[[nodiscard]] std::expected<Limits, Status> limits(Client& client);
[[nodiscard]] std::expected<void, Status> fsync(Client& client, std::string_view handle);
// copy-data and users-groups-by-id, sent only when the server offered them: else Code::op_unsupported
// at once, nothing sent.
[[nodiscard]] std::expected<void, Status> copy_data(Client& client, std::string_view from, std::uint64_t from_offset,
                                                    std::uint64_t length, std::string_view to, std::uint64_t to_offset);
[[nodiscard]] std::expected<IdNames, Status> users_groups_by_id(Client& client, std::span<const std::uint32_t> uids,
                                                                std::span<const std::uint32_t> gids);
[[nodiscard]] std::expected<std::string, Status> readlink(Client& client, std::string_view path);
[[nodiscard]] std::expected<void, Status> symlink(Client& client, std::string_view link, std::string_view target);
// Every entry of a folder except "." and "..", in the server's order. Past `limit` entries it
// stops: a Status (Code::failure) saying the folder holds too many.
[[nodiscard]] std::expected<std::vector<Entry>, Status> list(Client& client, std::string_view dir,
                                                             std::size_t limit = max_listing);

// Reads [offset, offset + length) of an open file with up to `depth` READs of at most `chunk`
// bytes in flight, never asking past the end of the range. A short DATA (servers may send less
// than asked) is asked again for the rest. `sink(offset, bytes)` gets the data as it arrives,
// not necessarily in order. The file ending before the range does is a Status with Code::eof.
using Sink = std::function<void(std::uint64_t offset, std::string_view bytes)>;
[[nodiscard]] std::expected<void, Status> read_range(Client& client, std::string_view handle, std::uint64_t offset,
                                                     std::uint64_t length, const Sink& sink, int depth = 64,
                                                     std::uint32_t chunk = max_read);

// The bytes write_range sends: exactly `length` of them from `offset`, valid until its next call.
// Fewer end the range there (a Status with Code::eof once the replies came).
using Source = std::function<std::string_view(std::uint64_t offset, std::uint32_t length)>;
// A WRITE the server acknowledged, as its reply comes (not necessarily in order).
using Acked = std::function<void(std::uint64_t offset, std::uint32_t length)>;

// Writes [offset, offset + length) of a file open for writing, in WRITEs of at most `chunk` bytes,
// with at most `depth` of them and `unacked` bytes sent but not yet acknowledged (at least one
// WRITE however small `unacked` is). Each WRITE goes out as it is queued, held back only while the
// server's window is full; the replies already here are taken between WRITEs without waiting
// (Client::pump_ready), and it waits for one only at a cap. The first failure stops it once every
// WRITE in flight has its reply; the ones acknowledged meanwhile still reach `acked`.
[[nodiscard]] std::expected<void, Status> write_range(Client& client, std::string_view handle, std::uint64_t offset,
                                                      std::uint64_t length, const Source& source,
                                                      const Acked& acked = {}, int depth = 64,
                                                      std::uint64_t unacked = max_unacked,
                                                      std::uint32_t chunk = max_write);

// The bytes copy_range has copied so far, after each piece.
using Copied = std::function<void(std::uint64_t copied)>;
// The time copy_range times its pieces by: steady_clock's unless a test gives its own.
using Now = std::function<std::chrono::steady_clock::time_point()>;

// The piece copy_range asks for after one of `bytes` took `took`: what the server copies in a
// third of `budget` (the connection's timeout) at that pace, at least copy_least_piece, at most
// `cap`. No budget (0 or less), or no time taken: `cap`.
[[nodiscard]] std::uint64_t next_copy_piece(std::uint64_t bytes, std::chrono::nanoseconds took,
                                            std::chrono::milliseconds budget, std::uint64_t cap);

// Copies [from_offset, from_offset + length) of the file open as `from` into the file open for
// writing (not appending) as `to`, at `to_offset`, by copy-data requests one after the other: the
// server copies, nothing comes through grab. Each piece must be done within `budget`, the
// connection's timeout (Target::timeout): the first is copy_first_piece, each next one sized by
// next_copy_piece, none above `piece`; no budget (0: a Target made by hand), every piece `piece`.
// After each piece the target's size (FSTAT) must reach where the piece ended, as OpenSSH answers
// ok for a piece whose last 64 KiB failed (copy_data): shorter is a Status (Code::failure), and so
// is a server that doesn't tell the size. `copied` follows each piece; `stop` is looked at before
// each one. The bytes copied: `length`, or fewer when stopped. Only for a target with nothing past
// where the copy writes (a new or emptied file): one longer already hides a short piece. Not
// offered: Code::op_unsupported, nothing sent.
[[nodiscard]] std::expected<std::uint64_t, Status> copy_range(Client& client, std::string_view from, std::uint64_t from_offset,
                                                              std::uint64_t length, std::string_view to,
                                                              std::uint64_t to_offset, std::chrono::milliseconds budget,
                                                              const Copied& copied = {}, std::stop_token stop = {},
                                                              std::uint64_t piece = copy_piece, const Now& now = {});

} // namespace grab::sftp
