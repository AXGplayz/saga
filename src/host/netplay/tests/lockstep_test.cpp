// Standalone tests for the netplay core. No game code or Bazel needed:
//   c++ -std=c++20 -I src src/host/netplay/lockstep.cpp src/host/netplay/net_socket.cpp
//       src/host/netplay/tests/lockstep_test.cpp -o lockstep_test -pthread && ./lockstep_test

#include "host/netplay/lockstep.hpp"
#include "host/netplay/net_socket.hpp"

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace saga::netplay;

namespace {

    int failures = 0;

#define CHECK(condition)                                                                                               \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            std::printf("  FAILED %s:%d: %s\n", __FILE__, __LINE__, #condition);                                     \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (0)

    NetInput scripted_input(int player, std::uint32_t frame) {
        // Deterministic but busy input: buttons change every few frames, sticks sweep.
        NetInput input;
        input.buttons = (frame * 2654435761u + static_cast<std::uint32_t>(player) * 97u) >> 20;
        input.left_x = static_cast<std::int8_t>(static_cast<int>((frame * 7 + player * 31) % 255) - 127);
        input.left_y = static_cast<std::int8_t>(static_cast<int>((frame * 13 + player * 17) % 255) - 127);
        return input;
    }

    struct InFlight final {
        std::uint64_t deliver_at;
        std::vector<std::uint8_t> bytes;
    };

    // Simulates two games running lockstep over a hostile network.
    void test_lossy_network(double loss, double duplicate, int min_latency, int max_latency, std::uint32_t delay) {
        std::printf("lossy network: loss=%.0f%% dup=%.0f%% latency=%d-%dms delay=%u\n", loss * 100, duplicate * 100,
                    min_latency, max_latency, delay);
        constexpr std::uint32_t frames = 3000;
        constexpr std::uint32_t session = 0xC0FFEE;
        std::mt19937 rng(1234);
        std::uniform_real_distribution<double> chance(0.0, 1.0);
        std::uniform_int_distribution<int> latency(min_latency, max_latency);

        Lockstep side[2] = {Lockstep(delay), Lockstep(delay)};
        std::deque<InFlight> wire[2]; // wire[i] carries packets towards side i
        std::uint32_t next_frame[2] = {0, 0};
        std::vector<NetInput> applied[2][2]; // applied[side][port]
        std::uint64_t now = 0;
        std::uint64_t stalled_ms = 0;

        auto send = [&](int from) {
            std::uint8_t buffer[max_packet_size];
            const std::size_t size =
                encode_input_packet(buffer, session, side[from].make_packet(static_cast<std::uint32_t>(now + 1)));
            const int copies = chance(rng) < duplicate ? 2 : 1;
            for (int copy = 0; copy < copies; ++copy) {
                if (chance(rng) < loss)
                    continue;
                wire[1 - from].push_back(
                    InFlight{now + static_cast<std::uint64_t>(latency(rng)), {buffer, buffer + size}});
            }
        };

        while ((next_frame[0] < frames || next_frame[1] < frames) && now < 600000) {
            now += 1;
            for (int s = 0; s < 2; ++s) {
                // Deliver whatever has arrived (latency jitter reorders packets).
                for (auto it = wire[s].begin(); it != wire[s].end();) {
                    if (it->deliver_at <= now) {
                        PacketHeader header;
                        InputPacket packet;
                        CHECK(decode_header(it->bytes.data(), it->bytes.size(), header));
                        CHECK(header.session == session && header.type == packet_inputs);
                        CHECK(decode_input_packet(it->bytes.data(), it->bytes.size(), packet));
                        side[s].receive(packet, static_cast<std::uint32_t>(now + 1));
                        it = wire[s].erase(it);
                    } else {
                        ++it;
                    }
                }
            }
            // Each game ticks at ~60 fps but only advances when its next frame is ready.
            if (now % 16 != 0)
                continue;
            for (int s = 0; s < 2; ++s) {
                const std::uint32_t frame = next_frame[s];
                if (frame >= frames)
                    continue;
                if (side[s].local_count() == frame + delay) // sample once per frame
                    side[s].submit_local(frame, scripted_input(s, frame));
                send(s);
                if (!side[s].ready(frame)) {
                    stalled_ms += 16;
                    continue;
                }
                // Port 0 is the host (side 0), port 1 the guest (side 1), on both machines.
                const NetInput mine = side[s].local_input(frame);
                const NetInput theirs = side[s].remote_input(frame);
                applied[s][0].push_back(s == 0 ? mine : theirs);
                applied[s][1].push_back(s == 0 ? theirs : mine);
                if (frame % 60 == 0) {
                    // Both games hash identical state, so checkpoints must match.
                    std::uint32_t hash = 2166136261u;
                    for (int port = 0; port < 2; ++port) {
                        const NetInput &input = applied[s][port].back();
                        hash = (hash ^ input.buttons) * 16777619u;
                    }
                    side[s].add_local_checkpoint(Checkpoint{frame, hash});
                }
                next_frame[s] = frame + 1;
            }
        }

        CHECK(next_frame[0] == frames && next_frame[1] == frames);
        CHECK(applied[0][0] == applied[1][0]);
        CHECK(applied[0][1] == applied[1][1]);
        // Every applied sample must be exactly what the owning player pressed `delay` frames earlier.
        bool exact = true;
        for (std::uint32_t frame = delay; frame < frames && exact; ++frame) {
            exact = applied[0][0][frame] == scripted_input(0, frame - delay) &&
                    applied[0][1][frame] == scripted_input(1, frame - delay);
        }
        CHECK(exact);
        CHECK(side[0].desync_state() == DesyncState::in_sync && side[1].desync_state() == DesyncState::in_sync);
        CHECK(side[0].matched_checkpoints() > 30 && side[1].matched_checkpoints() > 30);
        std::printf("  ok: %u frames identical on both sides, %u checkpoints matched, rtt~%ums, stall %.1fs\n", frames,
                    side[0].matched_checkpoints(), side[0].rtt_ms(), static_cast<double>(stalled_ms) / 2000.0);
    }

