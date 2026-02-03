#include "pulse_router.hpp"

#include <core/audio.hpp>
#include <helpers/logger.hpp>

#include <pulse/pulseaudio.h>

#include <utility>

namespace wolf::core::audio {

PulseAudioRouter::PulseAudioRouter(std::shared_ptr<events::EventBusType> ev_bus,
                                   std::shared_ptr<audio::Server> pulse_server,
                                   std::string sink_prefix)
    : ev_bus_(std::move(ev_bus)), pulse_server_(std::move(pulse_server)), sink_prefix_(std::move(sink_prefix)) {}

PulseAudioRouter::~PulseAudioRouter() { stop(); }

void PulseAudioRouter::start() {
  bool expected = false;
  if (!started_.compare_exchange_strong(expected, true)) {
    return; // already started
  }

  if (!ev_bus_) {
    logs::log(logs::warning, "[PULSE_ROUTER] No event bus provided, cannot start");
    return;
  }
  if (!pulse_server_ || !pulse_server_->ctx) {
    logs::log(logs::warning, "[PULSE_ROUTER] No Pulse server/context provided, cannot start");
    return;
  }

  // Keep handler registrations alive by storing them as members.
  created_handler_ = ev_bus_->register_handler<immer::box<events::DockerContainerCreated>>(
      [this](const immer::box<events::DockerContainerCreated> &ev) { on_container_created(*ev); });

  stopped_handler_ = ev_bus_->register_handler<immer::box<events::DockerContainerStopped>>(
      [this](const immer::box<events::DockerContainerStopped> &ev) { on_container_stopped(*ev); });

  // Enable PA subscribe on the PA thread (or queued until connected).
  enable_pulse_subscribe();

  logs::log(logs::info, "[PULSE_ROUTER] Started");
}

void PulseAudioRouter::stop() {
  bool expected = true;
  if (!started_.compare_exchange_strong(expected, false)) {
    return; // not started
  }

  // Best-effort: unregister event handlers
  try {
    if (created_handler_) {
      created_handler_->unregister();
    }
  } catch (...) {
    // ignore
  }
  try {
    if (stopped_handler_) {
      stopped_handler_->unregister();
    }
  } catch (...) {
    // ignore
  }

  disable_pulse_subscribe();

  // Clear map
  {
    std::lock_guard<std::mutex> lk(map_mu_);
    host_to_session_.clear();
  }

  logs::log(logs::info, "[PULSE_ROUTER] Stopped");
}

void PulseAudioRouter::rescan() {
  if (!started_.load() || !pulse_server_ || !pulse_server_->ctx) return;

  audio::queue_op(pulse_server_, [this]() {
    if (!pulse_server_ || !pulse_server_->ctx) return;
    auto *c = pulse_server_->ctx;
    auto op = pa_context_get_sink_input_info_list(c, &PulseAudioRouter::pa_sink_input_info_cb, this);
    if (op) pa_operation_unref(op);
  });
}

void PulseAudioRouter::on_container_created(const events::DockerContainerCreated &ev) {
  if (ev.hostname.empty() || ev.session_id.empty()) return;

  {
    std::lock_guard<std::mutex> lk(map_mu_);
    host_to_session_[ev.hostname] = ev.session_id;
  }

  logs::log(logs::debug,
            "[PULSE_ROUTER] Map add host='{}' -> session='{}' (container_id='{}')",
            ev.hostname,
            ev.session_id,
            ev.container_id);

  // Fix race: if sink-input already exists, route it now.
  rescan();
}

void PulseAudioRouter::on_container_stopped(const events::DockerContainerStopped &ev) {
  if (ev.hostname.empty()) return;

  {
    std::lock_guard<std::mutex> lk(map_mu_);
    auto it = host_to_session_.find(ev.hostname);
    if (it != host_to_session_.end()) {
      // Optional: only erase if session matches (guards against hostname reuse)
      if (ev.session_id.empty() || it->second == ev.session_id) {
        host_to_session_.erase(it);
      }
    }
  }

  logs::log(logs::debug,
            "[PULSE_ROUTER] Map del host='{}' session='{}' (container_id='{}')",
            ev.hostname,
            ev.session_id,
            ev.container_id);
}

void PulseAudioRouter::enable_pulse_subscribe() {
  if (!pulse_server_ || !pulse_server_->ctx) return;

  audio::queue_op(pulse_server_, [this]() {
    if (!pulse_server_ || !pulse_server_->ctx) return;
    auto *c = pulse_server_->ctx;

    pa_context_set_subscribe_callback(c, &PulseAudioRouter::pa_subscribe_cb, this);

    auto op = pa_context_subscribe(c, PA_SUBSCRIPTION_MASK_SINK_INPUT, nullptr, nullptr);
    if (op) pa_operation_unref(op);

    // Also do an initial scan
    auto op2 = pa_context_get_sink_input_info_list(c, &PulseAudioRouter::pa_sink_input_info_cb, this);
    if (op2) pa_operation_unref(op2);

    logs::log(logs::debug, "[PULSE_ROUTER] Enabled PulseAudio sink-input subscription");
  });
}

void PulseAudioRouter::disable_pulse_subscribe() {
  if (!pulse_server_ || !pulse_server_->ctx) return;

  audio::queue_op(pulse_server_, [this]() {
    if (!pulse_server_ || !pulse_server_->ctx) return;
    auto *c = pulse_server_->ctx;

    // Unsubscribe: set callback null and subscribe mask 0 (best-effort)
    pa_context_set_subscribe_callback(c, nullptr, nullptr);
    auto op = pa_context_subscribe(c, static_cast<pa_subscription_mask_t>(0), nullptr, nullptr);
    if (op) pa_operation_unref(op);

    logs::log(logs::debug, "[PULSE_ROUTER] Disabled PulseAudio sink-input subscription");
  });
}

void PulseAudioRouter::pa_subscribe_cb(pa_context *c, unsigned int t, unsigned int idx, void *userdata) {
  auto *self = static_cast<PulseAudioRouter *>(userdata);
  if (!self || !self->started_.load()) return;

  const auto ev = static_cast<pa_subscription_event_type_t>(t);
  const auto facility = ev & PA_SUBSCRIPTION_EVENT_FACILITY_MASK;
  const auto type = ev & PA_SUBSCRIPTION_EVENT_TYPE_MASK;

  if (facility != PA_SUBSCRIPTION_EVENT_SINK_INPUT) return;
  if (type != PA_SUBSCRIPTION_EVENT_NEW && type != PA_SUBSCRIPTION_EVENT_CHANGE) return;

  // Query full info for this sink-input index
  auto op = pa_context_get_sink_input_info(c, idx, &PulseAudioRouter::pa_sink_input_info_cb, userdata);
  if (op) pa_operation_unref(op);
}

void PulseAudioRouter::pa_sink_input_info_cb(pa_context *c,
                                            const pa_sink_input_info *info,
                                            int eol,
                                            void *userdata) {
  if (eol || !info) return;
  auto *self = static_cast<PulseAudioRouter *>(userdata);
  if (!self || !self->started_.load()) return;

  self->route_sink_input_(c, info);
}

void PulseAudioRouter::route_sink_input_(pa_context *c, const pa_sink_input_info *info) {
  if (!info || !info->proplist) return;

  const char *host = pa_proplist_gets(info->proplist, "application.process.host");
  if (!host || host[0] == '\0') return;

  auto target = target_sink_for_host_(host);
  if (!target.has_value()) return;

  // Move the sink-input to the session's virtual sink.
  auto op = pa_context_move_sink_input_by_name(c, info->index, target->c_str(), nullptr, nullptr);
  if (op) pa_operation_unref(op);

  logs::log(logs::debug,
            "[PULSE_ROUTER] Move sink-input={} host='{}' -> '{}'",
            info->index,
            host,
            *target);
}

std::optional<std::string> PulseAudioRouter::target_sink_for_host_(std::string_view host) const {
  std::lock_guard<std::mutex> lk(map_mu_);
  auto it = host_to_session_.find(std::string(host));
  if (it == host_to_session_.end()) return std::nullopt;
  if (it->second.empty()) return std::nullopt;

  return sink_prefix_ + it->second;
}

} // namespace wolf::core::audio
```0
