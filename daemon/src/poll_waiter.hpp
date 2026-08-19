// SPDX-License-Identifier: MIT
#pragma once

#include <string>

#include <systemd/sd-bus.h>
#include <systemd/sd-event.h>

namespace kas {

// Supports the protocol's blocking poll() semantics on the D-Bus side.
//
// A poll() method handler that finds its message queue empty must not block
// the event loop: it hands the request to a PollWaiter (returning a positive
// integer from the handler, which tells sd-bus the reply will be generated
// asynchronously) and later sends the reply either when a message arrives
// (reply(batch)) or when the timeout expires (reply_empty()).
//
// At most one poll() may be pending per waiter; the D-Bus object checks
// pending() and rejects a second one.
class PollWaiter {
public:
    PollWaiter() = default;
    ~PollWaiter();

    PollWaiter(const PollWaiter&) = delete;
    PollWaiter& operator=(const PollWaiter&) = delete;

    // Whether a poll() request is currently waiting for a reply.
    bool pending() const { return msg_ != nullptr; }

    // Take over the request: keep a reference to the message and arm a
    // timeout timer (timeout_ms > 0). Returns 0 / negative errno.
    int arm(sd_event* event, sd_bus_message* request, int timeout_ms);

    // Send the reply payload to the pending caller and clear the waiter.
    void reply(const std::string& payload);
    // Send the empty-array reply (poll timeout).
    void reply_empty();

    // Drop the pending request without replying (object teardown).
    void cancel();

private:
    static int on_timeout(sd_event_source* source, uint64_t usec, void* userdata);

    sd_event* event_ = nullptr;
    sd_bus_message* msg_ = nullptr; // referenced while pending
    sd_event_source* timer_ = nullptr;
};

} // namespace kas
