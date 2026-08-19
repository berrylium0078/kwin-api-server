// SPDX-License-Identifier: MIT
#include "poll_waiter.hpp"

#include <cerrno>

namespace kas {

PollWaiter::~PollWaiter() {
    cancel();
}

int PollWaiter::arm(sd_event* event, sd_bus_message* request, int timeout_ms) {
    if (!event || !request || timeout_ms <= 0) {
        return -EINVAL;
    }
    if (pending()) {
        return -EBUSY;
    }
    event_ = event;
    msg_ = sd_bus_message_ref(request);

    uint64_t now = 0;
    int r = sd_event_now(event, CLOCK_MONOTONIC, &now);
    if (r < 0) {
        cancel();
        return r;
    }
    uint64_t usec = now + static_cast<uint64_t>(timeout_ms) * 1000ULL;
    r = sd_event_add_time(event, &timer_, CLOCK_MONOTONIC, usec, 0, on_timeout, this);
    if (r < 0) {
        cancel();
        return r;
    }
    return 0;
}

void PollWaiter::reply(const std::string& payload) {
    if (!msg_) {
        return;
    }
    // The message reference is ours (taken in arm()); sd_bus_reply_method_return
    // copies the payload into the outgoing message, so replying from outside the
    // original handler is fine.
    sd_bus_message* msg = msg_;
    msg_ = nullptr;
    if (timer_) {
        // Safe to unref from within the timer's own callback; sd-event defers
        // the actual free until the callback returns.
        sd_event_source_set_enabled(timer_, SD_EVENT_OFF);
        sd_event_source_unref(timer_);
        timer_ = nullptr;
    }
    sd_bus_reply_method_return(msg, "s", payload.c_str());
    sd_bus_message_unref(msg);
    event_ = nullptr;
}

void PollWaiter::reply_empty() {
    reply("[]");
}

void PollWaiter::cancel() {
    if (timer_) {
        sd_event_source_set_enabled(timer_, SD_EVENT_OFF);
        sd_event_source_unref(timer_);
        timer_ = nullptr;
    }
    if (msg_) {
        sd_bus_message_unref(msg_);
        msg_ = nullptr;
    }
    event_ = nullptr;
}

int PollWaiter::on_timeout(sd_event_source* /*source*/, uint64_t /*usec*/, void* userdata) {
    auto* self = static_cast<PollWaiter*>(userdata);
    self->reply_empty();
    return 0;
}

} // namespace kas
