#pragma once

#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <cbor.h>

namespace thewatcher::proto
{

enum class FrameType : uint8_t
{
    HEARTBEAT = 0x01,
    METRICS = 0x02,
    COMMAND = 0x03,
    COMMAND_ACK = 0x04,
    CONFIG_UPDATE = 0x05,
    ENROLL_REQUEST = 0x06,
    ENROLL_RESPONSE = 0x07,
    CONFIG_REQUEST = 0x08,
    LOG_MATCH = 0x09,
};

// Wire frame sent over every ZMQ message (CBOR-encoded).
struct Frame
{
    uint8_t type; // FrameType cast to uint8_t
    std::string agent_id;
    int64_t timestamp_ms = 0;
    std::vector<uint8_t> payload; // type-specific CBOR payload
};

// Enrollment sub-types (carried in Frame::payload).
struct EnrollRequest
{
    std::string agent_id;
    std::string hostname;
    std::string platform;
    std::string curve_public_key_z85; // 40-char z85
    // SEC-05: optional pre-shared enrollment token; required by the server
    // when ENROLLMENT_TOKEN is configured. Empty when unused.
    std::string enrollment_token;
};

struct EnrollResponse
{
    bool approved = false;
    std::string message;
    std::string server_public_key_z85;
    std::string server_public_key_fingerprint;
};

// Sent by the agent when a log file line matches a configured pattern.
struct LogMatch
{
    std::string indicator_name; // user-chosen indicator label
    std::string path;           // file that was tailed
    std::string matched_line;
    std::string severity; // "yellow", "amber", or "red"
};

// ── libcbor helpers ───────────────────────────────────────────────────────────

namespace detail
{

    struct CborDeleter
    {
        // Drops one libcbor reference. A null item is ignored.
        void operator()(cbor_item_t* item) const noexcept
        {
            if (item != nullptr)
            {
                cbor_decref(&item);
            }
        }
    };

    using CborPtr = std::unique_ptr<cbor_item_t, CborDeleter>;

    // Takes ownership of a libcbor item. Throws std::runtime_error on null,
    // which means libcbor failed to allocate.
    inline CborPtr adopt(cbor_item_t* item)
    {
        if (item == nullptr)
        {
            throw std::runtime_error("libcbor allocation failed");
        }
        return CborPtr(item);
    }

    // Appends value to array and releases the caller's reference to value.
    // Throws std::runtime_error on a null argument or if the append fails.
    inline void push(cbor_item_t* array, cbor_item_t* value)
    {
        if (array == nullptr || value == nullptr)
        {
            throw std::runtime_error("Invalid CBOR array/value");
        }

        if (!cbor_array_push(array, value))
        {
            cbor_decref(&value);
            throw std::runtime_error("Failed to append CBOR array item");
        }

        // cbor_array_push increments the child refcount; release our local ref.
        cbor_decref(&value);
    }

    // Returns element index of a CBOR array without taking a reference. Throws
    // std::runtime_error if array is not an array, index is out of range, or the
    // element is null.
    inline cbor_item_t* array_get(cbor_item_t* array, size_t index)
    {
        if (!cbor_isa_array(array) || cbor_array_size(array) <= index)
        {
            throw std::runtime_error("Invalid CBOR array shape");
        }

        cbor_item_t** items = cbor_array_handle(array);
        if (items == nullptr || items[index] == nullptr)
        {
            throw std::runtime_error("Invalid CBOR array item");
        }

        return items[index];
    }

    inline cbor_item_t* make_uint8(uint8_t value)
    {
        return cbor_build_uint8(value);
    }

    // Builds a CBOR integer from value. Negative values use the CBOR -1 - n
    // encoding. Returns null if libcbor fails to allocate.
    inline cbor_item_t* make_int64(int64_t value)
    {
        if (value >= 0)
        {
            return cbor_build_uint64(static_cast<uint64_t>(value));
        }

        // CBOR negative integer n is encoded as -1 - n.
        const uint64_t encoded = static_cast<uint64_t>(-(value + 1));
        return cbor_build_negint64(encoded);
    }

