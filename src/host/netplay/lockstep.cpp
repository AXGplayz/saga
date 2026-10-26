#include "host/netplay/lockstep.hpp"

#include <algorithm>
#include <cmath>

namespace saga::netplay {
    namespace {

        void put_u8(std::uint8_t *&out, std::uint8_t value) { *out++ = value; }

        void put_u32(std::uint8_t *&out, std::uint32_t value) {
            out[0] = static_cast<std::uint8_t>(value);
            out[1] = static_cast<std::uint8_t>(value >> 8);
            out[2] = static_cast<std::uint8_t>(value >> 16);
            out[3] = static_cast<std::uint8_t>(value >> 24);
            out += 4;
        }

        struct Reader final {
            const std::uint8_t *cursor;
            const std::uint8_t *end;
            bool ok = true;

            std::uint8_t u8() {
                if (end - cursor < 1) {
                    ok = false;
                    return 0;
                }
                return *cursor++;
            }

            std::uint32_t u32() {
                if (end - cursor < 4) {
                    ok = false;
                    return 0;
                }
                const std::uint32_t value = static_cast<std::uint32_t>(cursor[0]) |
                                            (static_cast<std::uint32_t>(cursor[1]) << 8) |
                                            (static_cast<std::uint32_t>(cursor[2]) << 16) |
                                            (static_cast<std::uint32_t>(cursor[3]) << 24);
                cursor += 4;
                return value;
            }
        };

        constexpr std::size_t header_size = 9;

    } // namespace

    std::int8_t NetInput::quantize_axis(float value) noexcept {
        if (!(value == value)) // NaN
            return 0;
        value = std::clamp(value, -1.0f, 1.0f);
        return static_cast<std::int8_t>(std::lround(value * 127.0f));
    }

    std::size_t encode_header(std::uint8_t *out, std::uint8_t type, std::uint32_t session) {
        std::uint8_t *cursor = out;
        put_u32(cursor, packet_magic);
        put_u8(cursor, type);
        put_u32(cursor, session);
        return static_cast<std::size_t>(cursor - out);
    }

    bool decode_header(const std::uint8_t *data, std::size_t size, PacketHeader &out) {
        Reader reader{data, data + size};
        if (reader.u32() != packet_magic)
            return false;
        out.type = reader.u8();
        out.session = reader.u32();
        return reader.ok;
    }

    std::size_t encode_input_packet(std::uint8_t *out, std::uint32_t session, const InputPacket &packet) {
        const std::size_t count = std::min(packet.inputs.size(), max_inputs_per_packet);
        std::uint8_t *cursor = out + encode_header(out, packet_inputs, session);
        put_u32(cursor, packet.ack);
        put_u32(cursor, packet.first_frame);
        put_u8(cursor, static_cast<std::uint8_t>(count));
        for (std::size_t i = 0; i < count; ++i) {
            put_u32(cursor, packet.inputs[i].buttons);
            put_u8(cursor, static_cast<std::uint8_t>(packet.inputs[i].left_x));
            put_u8(cursor, static_cast<std::uint8_t>(packet.inputs[i].left_y));
        }
        put_u8(cursor, packet.checkpoint ? 1 : 0);
        put_u32(cursor, packet.checkpoint ? packet.checkpoint->frame : 0);
        put_u32(cursor, packet.checkpoint ? packet.checkpoint->hash : 0);
        put_u32(cursor, packet.send_time_ms);
        put_u32(cursor, packet.echo_time_ms);
        return static_cast<std::size_t>(cursor - out);
    }

    bool decode_input_packet(const std::uint8_t *data, std::size_t size, InputPacket &out) {
        if (size < header_size)
            return false;
        Reader reader{data + header_size, data + size};
        out.ack = reader.u32();
        out.first_frame = reader.u32();
        const std::uint8_t count = reader.u8();
        if (!reader.ok || count > max_inputs_per_packet)
            return false;
        out.inputs.resize(count);
        for (NetInput &input : out.inputs) {
            input.buttons = reader.u32();
            input.left_x = static_cast<std::int8_t>(reader.u8());
            input.left_y = static_cast<std::int8_t>(reader.u8());
        }
        const bool has_checkpoint = reader.u8() != 0;
        Checkpoint checkpoint;
        checkpoint.frame = reader.u32();
        checkpoint.hash = reader.u32();
        out.checkpoint = has_checkpoint ? std::optional<Checkpoint>(checkpoint) : std::nullopt;
        out.send_time_ms = reader.u32();
        out.echo_time_ms = reader.u32();
        return reader.ok;
    }

