#include <boost/bind/bind.hpp>
#include <boost/enable_shared_from_this.hpp>
#include <rtp/udp-ping.hpp>
#include <thread>

namespace rtp {

UDP_Server::UDP_Server(unsigned short port, const on_rtp_ping_fn &callback)
    : io_context(), socket_(io_context, udp::endpoint(udp::v4(), port)), callback(callback) {
  // We have to enable this because we'll bind additional sockets as udpsink in the audio/video pipelines
  socket_.set_option(udp::socket::reuse_address(true));
  start_receive();
}

UDP_Server::~UDP_Server() {
  socket_.close();
  io_context.stop();
}

void UDP_Server::run(std::chrono::milliseconds timeout) {
  io_context.run_for(timeout);
}

void UDP_Server::start_receive() {
  socket_.async_receive_from(boost::asio::buffer(recv_buffer_),
                             remote_endpoint_,
                             0,
                             boost::bind(&UDP_Server::handle_receive,
                                         this,
                                         boost::asio::placeholders::error,
                                         boost::asio::placeholders::bytes_transferred));
}

void UDP_Server::handle_receive(const boost::system::error_code &error, std::size_t bytes_transferred) {
  if (!error) {
    auto client_ip = remote_endpoint_.address().to_string();
    auto client_port = remote_endpoint_.port();

    if (bytes_transferred == 4) {
      logs::log(logs::trace, "[RTP] Received ping from {}:{}", client_ip, client_port);
      callback({.client_ip = client_ip, .client_port = client_port, .payload = {}});
    } else if (bytes_transferred >= sizeof(moonlight::SS_PING)) {
      logs::log(logs::trace, "[RTP] Received ping from {}:{} with payload", client_ip, client_port);

      auto ping = (moonlight::SS_PING *)recv_buffer_.data();
      callback({.client_ip = client_ip, .client_port = client_port, .payload = ping->payload});
    }

    // We'll keep receiving pings and sending callback events until the timeout elapsed.
    // This is because we don't know if downstream they are ready to start the session.
    // Downstream will make sure to only send one ping per session
    std::this_thread::sleep_for(std::chrono::milliseconds(250)); // let's avoid spamming though
    start_receive();
  } else {
    logs::log(logs::debug, "[RTP] Error receiving ping: {}", error.message());
  }
}

void wait_for_ping(unsigned short port, const on_rtp_ping_fn &callback) {
  auto thread = std::thread(
      [port](const on_rtp_ping_fn &callback) {
        try {
          auto server = UDP_Server(port, callback);

          logs::log(logs::info, "RTP server started on port: {}", port);
          server.run(std::chrono::seconds(30));
          logs::log(logs::debug, "RTP server on port: {} stopped", port);

        } catch (std::exception &e) {
          logs::log(logs::warning, "[RTP] Unable to start RTP server on {}: {}", port, e.what());
        }
      },
      callback);
  thread.detach();
}

void start_rtp_ping(const wolf::core::events::StreamSession &session) {
  // Video RTP Ping
  wait_for_ping(session.video_stream_port, [ev_bus = session.event_bus](const RTPPingEvent &ping) {
    logs::log(logs::trace, "[PING] video from {}:{}", ping.client_ip, ping.client_port);
    auto ev = wolf::core::events::RTPVideoPingEvent{.client_ip = ping.client_ip,
                                                    .client_port = ping.client_port,
                                                    .payload = ping.payload};
    ev_bus->fire_event(immer::box<wolf::core::events::RTPVideoPingEvent>(ev));
  });

  // Audio RTP Ping
  wait_for_ping(session.audio_stream_port, [ev_bus = session.event_bus](const RTPPingEvent &ping) {
    logs::log(logs::trace, "[PING] audio from {}:{}", ping.client_ip, ping.client_port);
    auto ev = wolf::core::events::RTPAudioPingEvent{.client_ip = ping.client_ip,
                                                    .client_port = ping.client_port,
                                                    .payload = ping.payload};
    ev_bus->fire_event(immer::box<wolf::core::events::RTPAudioPingEvent>(ev));
  });
}

} // namespace rtp