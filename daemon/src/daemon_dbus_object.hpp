// SPDX-License-Identifier: MIT
#pragma once

#include <string>

#include <systemd/sd-bus.h>
#include <systemd/sd-event.h>

#include "poll_waiter.hpp"
#include "proto/protocol.hpp"

namespace kas {

// The daemon's own script-facing D-Bus object at /daemon (interface name =
// service name, e.g. org.example.KwinApiServer):
//
//   log(level: s, msg: s) -> ()     (existing; moved here from DbusService)
//   poll(timeout: i) -> s           (phase 2)
//
// poll() returns daemon -> script control messages: client connect / disconnect
// notifications queued by the Server via enqueue_control(). Payload format
// (application layer, see PROTOCOL.md): {"event":"client_connected","id":N}
// / {"event":"client_disconnected","id":N}. The normal reply is a
// JSON-serialized array of the queued payloads; errors are JSON-encoded
// strings. Only one poll() may be pending per object.
class DaemonDBusObject {
public:
    DaemonDBusObject(sd_bus* bus, sd_event* event, const std::string& interface_name);
    ~DaemonDBusObject();

    DaemonDBusObject(const DaemonDBusObject&) = delete;
    DaemonDBusObject& operator=(const DaemonDBusObject&) = delete;

    // Queue one control message payload for the script; if a poll() is
    // pending, reply with the whole batch immediately. Drops + logs on a full
    // queue.
    void enqueue_control(const std::string& payload);

    static int method_log(sd_bus_message* message, void* userdata, sd_bus_error* error);
    static int method_poll(sd_bus_message* message, void* userdata, sd_bus_error* error);

private:
    int handle_poll(sd_bus_message* message);
    void maybe_reply_poll();

    sd_bus* bus_ = nullptr;
    sd_event* event_ = nullptr;
    std::string interface_name_;
    sd_bus_slot* slot_ = nullptr;
    proto::RxBuffer control_queue_;
    PollWaiter poll_;
};

} // namespace kas