    Lockstep::Lockstep(std::uint32_t delay)
        : delay_(std::max<std::uint32_t>(1, delay)), local_count_(delay_), remote_count_(delay_), peer_ack_(delay_) {
        // The first `delay` frames have no real samples; both sides agree they are neutral.
        for (std::uint32_t frame = 0; frame < delay_; ++frame) {
            local_[frame % ring_size] = Slot{frame, NetInput{}};
            remote_[frame % ring_size] = Slot{frame, NetInput{}};
        }
    }

    bool Lockstep::submit_local(std::uint32_t frame, const NetInput &input) {
        const std::uint32_t target = frame + delay_;
        if (target != local_count_ || local_count_ - peer_ack_ >= ring_size)
            return false;
        local_[target % ring_size] = Slot{target, input};
        ++local_count_;
        return true;
    }

    NetInput Lockstep::local_input(std::uint32_t frame) const {
        const Slot &slot = local_[frame % ring_size];
        return slot.frame == frame ? slot.input : NetInput{};
    }

    NetInput Lockstep::remote_input(std::uint32_t frame) const {
        const Slot &slot = remote_[frame % ring_size];
        return slot.frame == frame ? slot.input : NetInput{};
    }

    InputPacket Lockstep::make_packet(std::uint32_t now_ms) const {
        InputPacket packet;
        packet.ack = remote_count_;
        packet.first_frame = peer_ack_;
        const std::uint32_t last = std::min<std::uint32_t>(local_count_, peer_ack_ + max_inputs_per_packet);
        for (std::uint32_t frame = peer_ack_; frame < last; ++frame)
            packet.inputs.push_back(local_input(frame));
        packet.checkpoint = latest_local_checkpoint_;
        packet.send_time_ms = now_ms;
        packet.echo_time_ms = last_remote_send_time_;
        return packet;
    }

    bool Lockstep::receive(const InputPacket &packet, std::uint32_t now_ms) {
        if (packet.ack > local_count_)
            return false; // acknowledges frames we never sent

        for (std::size_t i = 0; i < packet.inputs.size(); ++i) {
            const std::uint32_t frame = packet.first_frame + static_cast<std::uint32_t>(i);
            if (frame < remote_count_ || frame >= remote_count_ + ring_size)
                continue;
            remote_[frame % ring_size] = Slot{frame, packet.inputs[i]};
        }
        while (remote_[remote_count_ % ring_size].frame == remote_count_)
            ++remote_count_;

        peer_ack_ = std::max(peer_ack_, packet.ack);

        if (packet.checkpoint) {
            const Checkpoint &checkpoint = *packet.checkpoint;
            const Checkpoint &last =
                remote_checkpoints_[(remote_checkpoint_count_ + checkpoint_ring - 1) % checkpoint_ring];
            if (remote_checkpoint_count_ == 0 || last.frame != checkpoint.frame) {
                remote_checkpoints_[remote_checkpoint_count_ % checkpoint_ring] = checkpoint;
                ++remote_checkpoint_count_;
                compare_checkpoints();
            }
        }

        if (packet.send_time_ms != 0)
            last_remote_send_time_ = packet.send_time_ms;
        if (packet.echo_time_ms != 0) {
            const std::uint32_t sample = now_ms - packet.echo_time_ms;
            if (sample < 10000)
                rtt_ms_ = rtt_ms_ == 0 ? sample : (rtt_ms_ * 7 + sample) / 8;
        }
        return true;
    }

    void Lockstep::add_local_checkpoint(Checkpoint checkpoint) {
        local_checkpoints_[local_checkpoint_count_ % checkpoint_ring] = checkpoint;
        ++local_checkpoint_count_;
        latest_local_checkpoint_ = checkpoint;
        compare_checkpoints();
    }

    void Lockstep::compare_checkpoints() {
        const std::size_t local_n = std::min(local_checkpoint_count_, checkpoint_ring);
        const std::size_t remote_n = std::min(remote_checkpoint_count_, checkpoint_ring);
        for (std::size_t r = 0; r < remote_n; ++r) {
            const Checkpoint &remote = remote_checkpoints_[r];
            if (last_compared_frame_ != UINT32_MAX && remote.frame <= last_compared_frame_)
                continue;
            for (std::size_t l = 0; l < local_n; ++l) {
                const Checkpoint &local = local_checkpoints_[l];
                if (local.frame != remote.frame)
                    continue;
                last_compared_frame_ = remote.frame;
                if (local.hash == remote.hash) {
                    ++matched_checkpoints_;
                } else if (desync_ == DesyncState::in_sync) {
                    desync_ = DesyncState::desynced;
                    desync_frame_ = remote.frame;
                }
                break;
            }
        }
    }

} // namespace saga::netplay
