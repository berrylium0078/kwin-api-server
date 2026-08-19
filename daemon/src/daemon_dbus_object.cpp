// SPDX-License-Identifier: MIT
#include "daemon_dbus_object.hpp"

#include <cerrno>
#include <cstring>
#include <string>

#include "dbus_util.hpp"
#include "log.hpp"

namespace kas {

namespace {

constexpr const char* kDaemonObjectPath = "/daemon";

const sd_bus_vtable kDaemonVtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("log", "ss", "", DaemonDBusObject::method_log, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("poll", "i", "s", DaemonDBusObject::method_poll, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END,
};

} // namespace

DaemonDBusObject::DaemonDBusObject(sd_bus* bus, sd_event* event, const std::string& interface_name)
    : bus_(bus), event_(event), interface_name_(interface_name), control_queue_(128 * 1024) {
    int r = sd_bus_add_object_vtable(bus_, &slot_, kDaemonObjectPath, interface_name_.c_str(),
                                     kDaemonVtable, this);
    if (r < 0) {
        log_error("sd_bus_add_object_vtable(/daemon): " + std::string(std::strerror(-r)));
    }
}

DaemonDBusObject::~DaemonDBusObject() {
    poll_.cancel();
    if (slot_) {
        sd_bus_slot_unref(slot_);
    }
}

void DaemonDBusObject::enqueue_control(const std::string& payload) {
    int r = control_queue_.append(payload.data(), payload.size());
    if (r < 0) {
        log_warn("daemon control queue full, dropping message: " + payload);
        return;
    }
    maybe_reply_poll();
}

void DaemonDBusObject::maybe_reply_poll() {
    maybe_reply_pending_poll(&poll_, &control_queue_, nullptr);
}

int DaemonDBusObject::method_log(sd_bus_message* message, void* userdata, sd_bus_error* /*error*/) {
    auto* self = static_cast<DaemonDBusObject*>(userdata);
    (void)self; // log_* are free functions
    const char* level = nullptr;
    const char* msg = nullptr;
    int r = sd_bus_message_read(message, "ss", &level, &msg);
    if (r < 0) {
        return r;
    }
    if (!level || !msg) {
        return -EINVAL;
    }
    // Tag the line so journal entries coming from the KWin script can be told
    // apart from the daemon's own messages.
    const std::string text = "[script] " + std::string(msg);
    if (std::strcmp(level, "debug") == 0) {
        log_debug(text);
    } else if (std::strcmp(level, "warn") == 0) {
        log_warn(text);
    } else if (std::strcmp(level, "error") == 0) {
        log_error(text);
    } else {
        log_info(text); // "info" and anything unrecognized
    }
    return sd_bus_reply_method_return(message, "");
}

int DaemonDBusObject::method_poll(sd_bus_message* message, void* userdata, sd_bus_error* /*error*/) {
    auto* self = static_cast<DaemonDBusObject*>(userdata);
    return self->handle_poll(message);
}

int DaemonDBusObject::handle_poll(sd_bus_message* message) {
    return handle_poll_request(event_, message, &control_queue_, &poll_, nullptr);
}

} // namespace kas
