// SPDX-License-Identifier: MIT
#pragma once

#include <functional>
#include <string>

#include <systemd/sd-bus.h>
#include <systemd/sd-event.h>

#include "poll_waiter.hpp"
#include "proto/protocol.hpp"

namespace kas {

// Maximum poll() timeout (milliseconds), per the frozen protocol.
constexpr int kMaxPollTimeoutMs = 25'000;

// JSON-encode `s` as a JSON string literal (used for error replies, which the
// script parses as a string; see PROTOCOL.md).
std::string json_string(const std::string& s);

// Handle one poll(timeout) -> s method call against a message queue.
//
// Semantics (PROTOCOL.md): if messages are queued, reply immediately with all
// of them (JSON array string) and drain the queue; otherwise block until the
// first message arrives, replying with the batch, or return the empty array
// after `timeout` ms. Only one pending poll per queue. Timeout range
// 0..25000 ms; out-of-range values and concurrent polls are errors (JSON
// encoded strings). timeout == 0 never blocks.
//
// Returns 0 if a reply was sent, 1 if the reply was deferred (the caller's
// method handler must return 1 to sd-bus), or a negative errno on failure.
// `on_drained` is invoked after the queue was drained (e.g. to resume socket
// reads); it may be empty.
int handle_poll_request(sd_event* event, sd_bus_message* message, proto::RxBuffer* queue,
                        PollWaiter* poll, const std::function<void()>& on_drained);

// If a poll is pending and the queue has messages: reply with the whole batch
// and drain. Returns true if a reply was sent.
bool maybe_reply_pending_poll(PollWaiter* poll, proto::RxBuffer* queue,
                              const std::function<void()>& on_drained);

} // namespace kas
