#pragma once

#include <charconv>
#include <cstdint>
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
    std::uint64_t generation = 0;
    std::string key_z85;
};

// "<agent_id>:<generation>:<z85 key>". agent_id (letters, digits and "._-")
// and generation (decimal digits) never contain ':', but the Z85 alphabet
// does, so the key goes last and takes everything after the second ':'.
inline std::string make_session_identity(const std::string& agent_id, std::uint64_t generation,
                                         const std::string& key_z85)
{
    return agent_id + ":" + std::to_string(generation) + ":" + key_z85;
}

// Splits a user_id made by make_session_identity(). nullopt when it has no
// agent_id, generation or key (for example the empty user_id of an unbound
// key) or the generation is not a plain decimal number.
inline std::optional<SessionIdentity> parse_session_identity(const std::string& user_id)
{
    const auto first = user_id.find(':');
    if (first == std::string::npos || first == 0)
        return std::nullopt;
    const auto second = user_id.find(':', first + 1);
    if (second == std::string::npos || second == first + 1 || second + 1 == user_id.size())
        return std::nullopt;

    SessionIdentity identity;
    identity.agent_id = user_id.substr(0, first);
    const char* const digits_begin = user_id.data() + first + 1;
    const char* const digits_end = user_id.data() + second;
    const auto parsed = std::from_chars(digits_begin, digits_end, identity.generation);
    if (parsed.ec != std::errc{} || parsed.ptr != digits_end)
        return std::nullopt;
    identity.key_z85 = user_id.substr(second + 1);
    return identity;
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

    // The approval generation of an approved key: it changes whenever the key
    // is approved afresh (added after being removed, or for another agent_id)
    // and stays the same when the same binding is added again. nullopt when
    // the key is not approved. Sessions opened under an older generation are
    // stale (#183).
    std::optional<std::uint64_t> generation_for_key(const std::string& z85_key) const;

private:
    // Thread body: binds inproc://zeromq.zap.01 and answers each ZAP request
    // until stopped. Accepts a CURVE peer only if its key is approved, and
    // returns the bound agent_id as user_id (SEC-04). Accepts NULL peers for the
    // enrollment socket. Any other or malformed request is denied with 400.
    void run(std::stop_token st);

    zmq::context_t& ctx_;
    mutable std::shared_mutex keys_mutex_;
    struct Binding
    {
        std::string agent_id;
        std::uint64_t generation = 0;
    };
    std::unordered_map<std::string, Binding> approved_keys_; // z85 key -> binding
    std::uint64_t next_generation_ = 1;                      // guarded by keys_mutex_
    std::jthread thread_;
};

} // namespace thewatcher::server
