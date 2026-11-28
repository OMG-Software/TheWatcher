#pragma once

#include "common/collector_config.hpp"
#include "protocol.hpp"

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include <cbor.h>

namespace thewatcher
{

enum class CommandType : uint8_t
{
    SET_INTERVAL = 0x01,       // Change collection interval
    SET_PROCESS_LIMIT = 0x02,  // Change top-N process count
    RESTART_COLLECTORS = 0x03, // Restart all collectors
    PAUSE = 0x04,              // Pause metric collection
    RESUME = 0x05,             // Resume metric collection
    GET_STATUS = 0x06,         // Request an immediate status report
    DISCONNECT = 0x07,         // Graceful agent disconnect
};

// Sent from server → agent inside Frame::payload.
struct CommandMessage
{
    uint8_t command_type;      // CommandType cast to uint8_t
    std::string command_id;    // UUID, echoed in AckMessage
    std::vector<uint8_t> args; // command-specific CBOR payload
};

// Sent from agent → server inside Frame::payload after handling a command.
struct AckMessage
{
    std::string command_id;
    bool success = false;
    std::string message;
};

// Payload for SET_INTERVAL.
struct SetIntervalArgs
{
    int interval_seconds = 30;
};

// Payload for SET_PROCESS_LIMIT.
struct SetProcessLimitArgs
{
    int limit = 25;
};

// Sent from server → agent after admin changes agent config.
// Carried inside a CONFIG_UPDATE frame::payload.
struct ConfigUpdate
{
    int interval_seconds = 30;
    int process_limit = 25;
    CollectorConfig collector_config;
    int heartbeat_interval_seconds = 5;
};

} // namespace thewatcher

namespace thewatcher::proto::detail
{

// Narrows a wire int64 to int, throwing instead of silently wrapping
// (2^32 + 5 must not become 5).
inline int narrow_to_int(int64_t value)
{
    if (value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max())
        throw std::runtime_error("Integer out of range in command payload");
    return static_cast<int>(value);
}

template <>
inline CborPtr to_cbor(const thewatcher::CommandMessage& cmd)
{
    auto root = adopt(cbor_new_definite_array(3));

    push(root.get(), make_uint8(cmd.command_type));
    push(root.get(), make_string(cmd.command_id));
    push(root.get(), make_bytes(cmd.args));

    return root;
}

template <>
inline thewatcher::CommandMessage from_cbor<thewatcher::CommandMessage>(cbor_item_t* item)
{
    if (!cbor_isa_array(item) || cbor_array_size(item) != 3)
    {
        throw std::runtime_error("Invalid CommandMessage CBOR payload");
    }

    thewatcher::CommandMessage cmd;
    cmd.command_type = read_uint8(array_get(item, 0));
    cmd.command_id = read_string(array_get(item, 1));
    cmd.args = read_bytes(array_get(item, 2));

    return cmd;
}

template <>
inline CborPtr to_cbor(const thewatcher::AckMessage& ack)
{
    auto root = adopt(cbor_new_definite_array(3));

    push(root.get(), make_string(ack.command_id));
    push(root.get(), make_bool(ack.success));
    push(root.get(), make_string(ack.message));

    return root;
}

template <>
inline thewatcher::AckMessage from_cbor<thewatcher::AckMessage>(cbor_item_t* item)
{
    if (!cbor_isa_array(item) || cbor_array_size(item) != 3)
    {
        throw std::runtime_error("Invalid AckMessage CBOR payload");
    }

    thewatcher::AckMessage ack;
    ack.command_id = read_string(array_get(item, 0));
    ack.success = read_bool(array_get(item, 1));
    ack.message = read_string(array_get(item, 2));

    return ack;
}

template <>
inline CborPtr to_cbor(const thewatcher::SetIntervalArgs& a)
{
    auto root = adopt(cbor_new_definite_array(1));
    push(root.get(), make_int64(a.interval_seconds));
    return root;
}

template <>
inline thewatcher::SetIntervalArgs from_cbor<thewatcher::SetIntervalArgs>(cbor_item_t* item)
{
    if (!cbor_isa_array(item) || cbor_array_size(item) != 1)
    {
        throw std::runtime_error("Invalid SetIntervalArgs CBOR payload");
    }

    thewatcher::SetIntervalArgs a;
    a.interval_seconds = narrow_to_int(read_int64(array_get(item, 0)));
    return a;
}

template <>
inline CborPtr to_cbor(const thewatcher::SetProcessLimitArgs& a)
{
    auto root = adopt(cbor_new_definite_array(1));
    push(root.get(), make_int64(a.limit));
    return root;
}

template <>
inline thewatcher::SetProcessLimitArgs from_cbor<thewatcher::SetProcessLimitArgs>(cbor_item_t* item)
{
    if (!cbor_isa_array(item) || cbor_array_size(item) != 1)
    {
        throw std::runtime_error("Invalid SetProcessLimitArgs CBOR payload");
    }

    thewatcher::SetProcessLimitArgs a;
    a.limit = narrow_to_int(read_int64(array_get(item, 0)));
    return a;
}

template <>
inline CborPtr to_cbor(const thewatcher::ConfigUpdate& u)
{
    auto root = adopt(cbor_new_definite_array(4));

    push(root.get(), make_int64(u.interval_seconds));
    push(root.get(), make_int64(u.process_limit));

    auto collector = to_cbor(u.collector_config);
    push(root.get(), collector.release());
    push(root.get(), make_int64(u.heartbeat_interval_seconds));

    return root;
}

template <>
inline thewatcher::ConfigUpdate from_cbor<thewatcher::ConfigUpdate>(cbor_item_t* item)
{
    if (!cbor_isa_array(item) || cbor_array_size(item) < 3)
    {
        throw std::runtime_error("Invalid ConfigUpdate CBOR payload");
    }

    thewatcher::ConfigUpdate u;
    u.interval_seconds = narrow_to_int(read_int64(array_get(item, 0)));
    u.process_limit = narrow_to_int(read_int64(array_get(item, 1)));
    u.collector_config = from_cbor<thewatcher::CollectorConfig>(array_get(item, 2));
    if (cbor_array_size(item) >= 4)
        u.heartbeat_interval_seconds = narrow_to_int(read_int64(array_get(item, 3)));

    return u;
}

} // namespace thewatcher::proto::detail
