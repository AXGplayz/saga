#pragma once

// The only part of netplay that touches game internals.

#include <cstdint>
#include <string>

namespace saga::netplay {

    // Switches the engine into deterministic mode before NuMain starts:
    // synchronous asset loading, fixed frame time, frame-counted timers and a
    // second gamepad for player 2.
    void apply_deterministic_engine_settings();

    struct StateHash final {
        std::uint32_t total = 0;
        std::uint32_t rng = 0;
        std::uint32_t players = 0;
        std::uint32_t world = 0;
        std::uint32_t timer = 0;

        // One line for the session log, so two logs can be compared side by side.
        [[nodiscard]] std::string describe() const;
    };

    // Checksum of the simulation state that matters for co-op: random seeds,
    // every player's position, facing and health, the current level and menu,
    // and the game clock. Call on the game thread between frames.
    StateHash hash_game_state();

} // namespace saga::netplay
