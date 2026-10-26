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

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace grab::sftp {

// The byte stream SFTP runs on: an SSH channel, or a fake server in tests. Both calls block.
class Channel {
public:
    virtual ~Channel() = default;
    virtual void write(std::string_view bytes) = 0;
    // At least one byte into `buffer`, as many as are there; throws Broken when the stream ends.
    [[nodiscard]] virtual std::size_t read(std::span<char> buffer) = 0;
};

struct Broken : std::runtime_error {
    using std::runtime_error::runtime_error;
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

struct Attrs {
    std::optional<std::uint64_t> size;
    std::optional<std::uint32_t> permissions; // includes the file type bits (S_IFMT)
    std::optional<std::uint32_t> mtime;       // seconds since 1970
    [[nodiscard]] bool is_dir() const;
    [[nodiscard]] bool is_file() const;
    [[nodiscard]] bool is_link() const;
};

struct Entry {
    std::string name; // as the server sent it: bytes, not necessarily UTF-8
    Attrs attrs;
};

// Requests one Client keeps in flight unless told otherwise, and the largest READ: OpenSSH's
// sftp-server answers at most 255 KiB per READ. Phase 0 measured that a Storage Box connection's
// speed follows the bytes in flight (16 READs: about 1 MiB/s; 64: as fast as rclone).
inline constexpr std::uint32_t max_read = 255 * 1024;

class Client {
public:
    explicit Client(Channel& channel);
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    // INIT and the server's VERSION. Throws Broken (also for a version below 3).
    void start();
    [[nodiscard]] std::uint32_t version() const { return version_; }
    [[nodiscard]] bool has_extension(std::string_view name) const;

    template <typename T> using Done = std::move_only_function<void(std::expected<T, Status>)>;
    // The data of a READ, valid only during the call; an empty view never comes (EOF is a Status
    // with Code::eof).
    using DataDone = std::move_only_function<void(std::expected<std::string_view, Status>)>;

    // Requests, queued until flush() or pump(). Paths and handles are raw bytes.
    void open(std::string_view path, Done<std::string> done); // for reading; gives the handle
    void read(std::string_view handle, std::uint64_t offset, std::uint32_t length, DataDone done);
    void close(std::string_view handle, Done<void> done = {});
    void opendir(std::string_view path, Done<std::string> done);
    void readdir(std::string_view handle, Done<std::vector<Entry>> done); // Code::eof at the end
    void stat(std::string_view path, Done<Attrs> done);
    void lstat(std::string_view path, Done<Attrs> done);
    void fstat(std::string_view handle, Done<Attrs> done);
    void realpath(std::string_view path, Done<std::string> done);

    void flush();
    // Requests sent or queued whose replies haven't come yet.
    [[nodiscard]] std::size_t pending() const { return pending_.size(); }
    // Flushes, waits for the next reply and runs its callback. Throws Broken; also when nothing
    // is pending, as no reply would ever come.
    void pump();

private:
    struct Reply {
        std::uint8_t type;
        std::string_view body; // after the request id
    };
    using Handler = std::move_only_function<void(const Reply&)>;
    void request(std::uint8_t type, std::string_view payload, Handler handler);
    void attrs_request(std::uint8_t type, std::string_view arg, Done<Attrs> done);
    void read_packet();
    void read_exact(char* to, std::size_t n);

    Channel& channel_;
    std::uint32_t version_ = 0;
    std::vector<std::string> extensions_;
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
[[nodiscard]] std::expected<std::string, Status> realpath(Client& client, std::string_view path);
// Every entry of a folder except "." and "..", in the server's order.
[[nodiscard]] std::expected<std::vector<Entry>, Status> list(Client& client, std::string_view dir);

// Reads [offset, offset + length) of an open file with up to `depth` READs of at most `chunk`
// bytes in flight, never asking past the end of the range. A short DATA (servers may send less
// than asked) is asked again for the rest. `sink(offset, bytes)` gets the data as it arrives,
// not necessarily in order. The file ending before the range does is a Status with Code::eof.
using Sink = std::function<void(std::uint64_t offset, std::string_view bytes)>;
[[nodiscard]] std::expected<void, Status> read_range(Client& client, std::string_view handle, std::uint64_t offset,
                                                     std::uint64_t length, const Sink& sink, int depth = 64,
                                                     std::uint32_t chunk = max_read);

} // namespace grab::sftp
