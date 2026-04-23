#define CATCH_CONFIG_FAST_COMPILE

#include <catch2/catch_session.hpp>
#include <control/control.hpp>
#include <core/docker.hpp>
#include <helpers/logger.hpp>
#include <helpers/utils.hpp>
#include <streaming/streaming.hpp>
#include <cstdlib>

int main(int argc, char *argv[]) {
  logs::init(logs::parse_level(utils::get_env("WOLF_LOG_LEVEL", "TRACE")));
  streaming::init();
  control::init();       // Need to initialise enet once
  state::docker::init(); // Need to initialise libcurl once
  setenv("WOLF_SKIP_WAYLAND_SOCKET_WAIT", "TRUE", 1);

  int result = Catch::Session().run(argc, argv);

  return result;
}