    inline cbor_item_t* make_string(const std::string& value)
    {
        return cbor_build_stringn(value.data(), value.size());
    }

    inline cbor_item_t* make_bytes(const std::vector<uint8_t>& value)
    {
        return cbor_build_bytestring(reinterpret_cast<cbor_data>(const_cast<uint8_t*>(value.data())), value.size());
    }

    inline cbor_item_t* make_bool(bool value)
    {
        return cbor_build_bool(value);
    }

    // libcbor's cbor_get_uint64 reads 8 raw bytes from item->data, which is only
    // valid for CBOR_INT_64 width items. For narrower widths it returns garbage
    // from adjacent memory. Always dispatch through this helper.
    inline uint64_t cbor_uint_value(const cbor_item_t* item)
    {
        switch (cbor_int_get_width(item))
        {
        case CBOR_INT_8:
            return cbor_get_uint8(const_cast<cbor_item_t*>(item));
        case CBOR_INT_16:
            return cbor_get_uint16(const_cast<cbor_item_t*>(item));
        case CBOR_INT_32:
            return cbor_get_uint32(const_cast<cbor_item_t*>(item));
        case CBOR_INT_64:
            return cbor_get_uint64(const_cast<cbor_item_t*>(item));
        }
        throw std::runtime_error("Unknown CBOR integer width");
    }

    // Reads an unsigned CBOR integer that fits in uint8_t. Throws
    // std::runtime_error for any other type or an out-of-range value.
    inline uint8_t read_uint8(cbor_item_t* item)
    {
        if (!cbor_isa_uint(item))
        {
            throw std::runtime_error("Expected CBOR unsigned integer");
        }

        const uint64_t value = cbor_uint_value(item);
        if (value > std::numeric_limits<uint8_t>::max())
        {
            throw std::runtime_error("CBOR integer does not fit uint8_t");
        }

        return static_cast<uint8_t>(value);
    }

