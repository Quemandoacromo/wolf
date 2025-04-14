#pragma once
#include <boost/asio.hpp>
#include <core/gstreamer.hpp>
#include <core/virtual-display.hpp>
#include <events/events.hpp>
#include <fmt/format.h>
#include <gst-plugin/gstrtpmoonlightpay_audio.hpp>
#include <gst-plugin/gstrtpmoonlightpay_video.hpp>
#include <gst-plugin/video.hpp>
#include <gst/gst.h>
#include <immer/box.hpp>
#include <memory>
#include <moonlight/fec.hpp>

namespace streaming {

using namespace wolf::core;
using boost::asio::ip::udp;

void start_video_producer(std::size_t session_id,
                          wolf::core::virtual_display::wl_state_ptr wl_state,
                          const wolf::core::virtual_display::DisplayMode &display_mode,
                          const std::shared_ptr<events::EventBusType> &event_bus);

void start_audio_producer(std::size_t session_id,
                          const std::shared_ptr<events::EventBusType> &event_bus,
                          int channel_count,
                          const std::string &sink_name,
                          const std::string &server_name);

void start_streaming_video(immer::box<events::VideoSession> video_session,
                           const std::shared_ptr<events::EventBusType> &event_bus,
                           std::string client_ip,
                           unsigned short client_port,
                           std::shared_ptr<udp::socket> video_socket);

void start_streaming_audio(immer::box<events::AudioSession> audio_session,
                           const std::shared_ptr<events::EventBusType> &event_bus,
                           std::string client_ip,
                           unsigned short client_port,
                           std::shared_ptr<udp::socket> audio_socket,
                           const std::string &sink_name,
                           const std::string &server_name);

namespace custom_src {

struct GstAppDataState {
  wolf::core::gstreamer::gst_element_ptr app_src;
  wolf::core::virtual_display::wl_state_ptr wayland_state;
  GMainContext *context;
  GSource *source;
  int framerate;
  GstClockTime timestamp = 0;
};

std::shared_ptr<GstAppDataState> setup_app_src(const wolf::core::virtual_display::DisplayMode &video_session,
                                               wolf::core::virtual_display::wl_state_ptr wl_ptr);

bool push_data(GstAppDataState *data);
void app_src_need_data(GstElement *pipeline, guint size, GstAppDataState *data);
void app_src_enough_data(GstElement *pipeline, guint size, GstAppDataState *data);
} // namespace custom_src

/**
 * @return the Gstreamer version we are linked to
 */
inline std::string get_gst_version() {
  guint major, minor, micro, nano;
  gst_version(&major, &minor, &micro, &nano);
  return fmt::format("{}.{}.{}-{}", major, minor, micro, nano);
}

/**
 * GStreamer needs to be initialised once per run
 * Call this method in your main.
 */
inline void init() {
  /* It is also possible to call the init function with two NULL arguments,
   * in which case no command line options will be parsed by GStreamer.
   */
  gst_init(nullptr, nullptr);
  logs::log(logs::info, "Gstreamer version: {}", get_gst_version());

  gst_element_register(nullptr, "rtpmoonlightpay_video", GST_RANK_PRIMARY, gst_TYPE_rtp_moonlight_pay_video);
  gst_element_register(nullptr, "rtpmoonlightpay_audio", GST_RANK_PRIMARY, gst_TYPE_rtp_moonlight_pay_audio);

  moonlight::fec::init();
}

} // namespace streaming