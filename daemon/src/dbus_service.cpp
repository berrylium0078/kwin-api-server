// SPDX-License-Identifier: MIT
#include "dbus_service.hpp"

#include <cstring>

#include "config.hpp"
#include "log.hpp"

namespace kas {

namespace {

constexpr uint64_t kMethodCallTimeoutUsec = 30ULL * 1000 * 1000; // 30 s

// Interface served on the daemon's own object path. The interface name is the
// same dotted string as the service name, so both are configurable through
// KWIN_API_SERVICE_NAME.
const sd_bus_vtable kStatusVtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Status", "", "s", DbusService::method_status, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END,
};

} // namespace

DbusService::DbusService(const Config& config)
    : service_name_(config.service_name), object_path_(object_path_for_name(config.service_name)),
      status_("service=" + config.service_name) {}

DbusService::~DbusService() { close(); }

int DbusService::open() {
    if (bus_) {
        return 0;
    }
    int r = sd_bus_open_user(&bus_);
    if (r < 0) {
        log_error("sd_bus_open_user: " + std::string(std::strerror(-r)));
        return r;
    }
    // Never exit the process when the session bus goes away (it may restart);
    // NameLost is handled explicitly below.
    sd_bus_set_exit_on_disconnect(bus_, 0);
    // By default sd-bus closes the connection when the event loop exits
    // (close_on_exit). We want to keep it usable after the loop has stopped —
    // the shutdown path calls org.kde.kwin.Scripting.unloadScript() after
    // sd_event_loop() returns — so disable that behaviour.
    sd_bus_set_close_on_exit(bus_, 0);
    // loadScript can be slow while KWin is starting up.
    sd_bus_set_method_call_timeout(bus_, kMethodCallTimeoutUsec);
    return 0;
}

int DbusService::request_name() {
    int r = sd_bus_request_name(bus_, service_name_.c_str(), 0);
    if (r < 0) {
        log_error("cannot acquire D-Bus name " + service_name_ + ": " +
                  std::string(std::strerror(-r)));
        return r;
    }
    // If the name is later lost (stolen / bus restart), shut down.
    std::string match = "type='signal',sender='org.freedesktop.DBus',"
                        "interface='org.freedesktop.DBus',member='NameLost',"
                        "path='/org/freedesktop/DBus',arg0='" +
                        service_name_ + "'";
    r = sd_bus_add_match(bus_, nullptr, match.c_str(), on_name_lost, this);
    if (r < 0) {
        log_warn("sd_bus_add_match(NameLost): " + std::string(std::strerror(-r)));
        // non-fatal
    }
    return 0;
}

int DbusService::attach(sd_event* event) {
    event_ = event;
    int r = sd_bus_attach_event(bus_, event, 0);
    if (r < 0) {
        log_error("sd_bus_attach_event: " + std::string(std::strerror(-r)));
        return r;
    }
    r = sd_bus_add_object_vtable(bus_, nullptr, object_path_.c_str(),
                                 service_name_.c_str(), kStatusVtable, this);
    if (r < 0) {
        log_error("sd_bus_add_object_vtable: " + std::string(std::strerror(-r)));
        return r;
    }
    return 0;
}

void DbusService::detach() {
    if (bus_ && event_) {
        sd_bus_detach_event(bus_);
        event_ = nullptr;
    }
}

void DbusService::close() {
    if (bus_) {
        if (event_) {
            sd_bus_detach_event(bus_);
            event_ = nullptr;
        }
        sd_bus_close(bus_); // releases the name
        sd_bus_unref(bus_);
        bus_ = nullptr;
    }
}

int DbusService::method_status(sd_bus_message* message, void* userdata, sd_bus_error* /*error*/) {
    auto* self = static_cast<DbusService*>(userdata);
    return sd_bus_reply_method_return(message, "s", self->status_.c_str());
}

int DbusService::on_name_lost(sd_bus_message* message, void* userdata, sd_bus_error* /*error*/) {
    auto* self = static_cast<DbusService*>(userdata);
    const char* name = nullptr;
    sd_bus_message_read(message, "s", &name);
    if (name && self->service_name_ == name) {
        log_warn("D-Bus name lost: " + self->service_name_ + ", shutting down");
        if (self->event_) {
            // Detach first: leaving the loop with the bus attached marks the
            // bus closed, which would break the post-loop unloadScript call.
            self->detach();
            sd_event_exit(self->event_, 1);
        }
    }
    return 0;
}

} // namespace kas
