#pragma once

// Online co-op sessions and the single-PC determinism test.
//
//   host:   waits for a friend, sends them this PC's saves, then both games
//           run in lockstep. This PC is player 1.
//   join:   dials the host, boots from a copy of the host's saves, player 2.
//   record: plays normally (two local pads) while recording every input and
//           a checksum of the game state once per second.
//   replay: re-runs a recording and reports the first frame where the game
//           state differs. If replays match, online co-op will stay in sync.

#include <cstdint>
#include <string>

namespace saga::netplay {

    enum class Mode { off, host, join, record, replay };

    struct Config final {
        Mode mode = Mode::off;
        std::string address;      // join: "host[:port]"
        std::uint16_t port = 7777; // host: TCP and UDP port to listen on
        int delay = 0;            // input delay in frames; 0 = pick from measured ping
        std::string file;         // record/replay: input file
        std::string documents_path = "res/";
        std::string obb_path = "res/main.1060.com.wb.lego.tcs.obb";
    };

    enum class Phase { idle, connecting, ready, failed, playing, ended };

    // Starts the session. Networked modes connect on a background thread.
    void start(const Config &config);
    [[nodiscard]] Phase phase();
    // One line describing the session, for the window title.
    [[nodiscard]] std::string status_text();
    [[nodiscard]] std::string failure_text();
    // Folder the game must use for saves (a guest boots from the host's saves).
    [[nodiscard]] std::string documents_path();
    // Call once phase() is ready, right before NuMain starts.
    void begin_game();
    // Says goodbye to the other game and stops waiting for it.
    void stop();

} // namespace saga::netplay
