#include "host/netplay/game_bridge.hpp"

#include "decomp.h"
#include "globals.h"
#include "host/platform/input.hpp"
#include "host/platform/netplay_hooks.h"
#include "legoapi/characters/core/players.h"
#include "legoapi/core/input/timer.h"
#include "legoapi/misc/utilities.h"
#include "legoapi/world/level.h"
#include "legoapi/world/world.h"
#include "nu2api/nucore/bgproc.h"
#include "nu2api/numath/nurand.h"

#include <cstdio>
#include <cstring>

i32 GetMenuID(void);

namespace saga::netplay {
    namespace {

        struct Fnv final {
            std::uint32_t value = 2166136261u;

            void add(std::uint32_t word) {
                for (int i = 0; i < 4; ++i) {
                    value ^= (word >> (i * 8)) & 0xffu;
                    value *= 16777619u;
                }
            }

            void add_float(f32 number) {
                std::uint32_t bits;
                std::memcpy(&bits, &number, sizeof(bits));
                add(bits);
            }
        };

    } // namespace

    void apply_deterministic_engine_settings() {
        // Background loading finishes at a different moment on each PC, so the
        // two games would see new areas appear on different frames. Load
        // everything on the game thread instead: slower hitches, same frame.
        BGLOAD = 0;
        multithreaded = 0;

        // Every frame advances the simulation by exactly 1/60 s.
        g_host_forced_frametime = 0.016666668f;
        g_host_lockstep_active = 1;

        HostInputEnableTwoPlayer(true);
    }

    std::string StateHash::describe() const {
        char text[128];
        std::snprintf(text, sizeof(text), "total=%08x rng=%08x players=%08x world=%08x timer=%08x", total, rng, players,
                      world, timer);
        return text;
    }

    StateHash hash_game_state() {
        StateHash result;

        Fnv rng;
        rng.add(static_cast<std::uint32_t>(qseed));
        rng.add(static_cast<std::uint32_t>(GAMERAND));
        rng.add(NuRandGetSeed());
        result.rng = rng.value;

        Fnv players;
        for (i32 i = 0; i < 8; ++i) {
            const GameObject_s *object = Player[i];
            if (object == nullptr) {
                players.add(0xffffffffu);
                continue;
            }
            players.add(static_cast<std::uint32_t>(object->id));
            players.add_float(object->apiobj.position.x);
            players.add_float(object->apiobj.position.y);
            players.add_float(object->apiobj.position.z);
            players.add(object->apiobj.facing_angle);
            players.add(static_cast<std::uint32_t>(object->current_hp));
            players.add(object->apiobj.player_controlled ? 1u : 0u);
        }
        result.players = players.value;

        Fnv world;
        std::int32_t level = -1;
        if (WORLD != nullptr && WORLD->current_level != nullptr && LDataList != nullptr)
            level = static_cast<std::int32_t>(WORLD->current_level - LDataList);
        world.add(static_cast<std::uint32_t>(level));
        world.add(static_cast<std::uint32_t>(GetMenuID()));
        world.add(static_cast<std::uint32_t>(Paused));
        result.world = world.value;

        Fnv timer;
        timer.add(static_cast<std::uint32_t>(GameTimer.update_count));
        timer.add_float(GameTimer.time_elapsed);
        result.timer = timer.value;

        Fnv total;
        total.add(result.rng);
        total.add(result.players);
        total.add(result.world);
        total.add(result.timer);
        result.total = total.value;
        return result;
    }

} // namespace saga::netplay
