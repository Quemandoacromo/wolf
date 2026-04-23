#pragma once

#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/stat.h>
#include <thread>

#include <helpers/logger.hpp>

namespace wolf::core::sessions {
constexpr std::string_view VIRTUAL_SINK_PREFIX = "virtual_sink_";

inline bool wait_for_wayland_socket(std::string_view runtime_dir,
                                    const std::string &socket_name,
                                    std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
  auto socket_path = std::filesystem::path(runtime_dir) / socket_name;
  auto deadline = std::chrono::steady_clock::now() + timeout;
  struct stat st {};

  while (std::chrono::steady_clock::now() < deadline) {
    if (stat(socket_path.c_str(), &st) == 0 && S_ISSOCK(st.st_mode)) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  if (stat(socket_path.c_str(), &st) == 0) {
    logs::log(logs::error,
              "Wayland endpoint {} exists but is not a socket (mode={:o})",
              socket_path.string(),
              st.st_mode);
  } else {
    logs::log(logs::error, "Wayland socket {} was never created", socket_path.string());
  }

  return false;
}
} // namespace wolf::core::sessions
