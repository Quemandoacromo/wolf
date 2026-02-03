#pragma once

#include <events/events.hpp>
#include <immer/box.hpp>

#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

// Forward-declare PulseAudio types (include <pulse/pulseaudio.h> in the .cpp)
struct pa_context;
struct pa_operation;
struct pa_sink_input_info;

namespace wolf::core::audio {

// Forward-declare the Wolf Pulse server handle (defined in core/audio.*)
struct Server;

/**
 * Routes PulseAudio sink-inputs to the correct per-session virtual sink.
 *
 * Mapping source:
 *  - events::DockerContainerCreated / events::DockerContainerStopped
 *    (hostname -> session_id)
 *
 * Routing rule:
 *  - For any sink-input where application.process.host == hostname:
 *      move sink-input to sink "virtual_sink_<session_id>"
 *
 * Intended to be created once (when the audio server is enabled) and kept alive
 * for the lifetime of the Wolf process.
 */
class PulseAudioRouter {
public:
  PulseAudioRouter(std::shared_ptr<events::EventBusType> ev_bus,
                   std::shared_ptr<audio::Server> pulse_server,
                   std::string sink_prefix = "virtual_sink_");

  ~PulseAudioRouter();

  PulseAudioRouter(const PulseAudioRouter &) = delete;
  PulseAudioRouter &operator=(const PulseAudioRouter &) = delete;

  /// Registers event-bus handlers and enables PulseAudio sink-input subscription.
  /// Safe to call multiple times; subsequent calls are no-ops.
  void start();

  /// Unregisters handlers and disables routing (best-effort).
  /// Safe to call multiple times.
  void stop();

  /// Force a rescan of existing sink-inputs and route anything that matches the map.
  void rescan();

private:
  // ---- Wolf event handlers ----
  void on_container_created(const events::DockerContainerCreated &ev);
  void on_container_stopped(const events::DockerContainerStopped &ev);

  // ---- PulseAudio subscription ----
  void enable_pulse_subscribe();
  void disable_pulse_subscribe();

  // Callback for pa_context_subscribe events
  static void pa_subscribe_cb(pa_context *c, unsigned int t, unsigned int idx, void *userdata);

  // Callback for pa_context_get_sink_input_info (and list)
  static void pa_sink_input_info_cb(pa_context *c, const pa_sink_input_info *info, int eol, void *userdata);

  // Core routing decision for one sink-input
  void route_sink_input_(pa_context *c, const pa_sink_input_info *info);

  // Compute target sink for a given host (container hostname)
  std::optional<std::string> target_sink_for_host_(std::string_view host) const;

private:
  std::shared_ptr<events::EventBusType> ev_bus_;
  std::shared_ptr<audio::Server> pulse_server_;
  std::string sink_prefix_;

  // Keep handler registrations alive
  immer::box<events::EventBusHandlers> created_handler_;
  immer::box<events::EventBusHandlers> stopped_handler_;

  // hostname -> session_id
  mutable std::mutex map_mu_;
  std::unordered_map<std::string, std::string> host_to_session_;

  std::atomic<bool> started_{false};
};

} // namespace wolf::core::audio