    void test_desync_detection() {
        std::printf("desync detection\n");
        Lockstep a(2);
        Lockstep b(2);
        a.add_local_checkpoint(Checkpoint{60, 111});
        b.add_local_checkpoint(Checkpoint{60, 111});
        a.add_local_checkpoint(Checkpoint{120, 222});
        b.add_local_checkpoint(Checkpoint{120, 999}); // b's game drifted
        InputPacket from_b = b.make_packet(5);
        a.receive(from_b, 6);
        CHECK(a.desync_state() == DesyncState::desynced);
        CHECK(a.desync_frame() == 120);
        std::printf("  ok: mismatch reported at frame %u\n", a.desync_frame());
    }

    void test_wire_format() {
        std::printf("wire format\n");
        InputPacket packet;
        packet.ack = 1234;
        packet.first_frame = 99;
        packet.inputs = {NetInput{0x800, -127, 127}, NetInput{0xFFFFFFFF, 0, -1}};
        packet.checkpoint = Checkpoint{600, 0xDEADBEEF};
        packet.send_time_ms = 42;
        packet.echo_time_ms = 41;
        std::uint8_t buffer[max_packet_size];
        const std::size_t size = encode_input_packet(buffer, 7, packet);
        InputPacket decoded;
        PacketHeader header;
        CHECK(decode_header(buffer, size, header) && header.session == 7);
        CHECK(decode_input_packet(buffer, size, decoded));
        CHECK(decoded.ack == 1234 && decoded.first_frame == 99 && decoded.inputs == packet.inputs);
        CHECK(decoded.checkpoint && decoded.checkpoint->hash == 0xDEADBEEF);
        // Truncated packets are rejected rather than read past the end.
        for (std::size_t cut = 0; cut < size; ++cut)
            CHECK(!decode_input_packet(buffer, cut, decoded) || cut >= size);
        CHECK(NetInput::quantize_axis(1.5f) == 127 && NetInput::quantize_axis(-1.0f) == -127);
        CHECK(NetInput::quantize_axis(0.0f) == 0);
        std::printf("  ok\n");
    }

    void test_real_sockets() {
        std::printf("real UDP + TCP over loopback\n");
        std::string error;
        TcpListener listener;
        CHECK(listener.open(47777, error));
        std::thread client([] {
            NetAddress address;
            std::string client_error;
            CHECK(net_resolve("127.0.0.1:47777", 0, address, client_error));
            TcpStream stream;
            CHECK(stream.connect(address, 2000, client_error));
            const char hello[5] = {'h', 'e', 'l', 'l', 'o'};
            CHECK(stream.send_all(hello, sizeof(hello)));
        });
        TcpStream accepted = listener.accept(3000);
        CHECK(accepted.is_open());
        char received[5] = {};
        CHECK(accepted.receive_all(received, sizeof(received), 2000));
        CHECK(std::string(received, 5) == "hello");
        client.join();

        UdpSocket a;
        UdpSocket b;
        CHECK(a.open(47778, error));
        CHECK(b.open(0, error));
        NetAddress to_a;
        CHECK(net_resolve("127.0.0.1", 47778, to_a, error));
        const std::uint8_t datagram[3] = {1, 2, 3};
        CHECK(b.send_to(to_a, datagram, sizeof(datagram)));
        std::uint8_t buffer[16];
        NetAddress from;
        CHECK(a.receive(buffer, sizeof(buffer), from, 1000) == 3);
        CHECK(from.port == b.local_port());
        std::printf("  ok\n");
    }

} // namespace

int main() {
    test_wire_format();
    test_desync_detection();
    test_lossy_network(0.0, 0.0, 10, 10, 2);
    test_lossy_network(0.3, 0.1, 20, 120, 4);
    test_lossy_network(0.6, 0.2, 40, 200, 8);
    test_real_sockets();
    std::printf(failures == 0 ? "\nALL TESTS PASSED\n" : "\n%d CHECKS FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
