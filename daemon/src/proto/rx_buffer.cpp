// SPDX-License-Identifier: MIT
#include "protocol.hpp"

#include <cerrno>
#include <cstring>
#include <stdexcept>

namespace kas {
namespace proto {

// Layout: data_[0] == '[' always; each complete message is stored as its
// payload bytes followed by ','. Example with two messages "a" and "b":
//
//     data = "[a,b,"  len = 5  complete_len = 5  messages = 2
//
// poll() turns the trailing ',' into ']' in place and returns the prefix
// [0, complete_len). A payload may still be streaming in when poll() is
// called (reads and D-Bus polls interleave on the daemon's loop): bytes of
// the in-flight payload live in [complete_len, len) and are NOT part of the
// poll() result. drain() then drops the complete prefix and keeps the tail.

RxBuffer::RxBuffer(size_t capacity) : capacity_(capacity) {
    if (capacity < 2) {
        throw std::invalid_argument("RxBuffer: capacity too small");
    }
    data_ = new char[capacity];
    data_[0] = '[';
}

RxBuffer::~RxBuffer() {
    delete[] data_;
}

bool RxBuffer::can_fit(size_t payload_len) const {
    // payload bytes + the trailing ',' separator
    return len_ + payload_len + 1 <= capacity_;
}

int RxBuffer::append(const char* payload, size_t len) {
    if (!can_fit(len)) {
        return -ENOBUFS;
    }
    std::memcpy(data_ + len_, payload, len);
    len_ += len;
    message_complete();
    return 0;
}

char* RxBuffer::tail() {
    return data_ + len_;
}

void RxBuffer::advance(size_t n) {
    len_ += n;
}

void RxBuffer::message_complete() {
    // The payload was written at [len - payload_len, len); seal it with ','.
    data_[len_] = ',';
    len_ += 1;
    complete_len_ = len_;
    messages_ += 1;
}

const char* RxBuffer::poll(size_t* out_len) {
    if (messages_ == 0) {
        static const char kEmpty[] = "[]";
        *out_len = sizeof(kEmpty) - 1;
        return kEmpty;
    }
    // The last complete message ends with ','; make it ']' to form a valid
    // JSON array. Contract: the returned pointer is only valid until the next
    // mutation — callers must copy the result and call drain() before any
    // further append or read.
    data_[complete_len_ - 1] = ']';
    *out_len = complete_len_;
    return data_;
}

void RxBuffer::drain() {
    if (messages_ == 0) {
        return;
    }
    // Keep any in-flight partial payload; shift it to right after the '['.
    const size_t tail = len_ - complete_len_;
    if (tail > 0) {
        std::memmove(data_ + 1, data_ + complete_len_, tail);
    }
    len_ = 1 + tail;
    complete_len_ = 1;
    messages_ = 0;
}

} // namespace proto
} // namespace kas