    // Reads a CBOR unsigned or negative integer as int64_t. Throws
    // std::runtime_error for any other type or a value outside the int64_t range.
    inline int64_t read_int64(cbor_item_t* item)
    {
        if (cbor_isa_uint(item))
        {
            const uint64_t value = cbor_uint_value(item);
            if (value > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
            {
                throw std::runtime_error("CBOR integer does not fit int64_t");
            }
            return static_cast<int64_t>(value);
        }

        if (cbor_isa_negint(item))
        {
            const uint64_t encoded = cbor_uint_value(item);
            if (encoded > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
            {
                throw std::runtime_error("CBOR negative integer does not fit int64_t");
            }
            return -1 - static_cast<int64_t>(encoded);
        }

        throw std::runtime_error("Expected CBOR integer");
    }

    // Copies a definite-length CBOR text string. Throws std::runtime_error for any
    // other type, including indefinite-length strings.
    inline std::string read_string(cbor_item_t* item)
    {
        if (!cbor_isa_string(item) || !cbor_string_is_definite(item))
        {
            throw std::runtime_error("Expected definite CBOR string");
        }

        const auto* data = cbor_string_handle(item);
        const size_t size = cbor_string_length(item);

        return std::string(reinterpret_cast<const char*>(data), size);
    }

    // Copies a definite-length CBOR byte string. Throws std::runtime_error for any
    // other type, including indefinite-length strings.
    inline std::vector<uint8_t> read_bytes(cbor_item_t* item)
    {
        if (!cbor_isa_bytestring(item) || !cbor_bytestring_is_definite(item))
        {
            throw std::runtime_error("Expected definite CBOR byte string");
        }

        const auto* data = cbor_bytestring_handle(item);
        const size_t size = cbor_bytestring_length(item);

        return std::vector<uint8_t>(data, data + size);
    }

    // Reads a CBOR bool. Throws std::runtime_error for any other type.
    inline bool read_bool(cbor_item_t* item)
    {
        if (!cbor_is_bool(item))
        {
            throw std::runtime_error("Expected CBOR bool");
        }

        return cbor_get_bool(item);
    }

    // ── primary templates for to_cbor / from_cbor ────────────────────────────────
    // to_cbor and from_cbor are templates so that explicit specialisations defined
    // in any header are resolved via the primary at instantiation time. This
    // sidesteps the qualified-name two-phase-lookup gotcha that plain overloads
    // would hit when called via detail::to_cbor(obj) inside pack<T>.

    template <typename T>
    CborPtr to_cbor(const T&);

    template <typename T>
    T from_cbor(cbor_item_t*);

    template <>
    inline CborPtr to_cbor(const Frame& f)
    {
        auto root = adopt(cbor_new_definite_array(4));

        push(root.get(), make_uint8(f.type));
        push(root.get(), make_string(f.agent_id));
        push(root.get(), make_int64(f.timestamp_ms));
        push(root.get(), make_bytes(f.payload));

        return root;
    }

    template <>
    inline CborPtr to_cbor(const EnrollRequest& r)
    {
        auto root = adopt(cbor_new_definite_array(5));

        push(root.get(), make_string(r.agent_id));
        push(root.get(), make_string(r.hostname));
        push(root.get(), make_string(r.platform));
        push(root.get(), make_string(r.curve_public_key_z85));
        push(root.get(), make_string(r.enrollment_token));

        return root;
    }

    template <>
    inline CborPtr to_cbor(const EnrollResponse& r)
    {
        auto root = adopt(cbor_new_definite_array(4));

        push(root.get(), make_bool(r.approved));
        push(root.get(), make_string(r.message));
        push(root.get(), make_string(r.server_public_key_z85));
        push(root.get(), make_string(r.server_public_key_fingerprint));

        return root;
    }

    // Decodes a Frame from a 4-element CBOR array: type, agent_id, timestamp_ms,
    // payload. Throws std::runtime_error on a wrong shape or element type.
    template <>
    inline Frame from_cbor<Frame>(cbor_item_t* item)
    {
        if (!cbor_isa_array(item) || cbor_array_size(item) != 4)
        {
            throw std::runtime_error("Invalid Frame CBOR payload");
        }

        Frame f;
        f.type = read_uint8(array_get(item, 0));
        f.agent_id = read_string(array_get(item, 1));
        f.timestamp_ms = read_int64(array_get(item, 2));
        f.payload = read_bytes(array_get(item, 3));

        return f;
    }

    // Decodes an EnrollRequest from a 4- or 5-element CBOR array. The fifth
    // element is the SEC-05 token; without it enrollment_token stays empty.
    // Throws std::runtime_error on any other shape or element type.
    template <>
    inline EnrollRequest from_cbor<EnrollRequest>(cbor_item_t* item)
    {
        // 0.7.x agents send 4 elements (no enrollment token); current agents
        // send 5. Accept both so old agents can still enroll after a server
        // upgrade. A missing token decodes as empty and so fails the server's
        // ENROLLMENT_TOKEN comparison when one is configured.
        if (!cbor_isa_array(item))
        {
            throw std::runtime_error("Invalid EnrollRequest CBOR payload");
        }
        const auto element_count = cbor_array_size(item);
        if (element_count != 4 && element_count != 5)
        {
            throw std::runtime_error("Invalid EnrollRequest CBOR payload");
        }

        EnrollRequest r;
        r.agent_id = read_string(array_get(item, 0));
        r.hostname = read_string(array_get(item, 1));
        r.platform = read_string(array_get(item, 2));
        r.curve_public_key_z85 = read_string(array_get(item, 3));
        if (element_count == 5)
        {
            r.enrollment_token = read_string(array_get(item, 4));
        }

        return r;
    }

    // Decodes an EnrollResponse from a 4-element CBOR array. Throws
    // std::runtime_error on a wrong shape or element type.
    template <>
    inline EnrollResponse from_cbor<EnrollResponse>(cbor_item_t* item)
    {
        if (!cbor_isa_array(item) || cbor_array_size(item) != 4)
        {
            throw std::runtime_error("Invalid EnrollResponse CBOR payload");
        }

        EnrollResponse r;
        r.approved = read_bool(array_get(item, 0));
        r.message = read_string(array_get(item, 1));
        r.server_public_key_z85 = read_string(array_get(item, 2));
        r.server_public_key_fingerprint = read_string(array_get(item, 3));

        return r;
    }

    template <>
    inline CborPtr to_cbor(const LogMatch& m)
    {
        auto root = adopt(cbor_new_definite_array(4));
        push(root.get(), make_string(m.indicator_name));
        push(root.get(), make_string(m.path));
        push(root.get(), make_string(m.matched_line));
        push(root.get(), make_string(m.severity));
        return root;
    }

    // Decodes a LogMatch from a CBOR array of at least four strings; extra
    // elements are ignored. Throws std::runtime_error on a wrong shape or type.
    template <>
    inline LogMatch from_cbor<LogMatch>(cbor_item_t* item)
    {
        if (!cbor_isa_array(item) || cbor_array_size(item) < 4)
            throw std::runtime_error("Invalid LogMatch CBOR payload");
        LogMatch m;
        m.indicator_name = read_string(array_get(item, 0));
        m.path = read_string(array_get(item, 1));
        m.matched_line = read_string(array_get(item, 2));
        m.severity = read_string(array_get(item, 3));
        return m;
    }

} // namespace detail

// ── hostile-payload pre-validation (SEC-03) ────────────────────────────────────
//
// libcbor's streaming builder allocates space for a definite container's
// declared element count up front (count * 8 bytes for arrays) before it
// reads a single element, and cbor_decref frees children recursively. A
// 9-byte payload can therefore demand a multi-gigabyte allocation, and a
// few tens of KB of nested single-element arrays can overflow the stack
// when the decoded tree is released. This structural scan runs before
// cbor_load and rejects such shapes: nesting is depth-capped, and a
// definite container whose declared element count cannot possibly fit in
// the remaining bytes (each element costs at least one byte) is refused.

// Scans one item starting at p and advances p past it,
// recursing into containers and tags. Callers start with depth 1. Throws
// std::runtime_error on truncation, an invalid header, nesting beyond the
// depth cap, or a declared length that exceeds the remaining bytes.
inline void validate_cbor_item(const std::uint8_t*& p, const std::uint8_t* end, std::size_t depth)
{
    constexpr std::size_t max_depth = 8; // deepest legitimate TheWatcher frame is ~4 levels

    if (depth > max_depth)
        throw std::runtime_error("CBOR nesting too deep");
    if (p >= end)
        throw std::runtime_error("Truncated CBOR payload");

    const std::uint8_t head = *p++;
    const std::uint8_t major_type = head >> 5;
    const std::uint8_t info = head & 0x1F;

    std::uint64_t value = 0;
    if (info <= 23)
    {
        value = info;
    }
    else if (info <= 27) // 24..27: 1/2/4/8-byte extended value
    {
        const std::size_t len = std::size_t{1} << (info - 24);
        if (static_cast<std::size_t>(end - p) < len)
            throw std::runtime_error("Truncated CBOR length");
        for (std::size_t i = 0; i < len; ++i)
            value = (value << 8) | *p++;
    }
    else if (!(info == 31 && (major_type == 2 || major_type == 3 || major_type == 4 || major_type == 5)))
    {
        // 28-30 are reserved; 31 (indefinite) is only valid for the
        // string and container types handled below.
        throw std::runtime_error("Invalid CBOR additional info");
    }

    switch (major_type)
    {
    case 0: // unsigned integer
    case 1: // negative integer
    case 7: // simple value / float
        return;
    case 2: // byte string
    case 3: // text string
        if (info == 31)
        {
            // Indefinite-length string: definite-length chunks of the same
            // major type, terminated by a break byte.
            for (;;)
            {
                if (p >= end)
                    throw std::runtime_error("Truncated CBOR string chunk");
                if (*p == 0xFF)
                {
                    ++p;
                    return;
                }
                const std::uint8_t chunk_head = *p;
                if ((chunk_head >> 5) != major_type || (chunk_head & 0x1F) == 31)
                    throw std::runtime_error("Invalid CBOR string chunk");
                validate_cbor_item(p, end, depth); // definite-length chunk
            }
        }
        if (value > static_cast<std::uint64_t>(end - p))
            throw std::runtime_error("CBOR string length exceeds payload");
        p += static_cast<std::size_t>(value);
        return;
    case 4: // array
    case 5: // map (each entry is a key + a value)
    {
        const std::uint64_t items_per_entry = (major_type == 5) ? 2 : 1;
        if (info == 31)
        {
            // Indefinite-length container terminated by a break byte.
            for (;;)
            {
                if (p >= end)
                    throw std::runtime_error("Truncated CBOR container");
                if (*p == 0xFF)
                {
                    ++p;
                    return;
                }
                validate_cbor_item(p, end, depth + 1);
            }
        }
        // The amplification vector: a declared count that cannot fit in
        // the remaining bytes is rejected before libcbor ever allocates.
        if (value > static_cast<std::uint64_t>(end - p) / items_per_entry)
            throw std::runtime_error("CBOR container count exceeds payload");
        for (std::uint64_t i = 0; i < value * items_per_entry; ++i)
            validate_cbor_item(p, end, depth + 1);
        return;
    }
    case 6: // tag: validate the tagged item
        validate_cbor_item(p, end, depth + 1);
        return;
    default:
        throw std::runtime_error("Invalid CBOR major type");
    }
}

// Structural pre-validation of a whole CBOR payload. Throws
// std::runtime_error on any hostile or malformed shape.
inline void validate_cbor_structure(const void* data, std::size_t size)
{
    const auto* p = static_cast<const std::uint8_t*>(data);
    const auto* end = p + size;
    validate_cbor_item(p, end, 1);
    if (p != end)
        throw std::runtime_error("Trailing bytes after CBOR payload");
}

// ── public helpers ────────────────────────────────────────────────────────────

// Serializes obj to CBOR bytes through detail::to_cbor. Throws
// std::runtime_error if libcbor fails to serialize.
template <typename T>
std::vector<uint8_t> pack(const T& obj)
{
    auto root = detail::to_cbor(obj);

    unsigned char* buffer = nullptr;
    size_t buffer_size = 0;

    cbor_serialize_alloc(root.get(), &buffer, &buffer_size);
    if (buffer == nullptr || buffer_size == 0)
    {
        throw std::runtime_error("Failed to serialize CBOR");
    }

    std::vector<uint8_t> out(buffer, buffer + buffer_size);
    std::free(buffer);

    return out;
}

// Decodes a T from a CBOR buffer. Throws std::runtime_error on an empty
// buffer, a payload that fails SEC-03 structural validation, a parse error,
// trailing bytes, or a shape that does not match T.
template <typename T>
T unpack(const void* data, std::size_t size);

template <typename T>
T unpack(const std::vector<uint8_t>& data)
{
    return unpack<T>(data.data(), data.size());
}

// Decodes a T from a CBOR buffer. Throws std::runtime_error on an empty buffer,
// a payload that fails structural validation, a parse error, trailing bytes, or
// a shape that does not match T.
template <typename T>
T unpack(const void* data, std::size_t size)
{
    if (data == nullptr || size == 0)
    {
        throw std::runtime_error("Cannot decode empty CBOR buffer");
    }

    // SEC-03: reject hostile declared lengths / nesting before libcbor's
    // builder can act on them.
    validate_cbor_structure(data, size);

    cbor_load_result result{};
    auto root = detail::adopt(cbor_load(
        static_cast<cbor_data>(const_cast<std::uint8_t*>(static_cast<const std::uint8_t*>(data))), size, &result));

    if (result.error.code != CBOR_ERR_NONE)
    {
        throw std::runtime_error("Failed to parse CBOR payload");
    }

    if (result.read != size)
    {
        throw std::runtime_error("Trailing bytes after CBOR payload");
    }

    return detail::from_cbor<T>(root.get());
}

inline std::vector<uint8_t> encode_frame(const Frame& f)
{
    return pack(f);
}

inline Frame decode_frame(const void* data, std::size_t size)
{
    return unpack<Frame>(data, size);
}

} // namespace thewatcher::proto