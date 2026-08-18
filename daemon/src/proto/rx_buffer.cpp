// SPDX-License-Identifier: MIT
#include "protocol.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>

namespace kas {
namespace proto {

// Layout: data[0] == '[' always; each complete message is stored as its
// payload bytes followed by ','. Example with two messages "a" and "b":
//
//     data = "[a,b,"  len = 5  complete_len = 5  messages = 2
//
// poll() turns the trailing ',' into ']' in place and returns the prefix
// [0, complete_len). A payload may still be streaming in when poll() is
// called (reads and D-Bus polls interleave on the daemon's loop): bytes of
// the in-flight payload live in [complete_len, len) and are NOT part of the
// poll() result. drain() then drops the complete prefix and keeps the tail.

int rx_buffer_init(rx_buffer* rx, size_t capacity) {
    if (!rx || capacity < 2) {
        return -EINVAL;
    }
    rx->data = static_cast<char*>(std::malloc(capacity));
    if (!rx->data) {
        return -ENOMEM;
    }
    rx->data[0] = '[';
    rx->len = 1;
    rx->capacity = capacity;
    rx->messages = 0;
    rx->complete_len = 1;
    return 0;
}

void rx_buffer_free(rx_buffer* rx) {
    if (!rx) {
        return;
    }
    std::free(rx->data);
    rx->data = nullptr;
    rx->len = 0;
    rx->capacity = 0;
    rx->messages = 0;
    rx->complete_len = 0;
}

bool rx_buffer_can_fit(const rx_buffer* rx, size_t payload_len) {
    // payload bytes + the trailing ',' separator
    return rx->len + payload_len + 1 <= rx->capacity;
}

int rx_buffer_append(rx_buffer* rx, const char* payload, size_t len) {
    if (!rx_buffer_can_fit(rx, len)) {
        return -ENOBUFS;
    }
    std::memcpy(rx->data + rx->len, payload, len);
    rx->len += len;
    rx_buffer_message_complete(rx);
    return 0;
}

void rx_buffer_message_complete(rx_buffer* rx) {
    // The payload was written at [len - payload_len, len); seal it with ','.
    rx->data[rx->len] = ',';
    rx->len += 1;
    rx->complete_len = rx->len;
    rx->messages += 1;
}

const char* rx_buffer_poll(rx_buffer* rx, size_t* out_len) {
    if (rx->messages == 0) {
        static const char kEmpty[] = "[]";
        *out_len = sizeof(kEmpty) - 1;
        return kEmpty;
    }
    // The last complete message ends with ','; make it ']' to form a valid
    // JSON array. Contract: the returned pointer is only valid until the next
    // mutation — callers must copy the result and call drain() before any
    // further append or read.
    rx->data[rx->complete_len - 1] = ']';
    *out_len = rx->complete_len;
    return rx->data;
}

void rx_buffer_drain(rx_buffer* rx) {
    if (rx->messages == 0) {
        return;
    }
    // Keep any in-flight partial payload; shift it to right after the '['.
    const size_t tail = rx->len - rx->complete_len;
    if (tail > 0) {
        std::memmove(rx->data + 1, rx->data + rx->complete_len, tail);
    }
    rx->len = 1 + tail;
    rx->complete_len = 1;
    rx->messages = 0;
}

} // namespace proto
} // namespace kas
