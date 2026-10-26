#pragma once

// Delay-based lockstep for two players.
//
// Both games run the same simulation from the same starting point. Every frame
// each side contributes one controller sample; a frame may only be simulated
// once both samples for it are known. A local sample taken on frame k is
// scheduled for frame k + delay, which hides network latency behind a small,
// constant input delay. Packets repeat every unacknowledged local sample, so
// lost, duplicated or reordered datagrams are harmless.
//
// This file is deliberately free of sockets and game code so it can be tested
// on its own.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace saga::netplay {

    // One controller sample. Sticks are quantised so both games see identical values.
    struct NetInput final {
        std::uint32_t buttons = 0;
        std::int8_t left_x = 0;
        std::int8_t left_y = 0;

        [[nodiscard]] bool operator==(const NetInput &other) const noexcept {
            return buttons == other.buttons && left_x == other.left_x && left_y == other.left_y;
        }
        [[nodiscard]] bool operator!=(const NetInput &other) const noexcept { return !(*this == other); }

        static std::int8_t quantize_axis(float value) noexcept;
        static float axis_value(std::int8_t value) noexcept { return static_cast<float>(value) / 127.0f; }
    };

    struct Checkpoint final {
        std::uint32_t frame = 0;
        std::uint32_t hash = 0;
    };

    // Wire format constants.
    inline constexpr std::uint32_t packet_magic = 0x4E534354; // "TCSN"
    inline constexpr std::uint8_t packet_inputs = 1;
    inline constexpr std::uint8_t packet_bye = 2;
    inline constexpr std::uint8_t packet_join = 3;
    inline constexpr std::uint8_t packet_joined = 4;
    inline constexpr std::size_t max_inputs_per_packet = 64;
    inline constexpr std::size_t max_packet_size = 64 + max_inputs_per_packet * 6;

    struct PacketHeader final {
        std::uint8_t type = 0;
        std::uint32_t session = 0;
    };

    // Decoded contents of an inputs packet.
    struct InputPacket final {
        std::uint32_t ack = 0;         // how many of the receiver's frames the sender holds contiguously
        std::uint32_t first_frame = 0; // frame of inputs[0]
        std::vector<NetInput> inputs;
        std::optional<Checkpoint> checkpoint;
        std::uint32_t send_time_ms = 0; // sender clock, echoed back for ping
        std::uint32_t echo_time_ms = 0; // last send_time_ms the sender received from us
    };

    std::size_t encode_header(std::uint8_t *out, std::uint8_t type, std::uint32_t session);
    bool decode_header(const std::uint8_t *data, std::size_t size, PacketHeader &out);
    std::size_t encode_input_packet(std::uint8_t *out, std::uint32_t session, const InputPacket &packet);
    bool decode_input_packet(const std::uint8_t *data, std::size_t size, InputPacket &out);

    enum class DesyncState { in_sync, desynced };

    class Lockstep final {
      public:
        // `delay` is the input delay in frames (>= 1). Frames before `delay`
        // use neutral input on both sides, so both start ready.
        explicit Lockstep(std::uint32_t delay);

        [[nodiscard]] std::uint32_t delay() const noexcept { return delay_; }

        // Schedules the local sample taken on frame `frame` for frame + delay.
        // Must be called once per frame, in order, starting at frame 0. Returns
        // false if that order is broken or the other game has stopped
        // acknowledging inputs for so long that unsent samples would be lost.
        bool submit_local(std::uint32_t frame, const NetInput &input);

        // True once both samples for `frame` are known.
        [[nodiscard]] bool ready(std::uint32_t frame) const noexcept { return frame < remote_count_ && frame < local_count_; }
        [[nodiscard]] NetInput local_input(std::uint32_t frame) const;
        [[nodiscard]] NetInput remote_input(std::uint32_t frame) const;

        // Number of contiguous frames held, starting at frame 0.
        [[nodiscard]] std::uint32_t local_count() const noexcept { return local_count_; }
        [[nodiscard]] std::uint32_t remote_count() const noexcept { return remote_count_; }
        [[nodiscard]] std::uint32_t peer_ack() const noexcept { return peer_ack_; }

        // Builds the next outgoing packet: every local sample the peer has not acknowledged.
        [[nodiscard]] InputPacket make_packet(std::uint32_t now_ms) const;
        // Merges a received packet. Returns false if it was malformed for this session.
        bool receive(const InputPacket &packet, std::uint32_t now_ms);

        // Checksums of the game state, compared between the two games.
        void add_local_checkpoint(Checkpoint checkpoint);
        [[nodiscard]] DesyncState desync_state() const noexcept { return desync_; }
        [[nodiscard]] std::uint32_t desync_frame() const noexcept { return desync_frame_; }
        [[nodiscard]] std::uint32_t matched_checkpoints() const noexcept { return matched_checkpoints_; }

        // Round-trip time estimate in milliseconds (0 until measured).
        [[nodiscard]] std::uint32_t rtt_ms() const noexcept { return rtt_ms_; }

      private:
        static constexpr std::size_t ring_size = 1024;
        static constexpr std::size_t checkpoint_ring = 64;

        struct Slot final {
            std::uint32_t frame = UINT32_MAX;
            NetInput input;
        };

        void compare_checkpoints();

        std::uint32_t delay_;
        std::array<Slot, ring_size> local_{};
        std::array<Slot, ring_size> remote_{};
        std::uint32_t local_count_;
        std::uint32_t remote_count_;
        std::uint32_t peer_ack_;

        std::array<Checkpoint, checkpoint_ring> local_checkpoints_{};
        std::array<Checkpoint, checkpoint_ring> remote_checkpoints_{};
        std::size_t local_checkpoint_count_ = 0;
        std::size_t remote_checkpoint_count_ = 0;
        std::optional<Checkpoint> latest_local_checkpoint_;
        std::uint32_t last_compared_frame_ = UINT32_MAX;
        std::uint32_t matched_checkpoints_ = 0;
        DesyncState desync_ = DesyncState::in_sync;
        std::uint32_t desync_frame_ = 0;

        std::uint32_t last_remote_send_time_ = 0;
        std::uint32_t rtt_ms_ = 0;
    };

} // namespace saga::netplay
