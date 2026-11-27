#pragma once

#include <optional>
#include <shared_mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>

#include <zmq.hpp>

namespace thewatcher::server
{

// The ZAP user_id of an authenticated CURVE session (issue #144). libzmq
// stamps it once, at the handshake, so it carries both the agent_id and the
// key the session was authenticated with. The server compares the key with
// the agent's current record on every frame, so a session opened with a key
// that was later deleted or replaced stops being accepted.
struct SessionIdentity
{
    std::string agent_id;
    std::string key_z85;
};

// "<agent_id>:<z85 key>". agent_id never contains ':' (enrollment limits it
// to letters, digits and "._-"), so the first ':' splits the two.
inline std::string make_session_identity(const std::string& agent_id, const std::string& key_z85)
{
    return agent_id + ":" + key_z85;
}

// Splits a user_id made by make_session_identity(). nullopt when it has no
// agent_id or no key (for example the empty user_id of an unbound key).
inline std::optional<SessionIdentity> parse_session_identity(const std::string& user_id)
{
    const auto separator = user_id.find(':');
    if (separator == std::string::npos || separator == 0 || separator + 1 == user_id.size())
        return std::nullopt;
    return SessionIdentity{user_id.substr(0, separator), user_id.substr(separator + 1)};
}

// Runs the ZAP (ZeroMQ Authentication Protocol) handler on a background thread.
// Approves CURVE connections whose z85 public key is in the approved set and
// reports the bound agent_id and key as the ZAP user_id (see SessionIdentity),
// which libzmq stamps onto every message from that peer as the "User-Id"
// metadata property.
class ZapHandler
{
public:
    explicit ZapHandler(zmq::context_t& ctx);
    ~ZapHandler();

    // Registers an approved key. agent_id becomes the authenticated identity
    // for peers using this key; an empty agent_id leaves the key approved but
    // without a bound identity.
    void add_key(const std::string& z85_key, const std::string& agent_id = {});
    void remove_key(const std::string& z85_key);
    bool has_key(const std::string& z85_key) const;

    // The agent_id bound to an approved key, or nullopt when the key is
    // unknown or was added without an identity.
    std::optional<std::string> user_id_for_key(const std::string& z85_key) const;

private:
    // Thread body: binds inproc://zeromq.zap.01 and answers each ZAP request
    // until stopped. Accepts a CURVE peer only if its key is approved, and
    // returns the bound agent_id as user_id (SEC-04). Accepts NULL peers for the
    // enrollment socket. Any other or malformed request is denied with 400.
    void run(std::stop_token st);

    zmq::context_t& ctx_;
    mutable std::shared_mutex keys_mutex_;
    std::unordered_map<std::string, std::string> approved_keys_; // z85 key -> agent_id
    std::jthread thread_;
};

} // namespace thewatcher::server
