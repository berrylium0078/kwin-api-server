// SPDX-License-Identifier: MIT
#include "client_dbus_object.hpp"

#include <cerrno>
#include <cstring>
#include <string>

#include "client_session.hpp"
#include "dbus_util.hpp"
#include "log.hpp"

namespace kas {

namespace {

constexpr const char* kClientInterface = "org.example.KwinApiClient";

const sd_bus_vtable kClientVtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("poll", "i", "s", ClientDBusObject::method_poll, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("push", "s", "s", ClientDBusObject::method_push, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END,
};

} // namespace

ClientDBusObject::ClientDBusObject(ClientSession& session, sd_bus* bus, sd_event* event,
                                   uint64_t id)
    : session_(session), bus_(bus), event_(event), path_("/cli" + std::to_string(id)) {
    int r = sd_bus_add_object_vtable(bus_, &slot_, path_.c_str(), kClientInterface, kClientVtable,
                                     this);
    if (r < 0) {
        log_error("sd_bus_add_object_vtable(" + path_ + "): " + std::string(std::strerror(-r)));
    }
}

ClientDBusObject::~ClientDBusObject() {
    poll_.cancel();
    if (slot_) {
        sd_bus_slot_unref(slot_);
    }
}

void ClientDBusObject::on_rx_messages() {
    // The session just appended messages to the RX buffer; if a poll() is
    // waiting, deliver the whole batch and resume reads (the drain frees RX
    // buffer space, which is what un-pauses the socket).
    maybe_reply_pending_poll(&poll_, &session_.client().rx(), [this] { session_.on_rx_drained(); });
}

int ClientDBusObject::method_poll(sd_bus_message* message, void* userdata, sd_bus_error* /*error*/) {
    auto* self = static_cast<ClientDBusObject*>(userdata);
    return self->handle_poll(message);
}

int ClientDBusObject::handle_poll(sd_bus_message* message) {
    return handle_poll_request(event_, message, &session_.client().rx(), &poll_,
                               [this] { session_.on_rx_drained(); });
}

int ClientDBusObject::method_push(sd_bus_message* message, void* userdata, sd_bus_error* /*error*/) {
    auto* self = static_cast<ClientDBusObject*>(userdata);
    return self->handle_push(message);
}

int ClientDBusObject::handle_push(sd_bus_message* message) {
    const char* msg = nullptr;
    int r = sd_bus_message_read(message, "s", &msg);
    if (r < 0) {
        return r;
    }
    if (!msg) {
        return -EINVAL;
    }
    int pr = session_.push(msg, std::strlen(msg));
    if (pr == 0) {
        return sd_bus_reply_method_return(message, "s", "");
    }
    // Failure: the message was ignored. Error strings are plain text (the
    // script distinguishes push success/failure by emptiness of the reply).
    const char* why = pr == -EMSGSIZE ? "message exceeds 1 MB"
                      : pr == -ENOBUFS ? "write buffer full"
                      : pr == -ENOTCONN ? "client gone"
                                        : "cannot queue message";
    return sd_bus_reply_method_return(message, "s", why);
}

} // namespace kas
