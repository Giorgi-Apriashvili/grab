#include "ssh_target.hpp"

#include "util.hpp"

#include <optional>
#include <string_view>

namespace grab::ssh {

std::expected<Target, std::string> target_for(const RcloneRemote& remote, const secrets::Store& store,
                                              const std::filesystem::path& default_known_hosts) {
    if (remote.host.empty() || remote.user.empty())
        return util::failf("{}: its host and user are needed", remote.name);
    Target t;
    t.host = remote.host;
    t.port = remote.port;
    t.user = remote.user;
    t.known_hosts = remote.known_hosts_file ? util::path_from_utf8(*remote.known_hosts_file) : default_known_hosts;

    const auto values = store.find(remote.name);
    auto secret = [&](std::string_view key) -> std::optional<std::string> {
        if (values == store.end()) return std::nullopt;
        auto it = values->second.find(std::string(key));
        if (it == values->second.end()) return std::nullopt;
        return it->second;
    };
    // Obscured values made plain; one that isn't in rclone's form is an error, not a password.
    auto plain = [&](std::string_view key) -> std::expected<std::string, std::string> {
        auto v = secret(key);
        if (!v) return std::string();
        auto revealed = secrets::reveal(*v);
        if (!revealed) return util::failf("{}: its stored {} can't be read", remote.name, key);
        return *revealed;
    };

    if (secret("key_pem")) return util::failf("{}: a key kept inside the settings (key_pem) isn't supported yet", remote.name);
    if (remote.key_use_agent) {
        t.agent = true;
    } else if (remote.key_file) {
        t.key_file = util::path_from_utf8(*remote.key_file);
        auto passphrase = plain("key_file_pass");
        if (!passphrase) return std::unexpected(passphrase.error());
        t.key_passphrase = std::move(*passphrase);
    }
    auto password = plain("pass");
    if (!password) return std::unexpected(password.error());
    t.password = std::move(*password);
    if (!t.agent && t.key_file.empty() && t.password.empty())
        return util::failf("{}: no ssh-agent, key file or password to log in with", remote.name);
    return t;
}

} // namespace grab::ssh
