// SPDX-License-Identifier: MIT
#pragma once

#include <string>

#include <systemd/sd-bus.h>
#include <systemd/sd-event.h>

#include "poll_waiter.hpp"

namespace kas {

class ClientSession;

// The per-client D-Bus object at /cli{id}, interface org.example.KwinApiClient
// (fixed literal name, see PROTOCOL.md):
//
//   poll(timeout: i) -> s   messages from that client (client -> script)
//   push(msg: s) -> s       queue one message for that client's socket
//                           (script -> client); "" on success, error string
//                           on failure
//
// Registered/unregistered by ClientSession (its whole lifecycle is owned
// there). Shares the poll() semantics with DaemonDBusObject via dbus_util.
class ClientDBusObject {
public:
    ClientDBusObject(ClientSession& session, sd_bus* bus, sd_event* event, uint64_t id);
    ~ClientDBusObject();

    ClientDBusObject(const ClientDBusObject&) = delete;
    ClientDBusObject& operator=(const ClientDBusObject&) = delete;

    // Called by ClientSession after it read new messages from the socket: if
    // a poll() is pending, reply with the batch (and resume RX reads).
    void on_rx_messages();

    static int method_poll(sd_bus_message* message, void* userdata, sd_bus_error* error);
    static int method_push(sd_bus_message* message, void* userdata, sd_bus_error* error);

private:
    int handle_poll(sd_bus_message* message);
    int handle_push(sd_bus_message* message);

    ClientSession& session_;
    sd_bus* bus_ = nullptr;
    sd_event* event_ = nullptr;
    sd_bus_slot* slot_ = nullptr;
    std::string path_;
    PollWaiter poll_;
};

} // namespace kas
