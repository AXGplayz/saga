// End-to-end session test over real sockets, with the game replaced by stubs.
// Run two processes: one host, one guest. Each writes the inputs the "game"
// received on every frame; the two files must be identical.
//
//   session_loopback_test host <work_dir> <port> <frames>
//   session_loopback_test join <work_dir> <port> <frames>

#include "host/netplay/game_bridge.hpp"
#include "host/netplay/lockstep.hpp"
#include "host/netplay/session.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

// ---------------------------------------------------------------- input stubs

namespace {
    void (*g_frame)(void) = nullptr;
    bool g_is_host = false;
    std::uint32_t g_sample_count[2] = {0, 0};
    struct Applied {
        std::uint32_t buttons[2] = {0, 0};
        float x[2] = {0, 0};
        float y[2] = {0, 0};
    };
    Applied g_current;
    std::vector<Applied> g_applied;
    std::uint32_t g_state = 2166136261u;
} // namespace

void HostInputEnableTwoPlayer(bool) {}
void HostInputSetLockstepFrame(void (*frame)(void)) { g_frame = frame; }
void HostInputClearNetFrame() {}

void HostInputSampleLocal(int port, unsigned *buttons, float *left_x, float *left_y) {
    // Each player presses a different, busy pattern on their own port 0.
    const std::uint32_t n = g_sample_count[port]++;
    const std::uint32_t player = g_is_host ? 0 : 1;
    *buttons = port == 0 ? ((n * 2654435761u) ^ (player * 0x5bd1e995u)) >> 18 : 0;
    *left_x = port == 0 ? static_cast<float>(static_cast<int>((n * 7 + player * 50) % 255) - 127) / 127.0f : 0.0f;
    *left_y = port == 0 ? static_cast<float>(static_cast<int>((n * 3 + player * 90) % 255) - 127) / 127.0f : 0.0f;
}

void HostInputSetNetFrame(int port, unsigned buttons, float left_x, float left_y) {
    g_current.buttons[port] = buttons;
    g_current.x[port] = left_x;
    g_current.y[port] = left_y;
}

namespace saga::netplay {
    void apply_deterministic_engine_settings() {}

    std::string StateHash::describe() const { return "total=" + std::to_string(total); }

    // The fake simulation state is a running hash of every applied input.
    StateHash hash_game_state() {
        StateHash hash;
        hash.total = g_state;
        hash.players = g_state;
        return hash;
    }
} // namespace saga::netplay

int main(int argc, char **argv) {
    if (argc != 5) {
        std::fprintf(stderr, "usage: %s host|join <work_dir> <port> <frames>\n", argv[0]);
        return 2;
    }
    namespace fs = std::filesystem;
    using namespace saga::netplay;
    const std::string role = argv[1];
    const fs::path work = argv[2];
    const auto port = static_cast<std::uint16_t>(std::atoi(argv[3]));
    const int frames = std::atoi(argv[4]);
    g_is_host = role == "host";

    const fs::path documents = work / role;
    fs::create_directories(documents / "SavedGames");
    if (g_is_host) {
        std::ofstream(documents / "SavedGames" / "slot0.sav") << "host progress: 47% complete";
        std::ofstream(documents / "SavedGames" / "code.bin") << "code";
    } else {
        std::ofstream(documents / "SavedGames" / "slot0.sav") << "guest's own save, must not be used";
    }
    std::ofstream(work / "fake.obb") << "same game data on both PCs";

    Config config;
    config.mode = g_is_host ? Mode::host : Mode::join;
    config.port = port;
    config.address = "127.0.0.1:" + std::to_string(port);
    config.documents_path = documents.string();
    config.obb_path = (work / "fake.obb").string();
    start(config);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (phase() == Phase::connecting && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    if (phase() != Phase::ready) {
        std::fprintf(stderr, "[%s] lobby failed: %s\n", role.c_str(), failure_text().c_str());
        return 1;
    }

    if (!g_is_host) {
        // The guest must boot from the host's saves, not its own.
        std::ifstream save(fs::path(documents_path()) / "SavedGames" / "slot0.sav");
        std::string text((std::istreambuf_iterator<char>(save)), {});
        if (text != "host progress: 47% complete") {
            std::fprintf(stderr, "[join] wrong save in %s: '%s'\n", documents_path().c_str(), text.c_str());
            return 1;
        }
        std::printf("[join] booting from the host's saves in %s\n", documents_path().c_str());
    }

    begin_game();
    // The guest leaves early to test a clean goodbye; the host keeps playing alone.
    const int my_frames = g_is_host ? frames + 120 : frames;
    for (int frame = 0; frame < my_frames; ++frame) {
        g_frame();
        g_applied.push_back(g_current);
        for (int p = 0; p < 2; ++p)
            g_state = (g_state ^ g_current.buttons[p]) * 16777619u;
    }
    std::printf("[%s] %s\n", role.c_str(), status_text().c_str());
    stop();

    std::ofstream out(work / (role + "_applied.txt"));
    for (int frame = 0; frame < frames; ++frame) {
        const Applied &a = g_applied[static_cast<std::size_t>(frame)];
        out << frame << ' ' << a.buttons[0] << ' ' << a.x[0] << ' ' << a.y[0] << ' ' << a.buttons[1] << ' ' << a.x[1]
            << ' ' << a.y[1] << '\n';
    }
    return 0;
}
