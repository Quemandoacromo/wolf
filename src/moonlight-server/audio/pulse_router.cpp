#include "pulse_router.hpp"

#include <core/audio.hpp>
#include <helpers/logger.hpp>

#include <pulse/pulseaudio.h>
#include <state/sessions.hpp>

#include <utility>

namespace wolf::core::audio {

immer::vector<immer::box<events::EventBusHandlers>>
setup_pulseaudio_router_handlers(const immer::box<state::AppState>& app_state,
                                 std::shared_ptr<PulseAudioRouterState> state) {
  immer::vector_transient<immer::box<events::EventBusHandlers>> handlers;

  if (!state) {
    logs::log(logs::warning, "[PULSE_ROUTER] No router state provided, cannot setup handlers");
    return handlers.persistent();
  }
  if (!app_state || !app_state->event_bus) {
    logs::log(logs::warning, "[PULSE_ROUTER] No event bus provided, cannot setup handlers");
    return handlers.persistent();
  }
  if (!state->pulse_server || !state->pulse_server->ctx) {
    logs::log(logs::warning, "[PULSE_ROUTER] No Pulse server/context provided, cannot setup handlers");
    return handlers.persistent();
  }

  // Register handlers (Wolf pattern: store registrations in `handlers`)
  handlers.push_back(app_state->event_bus->register_handler<immer::box<events::DockerContainerCreated>>(
      [state](const immer::box<events::DockerContainerCreated>& ev) {
        if (state) state->on_container_created(*ev);
      }));

  handlers.push_back(app_state->event_bus->register_handler<immer::box<events::DockerContainerStopped>>(
      [state](const immer::box<events::DockerContainerStopped>& ev) {
        if (state) state->on_container_stopped(*ev);
      }));

  // Subscribe to sink-input events and do an initial scan
  state->enable_pulse_subscribe();

  logs::log(logs::info, "[PULSE_ROUTER] Setup complete (handlers registered, PA subscribed)");
  return handlers.persistent();
}

// --- PulseAudioRouterState implementation ------------------------------------

void PulseAudioRouterState::rescan() {
  if (!pulse_server || !pulse_server->ctx) return;

  audio::queue_op(pulse_server, [this]() {
    if (!pulse_server || !pulse_server->ctx) return;
    auto* c = pulse_server->ctx;
    auto op = pa_context_get_sink_input_info_list(c, &PulseAudioRouterState::pa_sink_input_info_cb, this);
    if (op) pa_operation_unref(op);
  });
}

void PulseAudioRouterState::on_container_created(const events::DockerContainerCreated& ev) {
  if (ev.hostname.empty() || ev.session_id.empty()) return;

  {
    std::lock_guard<std::mutex> lk(map_mu);
    host_to_session[ev.hostname] = ev.session_id;
  }

  logs::log(logs::debug,
            "[PULSE_ROUTER] Map add host='{}' -> session='{}' (container_id='{}')",
            ev.hostname,
            ev.session_id,
            ev.container_id);

  // If sink-input already exists, route it now.
  rescan();
}

void PulseAudioRouterState::on_container_stopped(const events::DockerContainerStopped& ev) {
  if (ev.hostname.empty()) return;

  {
    std::lock_guard<std::mutex> lk(map_mu);
    auto it = host_to_session.find(ev.hostname);
    if (it != host_to_session.end()) {
      // Optional: only erase if session matches (guards against hostname reuse)
      if (ev.session_id.empty() || it->second == ev.session_id) {
        host_to_session.erase(it);
      }
    }
  }

  logs::log(logs::debug,
            "[PULSE_ROUTER] Map del host='{}' session='{}' (container_id='{}')",
            ev.hostname,
            ev.session_id,
            ev.container_id);
}

void PulseAudioRouterState::enable_pulse_subscribe() {
  if (!pulse_server || !pulse_server->ctx) return;

  audio::queue_op(pulse_server, [this]() {
    if (!pulse_server || !pulse_server->ctx) return;
    auto* c = pulse_server->ctx;

    pa_context_set_subscribe_callback(c, &PulseAudioRouterState::pa_subscribe_cb, this);

    auto op = pa_context_subscribe(c, PA_SUBSCRIPTION_MASK_SINK_INPUT, nullptr, nullptr);
    if (op) pa_operation_unref(op);

    // Initial scan
    auto op2 = pa_context_get_sink_input_info_list(c, &PulseAudioRouterState::pa_sink_input_info_cb, this);
    if (op2) pa_operation_unref(op2);

    logs::log(logs::debug, "[PULSE_ROUTER] Enabled PulseAudio sink-input subscription");
  });
}

void PulseAudioRouterState::pa_subscribe_cb(pa_context* c, unsigned int t, unsigned int idx, void* userdata) {
  auto* self = static_cast<PulseAudioRouterState*>(userdata);
  if (!self) return;

  const auto ev = static_cast<pa_subscription_event_type_t>(t);
  const auto facility = ev & PA_SUBSCRIPTION_EVENT_FACILITY_MASK;
  const auto type = ev & PA_SUBSCRIPTION_EVENT_TYPE_MASK;

  if (facility != PA_SUBSCRIPTION_EVENT_SINK_INPUT) return;
  if (type != PA_SUBSCRIPTION_EVENT_NEW && type != PA_SUBSCRIPTION_EVENT_CHANGE) return;

  // Query full info for this sink-input index
  auto op = pa_context_get_sink_input_info(c, idx, &PulseAudioRouterState::pa_sink_input_info_cb, userdata);
  if (op) pa_operation_unref(op);
}

void PulseAudioRouterState::pa_sink_input_info_cb(pa_context* c,
                                                 const pa_sink_input_info* info,
                                                 int eol,
                                                 void* userdata) {
  if (eol || !info) return;
  auto* self = static_cast<PulseAudioRouterState*>(userdata);
  if (!self) return;

  self->route_sink_input_(c, info);
}

void PulseAudioRouterState::route_sink_input_(pa_context* c, const pa_sink_input_info* info) {
  if (!info || !info->proplist) return;

  const char* host = pa_proplist_gets(info->proplist, "application.process.host");
  if (!host || host[0] == '\0') return;

  auto target = target_sink_for_host_(host);
  if (!target.has_value()) return;

  auto op = pa_context_move_sink_input_by_name(c, info->index, target->c_str(), nullptr, nullptr);
  if (op) pa_operation_unref(op);

  logs::log(logs::debug,
            "[PULSE_ROUTER] Move sink-input={} host='{}' -> '{}'",
            info->index,
            host,
            *target);
}

std::optional<std::string> PulseAudioRouterState::target_sink_for_host_(std::string_view host) const {
  std::lock_guard<std::mutex> lk(map_mu);
  auto it = host_to_session.find(std::string(host));
  if (it == host_to_session.end()) return std::nullopt;
  if (it->second.empty()) return std::nullopt;

  return sink_prefix + it->second;
}

} // namespace wolf::core::audio
