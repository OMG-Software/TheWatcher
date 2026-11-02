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

// Runs the ZAP (ZeroMQ Authentication Protocol) handler on a background thread.
// Approves CURVE connections whose z85 public key is in the approved set and
// reports the bound agent_id as the ZAP user_id, which libzmq stamps onto
// every message from that peer as the "User-Id" metadata property.
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
    void run(std::stop_token st);

    zmq::context_t& ctx_;
    mutable std::shared_mutex keys_mutex_;
    std::unordered_map<std::string, std::string> approved_keys_; // z85 key -> agent_id
    std::jthread thread_;
};

} // namespace thewatcher::server
