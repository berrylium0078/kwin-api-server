// SPDX-License-Identifier: MIT
#include "dbus_util.hpp"

#include <cerrno>
#include <string>

namespace kas {

std::string json_string(const std::string& s) {
    std::string out = "\"";
    for (char ch : s) {
        switch (ch) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            out += ch;
            break;
        }
    }
    out += '"';
    return out;
}

namespace {

// Copy the queued messages out of `queue` as a JSON array string and drain.
std::string drain_queue(proto::RxBuffer* queue) {
    size_t len = 0;
    const char* data = queue->poll(&len);
    std::string out(data, len);
    queue->drain();
    return out;
}

// Send `reply` to `message` and notify `on_drained` (resume socket reads).
int reply_with(sd_bus_message* message, const std::string& reply,
               const std::function<void()>& on_drained) {
    if (on_drained) {
        on_drained();
    }
    return sd_bus_reply_method_return(message, "s", reply.c_str());
}

} // namespace

// A poll() that is already pending (from the timer/arrival path): answer it
// with the whole batch, if the queue has messages.
bool maybe_reply_pending_poll(PollWaiter* poll, proto::RxBuffer* queue,
                              const std::function<void()>& on_drained) {
    if (!poll->pending() || queue->messages() == 0) {
        return false;
    }
    std::string reply = drain_queue(queue);
    if (on_drained) {
        on_drained();
    }
    poll->reply(reply);
    return true;
}

int handle_poll_request(sd_event* event, sd_bus_message* message, proto::RxBuffer* queue,
                        PollWaiter* poll, const std::function<void()>& on_drained) {
    int timeout = 0;
    int r = sd_bus_message_read(message, "i", &timeout);
    if (r < 0) {
        return r;
    }
    if (timeout < 0 || timeout > kMaxPollTimeoutMs) {
        return sd_bus_reply_method_return(message, "s",
                                          json_string("error: timeout out of range (0..25000)").c_str());
    }
    if (poll->pending()) {
        // The same object supports only one pending poll() at a time.
        return sd_bus_reply_method_return(message, "s",
                                          json_string("error: a poll is already pending").c_str());
    }
    if (queue->messages() > 0) {
        // Messages are queued: return immediately with all of them.
        return reply_with(message, drain_queue(queue), on_drained);
    }
    if (timeout == 0) {
        // Never blocks: no messages right now -> empty array.
        return sd_bus_reply_method_return(message, "s", "[]");
    }
    r = poll->arm(event, message, timeout);
    if (r < 0) {
        return sd_bus_reply_method_return(message, "s",
                                          json_string("error: cannot arm poll timer").c_str());
    }
    return 1; // deferred; the reply comes later
}

} // namespace kas
