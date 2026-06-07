#!/bin/bash
set -e

# Make sure configure folder exists
# as Wolf may try to create default config in non existing folder and crash.
# See https://github.com/games-on-whales/wolf/pull/65#discussion_r1509235307
# and https://github.com/games-on-whales/wolf/issues/64#issuecomment-1951479056
export WOLF_CFG_FOLDER=$HOST_APPS_STATE_FOLDER/cfg
mkdir -p $WOLF_CFG_FOLDER
# Adjust env variables if the user moved the folder
export WOLF_CFG_FILE=$WOLF_CFG_FOLDER/config.toml
export WOLF_PRIVATE_KEY_FILE=$WOLF_CFG_FOLDER/key.pem
export WOLF_PRIVATE_CERT_FILE=$WOLF_CFG_FOLDER/cert.pem

# Set default values for environment variables
export WOLF_RENDER_NODE=${WOLF_RENDER_NODE:-/dev/dri/renderD128}
export WOLF_ENCODER_NODE=${WOLF_ENCODER_NODE:-$WOLF_RENDER_NODE}
export GST_GL_DRM_DEVICE=${GST_GL_DRM_DEVICE:-$WOLF_ENCODER_NODE}

# Update fake-udev if missing from the path
export WOLF_DOCKER_FAKE_UDEV_PATH=${WOLF_DOCKER_FAKE_UDEV_PATH:-$HOST_APPS_STATE_FOLDER/fake-udev}
cp /wolf/fake-udev $WOLF_DOCKER_FAKE_UDEV_PATH

# Start PulseAudio inside the Wolf container and wait for it to be ready before
# launching Wolf. This replaces the legacy "WolfPulseAudio" sidecar container as
# the default audio backend: by the time Wolf checks for a running PulseAudio
# server it's already up, so there's no startup race and no need to tune
# WOLF_PULSE_CONTAINER_TIMEOUT_MS. Wolf still falls back to spinning up the
# sidecar container if this server isn't reachable.
#
# Set PULSE_SERVER yourself (e.g. to a host socket) to skip the embedded server
# and point Wolf at an external PulseAudio/PipeWire instead.
if [ -z "${PULSE_SERVER:-}" ] && command -v pulseaudio >/dev/null 2>&1; then
    export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/tmp/sockets}
    mkdir -p "$XDG_RUNTIME_DIR"
    pulse_socket="$XDG_RUNTIME_DIR/pulse-socket"

    # Remove a stale socket, PulseAudio refuses to start otherwise
    rm -f "$pulse_socket"

    echo "Starting embedded PulseAudio server on $pulse_socket"
    # -n: don't load the distro default.pa (we only want the modules below)
    # auth-anonymous=1: let the app containers connect without a cookie
    # exit-idle-time=-1: never shut down, even when no client is connected
    pulseaudio \
        -n \
        --daemonize=false \
        --fail=false \
        --exit-idle-time=-1 \
        --load="module-native-protocol-unix auth-anonymous=1 socket=$pulse_socket" \
        --load="module-always-sink" \
        --log-level=warn \
        --log-target=stderr &

    # Wait (up to ~10s) for the socket to appear so Wolf connects to it on the
    # first try rather than falling back to the sidecar container
    for _ in $(seq 1 100); do
        if [ -S "$pulse_socket" ]; then break; fi
        sleep 0.1
    done
    if [ -S "$pulse_socket" ]; then
        # Bare path (no "unix:" prefix): Wolf reports this verbatim to the app
        # containers as PULSE_SERVER and derives the socket mount from it
        export PULSE_SERVER="$pulse_socket"
        echo "Embedded PulseAudio ready"
    else
        echo "WARN: embedded PulseAudio didn't come up in time, Wolf will fall back to the sidecar container"
    fi
fi

exec /wolf/wolf