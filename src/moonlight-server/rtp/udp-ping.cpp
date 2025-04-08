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

    logs::log(logs::trace, "[RTP] Received ping from {}:{} ({} bytes)", client_ip, client_port, bytes_transferred);

    if (bytes_transferred == 4) {
      callback({.client_ip = client_ip, .client_port = client_port, .payload = {}});
    } else if (bytes_transferred >= sizeof(moonlight::SS_PING)) {
      auto ping = (moonlight::SS_PING *)recv_buffer_.data();
      callback({.client_ip = client_ip, .client_port = client_port, .payload = ping->payload});
    }
    // Continue to receive more data
    start_receive();
  } else {
    logs::log(logs::warning, "[RTP] Error receiving ping: {}", error.message());
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

void start_rtp_ping(unsigned short video_port,
                    unsigned short audio_port,
                    std::shared_ptr<wolf::core::events::EventBusType> event_bus) {
  // Video RTP Ping
  wait_for_ping(video_port, [=](const RTPPingEvent &ping) {
    logs::log(logs::trace, "[RTP] video {} from {}:{}", video_port, ping.client_ip, ping.client_port);
    auto ev = wolf::core::events::RTPVideoPingEvent{.client_ip = ping.client_ip,
                                                    .client_port = ping.client_port,
                                                    .payload = ping.payload};
    event_bus->fire_event(immer::box<wolf::core::events::RTPVideoPingEvent>(ev));
  });

  // Audio RTP Ping
  wait_for_ping(audio_port, [=](const RTPPingEvent &ping) {
    logs::log(logs::trace, "[RTP] audio {} from {}:{}", audio_port, ping.client_ip, ping.client_port);
    auto ev = wolf::core::events::RTPAudioPingEvent{.client_ip = ping.client_ip,
                                                    .client_port = ping.client_port,
                                                    .payload = ping.payload};
    event_bus->fire_event(immer::box<wolf::core::events::RTPAudioPingEvent>(ev));
  });
}

} // namespace rtp