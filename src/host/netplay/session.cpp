#include "host/netplay/session.hpp"

#include "host/netplay/game_bridge.hpp"
#include "host/netplay/lockstep.hpp"
#include "host/netplay/net_socket.hpp"
#include "host/platform/input.hpp"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace saga::netplay {
    namespace {

        namespace fs = std::filesystem;

        constexpr std::uint32_t protocol_version = 1;
        constexpr int checkpoint_interval = 60;          // frames between state checksums
        constexpr std::uint64_t disconnect_after_ms = 15000;
        constexpr std::uint64_t resend_interval_ms = 16;
        constexpr std::uint64_t max_save_bytes = 32u << 20;

        // Lobby message types (TCP, length-prefixed).
        enum Message : std::uint8_t {
            msg_hello = 1,
            msg_welcome = 2,
            msg_reject = 3,
            msg_file = 4,
            msg_files_done = 5,
            msg_ping = 6,
            msg_pong = 7,
            msg_start = 8,
        };

        // ------------------------------------------------------------ helpers

        struct Writer final {
            std::vector<std::uint8_t> bytes;
            void u8(std::uint8_t value) { bytes.push_back(value); }
            void u16(std::uint16_t value) {
                u8(static_cast<std::uint8_t>(value));
                u8(static_cast<std::uint8_t>(value >> 8));
            }
            void u32(std::uint32_t value) {
                for (int i = 0; i < 4; ++i)
                    u8(static_cast<std::uint8_t>(value >> (8 * i)));
            }
            void u64(std::uint64_t value) {
                u32(static_cast<std::uint32_t>(value));
                u32(static_cast<std::uint32_t>(value >> 32));
            }
            void text(const std::string &value) {
                u16(static_cast<std::uint16_t>(value.size()));
                bytes.insert(bytes.end(), value.begin(), value.end());
            }
            void blob(const std::vector<std::uint8_t> &value) {
                u32(static_cast<std::uint32_t>(value.size()));
                bytes.insert(bytes.end(), value.begin(), value.end());
            }
        };

        struct Reader final {
            const std::vector<std::uint8_t> &bytes;
            std::size_t at = 0;
            bool ok = true;
            std::uint8_t u8() {
                if (at >= bytes.size()) {
                    ok = false;
                    return 0;
                }
                return bytes[at++];
            }
            std::uint16_t u16() { return static_cast<std::uint16_t>(u8() | (u8() << 8)); }
            std::uint32_t u32() {
                std::uint32_t value = 0;
                for (int i = 0; i < 4; ++i)
                    value |= static_cast<std::uint32_t>(u8()) << (8 * i);
                return value;
            }
            std::uint64_t u64() {
                const std::uint64_t low = u32();
                return low | (static_cast<std::uint64_t>(u32()) << 32);
            }
            std::string text() {
                const std::uint16_t size = u16();
                if (at + size > bytes.size()) {
                    ok = false;
                    return {};
                }
                std::string value(bytes.begin() + static_cast<std::ptrdiff_t>(at),
                                  bytes.begin() + static_cast<std::ptrdiff_t>(at + size));
                at += size;
                return value;
            }
            std::vector<std::uint8_t> blob() {
                const std::uint32_t size = u32();
                if (at + size > bytes.size()) {
                    ok = false;
                    return {};
                }
                std::vector<std::uint8_t> value(bytes.begin() + static_cast<std::ptrdiff_t>(at),
                                                bytes.begin() + static_cast<std::ptrdiff_t>(at + size));
                at += size;
                return value;
            }
        };

        bool send_message(TcpStream &stream, const Writer &message) {
            std::uint8_t length[4];
            const auto size = static_cast<std::uint32_t>(message.bytes.size());
            for (int i = 0; i < 4; ++i)
                length[i] = static_cast<std::uint8_t>(size >> (8 * i));
            return stream.send_all(length, 4) && stream.send_all(message.bytes.data(), message.bytes.size());
        }

        bool receive_message(TcpStream &stream, std::vector<std::uint8_t> &message, int timeout_ms) {
            std::uint8_t length[4];
            if (!stream.receive_all(length, 4, timeout_ms))
                return false;
            const std::uint32_t size = static_cast<std::uint32_t>(length[0]) | (length[1] << 8) | (length[2] << 16) |
                                       (static_cast<std::uint32_t>(length[3]) << 24);
            if (size == 0 || size > max_save_bytes + 1024)
                return false;
            message.resize(size);
            return stream.receive_all(message.data(), size, timeout_ms);
        }

        std::uint64_t fnv64(std::uint64_t hash, const std::uint8_t *data, std::size_t size) {
            for (std::size_t i = 0; i < size; ++i) {
                hash ^= data[i];
                hash *= 1099511628211ull;
            }
            return hash;
        }

        // Hashes a whole file, or for big files its size plus first and last 8 MiB.
        std::uint64_t file_fingerprint(const std::string &path) {
            std::ifstream file(path, std::ios::binary);
            if (!file)
                return 0;
            file.seekg(0, std::ios::end);
            const std::uint64_t size = static_cast<std::uint64_t>(file.tellg());
            std::uint64_t hash = fnv64(1469598103934665603ull, reinterpret_cast<const std::uint8_t *>(&size), 8);
            constexpr std::uint64_t window = 8u << 20;
            std::vector<std::uint8_t> buffer(1 << 20);
            auto hash_range = [&](std::uint64_t start, std::uint64_t length) {
                file.clear();
                file.seekg(static_cast<std::streamoff>(start));
                while (length > 0) {
                    const auto chunk = static_cast<std::size_t>(std::min<std::uint64_t>(length, buffer.size()));
                    file.read(reinterpret_cast<char *>(buffer.data()), static_cast<std::streamsize>(chunk));
                    const auto got = static_cast<std::size_t>(file.gcount());
                    if (got == 0)
                        break;
                    hash = fnv64(hash, buffer.data(), got);
                    length -= got;
                }
            };
            if (size <= 2 * window) {
                hash_range(0, size);
            } else {
                hash_range(0, window);
                hash_range(size - window, window);
            }
            return hash;
        }

        std::string executable_path() {
#ifdef _WIN32
            char path[MAX_PATH] = {};
            GetModuleFileNameA(nullptr, path, MAX_PATH);
            return path;
#else
            char path[4096] = {};
            const ssize_t length = readlink("/proc/self/exe", path, sizeof(path) - 1);
            return length > 0 ? std::string(path, static_cast<std::size_t>(length)) : std::string();
#endif
        }

        std::string with_slash(std::string path) {
            if (!path.empty() && path.back() != '/' && path.back() != '\\')
                path += '/';
            return path;
        }

        bool safe_file_name(const std::string &name) {
            return !name.empty() && name.size() < 128 && name.find_first_of("/\\:") == std::string::npos &&
                   name != "." && name != "..";
        }

        std::uint32_t automatic_delay(std::uint32_t rtt_ms) {
            // One-way latency in frames, plus two frames of slack for jitter.
            const std::uint32_t one_way_frames = (rtt_ms / 2 + 16) / 17;
            return std::clamp<std::uint32_t>(one_way_frames + 2, 2, 12);
        }

        // ------------------------------------------------------------ session state

        struct ReplayData final {
            std::vector<NetInput> port0;
            std::vector<NetInput> port1;
            std::map<std::uint32_t, StateHash> checkpoints;
        };

        struct Session final {
            Config config;
            std::atomic<Phase> phase{Phase::idle};
            std::atomic<bool> stop_requested{false};
            std::thread lobby;

            std::mutex text_mutex;
            std::string status;
            std::string failure;
            std::string documents;

            // Network play.
            bool is_host = false;
            UdpSocket udp;
            NetAddress remote{};
            std::uint32_t session_id = 0;
            std::uint32_t delay = 0;
            std::unique_ptr<Lockstep> lockstep;
            std::uint64_t last_receive_ms = 0;
            std::uint64_t last_send_ms = 0;
            bool disconnected = false;
            bool desync_logged = false;

            // Frame driver.
            std::uint32_t frame = 0;
            std::FILE *log = nullptr;
            std::FILE *record = nullptr;
            ReplayData replay;
            std::uint32_t replay_matched = 0;
            bool replay_mismatch = false;
            std::uint32_t replay_mismatch_frame = 0;
        };

        Session &session() {
            static Session instance;
            return instance;
        }

        void set_status(const std::string &text) {
            Session &s = session();
            std::lock_guard lock(s.text_mutex);
            s.status = text;
        }

        void fail(const std::string &text) {
            Session &s = session();
            {
                std::lock_guard lock(s.text_mutex);
                s.failure = text;
                s.status = "Online co-op failed: " + text;
            }
            s.phase = Phase::failed;
        }

        void log_line(const char *format, ...) {
            Session &s = session();
            if (s.log == nullptr)
                return;
            const std::time_t now = std::time(nullptr);
            char stamp[32];
            std::strftime(stamp, sizeof(stamp), "%H:%M:%S", std::localtime(&now));
            std::fprintf(s.log, "[%s] ", stamp);
            va_list args;
            va_start(args, format);
            std::vfprintf(s.log, format, args);
            va_end(args);
            std::fputc('\n', s.log);
            std::fflush(s.log);
        }

        void open_log() {
            Session &s = session();
            const std::string path = with_slash(s.config.documents_path) + "netplay.log";
            s.log = std::fopen(path.c_str(), "w");
            const char *mode_name = "off";
            switch (s.config.mode) {
                case Mode::host: mode_name = "host"; break;
                case Mode::join: mode_name = "join"; break;
                case Mode::record: mode_name = "record"; break;
                case Mode::replay: mode_name = "replay"; break;
                case Mode::off: break;
            }
            log_line("netplay protocol %u, mode %s", protocol_version, mode_name);
        }

        // ------------------------------------------------------------ lobby: host

        std::vector<std::pair<std::string, std::vector<std::uint8_t>>> read_saves(const std::string &documents) {
            std::vector<std::pair<std::string, std::vector<std::uint8_t>>> files;
            const fs::path folder = fs::path(with_slash(documents)) / "SavedGames";
            std::error_code error;
            if (!fs::is_directory(folder, error))
                return files;
            std::uint64_t total = 0;
            for (const auto &entry : fs::directory_iterator(folder, error)) {
                if (!entry.is_regular_file(error))
                    continue;
                const std::string name = entry.path().filename().string();
                if (!safe_file_name(name))
                    continue;
                std::ifstream file(entry.path(), std::ios::binary);
                std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(file)), {});
                total += data.size();
                if (total > max_save_bytes)
                    break;
                files.emplace_back(name, std::move(data));
            }
            std::sort(files.begin(), files.end());
            return files;
        }

        bool host_handshake(TcpStream &stream, std::uint64_t exe_print, std::uint64_t obb_print) {
            Session &s = session();
            std::vector<std::uint8_t> message;
            if (!receive_message(stream, message, 10000) || message.empty() || message[0] != msg_hello) {
                log_line("ignored a connection from %s that did not say hello", stream.peer().to_string().c_str());
                return false;
            }
            Reader reader{message, 1};
            const std::uint32_t their_protocol = reader.u32();
            const std::uint64_t their_exe = reader.u64();
            const std::uint64_t their_obb = reader.u64();
            if (!reader.ok)
                return false;

            std::string problem;
            if (their_protocol != protocol_version || their_exe != exe_print)
                problem = "you and your friend are running different versions of the game. Use the exact same "
                          "download on both PCs.";
            else if (their_obb != obb_print)
                problem = "your game data files (.obb) are different. Both PCs need the same "
                          "main.1060.com.wb.lego.tcs.obb.";
            if (!problem.empty()) {
                Writer reject;
                reject.u8(msg_reject);
                reject.text(problem);
                send_message(stream, reject);
                log_line("rejected %s: %s", stream.peer().to_string().c_str(), problem.c_str());
                set_status("Online co-op: a friend tried to join but " + problem);
                return false;
            }

            s.session_id = std::random_device{}() | 1u;
            Writer welcome;
            welcome.u8(msg_welcome);
            welcome.u32(s.session_id);
            welcome.u16(s.config.port);
            if (!send_message(stream, welcome))
                return false;

            set_status("Online co-op: friend connected, sending your save files...");
            const auto saves = read_saves(s.config.documents_path);
            for (const auto &[name, data] : saves) {
                Writer file;
                file.u8(msg_file);
                file.text(name);
                file.blob(data);
                if (!send_message(stream, file))
                    return false;
            }
            Writer done;
            done.u8(msg_files_done);
            if (!send_message(stream, done))
                return false;
            log_line("sent %zu save files", saves.size());

            set_status("Online co-op: measuring connection...");
            std::vector<std::uint32_t> samples;
            for (int i = 0; i < 7; ++i) {
                Writer ping;
                ping.u8(msg_ping);
                ping.u64(net_now_ms());
                const std::uint64_t sent = net_now_ms();
                if (!send_message(stream, ping) || !receive_message(stream, message, 5000) || message[0] != msg_pong)
                    return false;
                samples.push_back(static_cast<std::uint32_t>(net_now_ms() - sent));
            }
            std::sort(samples.begin(), samples.end());
            const std::uint32_t rtt = samples[samples.size() / 2];
            s.delay = s.config.delay > 0 ? static_cast<std::uint32_t>(s.config.delay) : automatic_delay(rtt);
            log_line("ping %u ms, input delay %u frames", rtt, s.delay);

            Writer start_message;
            start_message.u8(msg_start);
            start_message.u8(static_cast<std::uint8_t>(s.delay));
            return send_message(stream, start_message);
        }

        void host_lobby() {
            Session &s = session();
            const std::uint64_t exe_print = file_fingerprint(executable_path());
            const std::uint64_t obb_print = file_fingerprint(s.config.obb_path);
            std::string error;
            TcpListener listener;
            if (!listener.open(s.config.port, error) || !s.udp.open(s.config.port, error)) {
                fail(error);
                return;
            }
            set_status("Online co-op: waiting for your friend to join (port " + std::to_string(s.config.port) +
                       ")...");
            log_line("listening on port %u", s.config.port);

            while (!s.stop_requested) {
                TcpStream stream = listener.accept(250);
                if (!stream.is_open())
                    continue;
                log_line("connection from %s", stream.peer().to_string().c_str());
                if (!host_handshake(stream, exe_print, obb_print))
                    continue;

                // The guest now opens its UDP path to us (this also opens its NAT).
                set_status("Online co-op: starting...");
                const std::uint64_t deadline = net_now_ms() + 10000;
                std::uint8_t buffer[max_packet_size];
                bool joined = false;
                while (!joined && net_now_ms() < deadline && !s.stop_requested) {
                    NetAddress from;
                    const int size = s.udp.receive(buffer, sizeof(buffer), from, 100);
                    PacketHeader header;
                    if (size > 0 && from.same_host(stream.peer()) &&
                        decode_header(buffer, static_cast<std::size_t>(size), header) &&
                        header.type == packet_join && header.session == s.session_id) {
                        s.remote = from;
                        joined = true;
                    }
                }
                if (!joined) {
                    log_line("friend never reached UDP port %u", s.config.port);
                    set_status("Online co-op: your friend connected but game traffic (UDP port " +
                               std::to_string(s.config.port) + ") is blocked. Forward UDP too, then try again.");
                    continue;
                }
                for (int i = 0; i < 3; ++i) {
                    const std::size_t size = encode_header(buffer, packet_joined, s.session_id);
                    s.udp.send_to(s.remote, buffer, size);
                }
                log_line("friend joined from %s", s.remote.to_string().c_str());
                s.is_host = true;
                s.phase = Phase::ready;
                return;
            }
        }

        // ------------------------------------------------------------ lobby: guest

        void join_lobby() {
            Session &s = session();
            NetAddress host;
            std::string error;
            if (!net_resolve(s.config.address, 7777, host, error)) {
                fail(error);
                return;
            }
            const std::uint64_t exe_print = file_fingerprint(executable_path());
            const std::uint64_t obb_print = file_fingerprint(s.config.obb_path);

            TcpStream stream;
            const std::uint64_t give_up = net_now_ms() + 30000;
            set_status("Online co-op: connecting to " + host.to_string() + "...");
            while (!stream.connect(host, 3000, error)) {
                if (net_now_ms() > give_up || s.stop_requested) {
                    fail(error);
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
            log_line("connected to %s", host.to_string().c_str());

            Writer hello;
            hello.u8(msg_hello);
            hello.u32(protocol_version);
            hello.u64(exe_print);
            hello.u64(obb_print);
            if (!send_message(stream, hello)) {
                fail("the host closed the connection");
                return;
            }

            // Boot from an exact copy of the host's saves; this PC's own saves are untouched.
            const fs::path guest_folder = fs::path(with_slash(s.config.documents_path)) / "netplay-guest";
            const fs::path saves_folder = guest_folder / "SavedGames";
            std::error_code fs_error;
            fs::remove_all(saves_folder, fs_error);
            fs::create_directories(saves_folder, fs_error);

            std::uint16_t udp_port = 0;
            std::vector<std::uint8_t> message;
            bool started = false;
            set_status("Online co-op: receiving the host's save files...");
            while (!started) {
                if (!receive_message(stream, message, 20000)) {
                    fail("lost the connection to the host while starting");
                    return;
                }
                Reader reader{message, 1};
                switch (message[0]) {
                    case msg_reject:
                        fail(reader.text());
                        return;
                    case msg_welcome:
                        s.session_id = reader.u32();
                        udp_port = reader.u16();
                        break;
                    case msg_file: {
                        const std::string name = reader.text();
                        const std::vector<std::uint8_t> data = reader.blob();
                        if (reader.ok && safe_file_name(name)) {
                            std::ofstream file(saves_folder / name, std::ios::binary);
                            file.write(reinterpret_cast<const char *>(data.data()),
                                       static_cast<std::streamsize>(data.size()));
                        }
                        break;
                    }
                    case msg_files_done:
                        set_status("Online co-op: measuring connection...");
                        break;
                    case msg_ping: {
                        Writer pong;
                        pong.u8(msg_pong);
                        pong.u64(reader.u64());
                        send_message(stream, pong);
                        break;
                    }
                    case msg_start:
                        s.delay = reader.u8();
                        started = true;
                        break;
                    default:
                        break;
                }
            }
            {
                std::lock_guard lock(s.text_mutex);
                s.documents = with_slash(guest_folder.generic_string());
            }

            if (!s.udp.open(0, error)) {
                fail(error);
                return;
            }
            s.remote = NetAddress{host.ip, udp_port};
            const std::uint64_t deadline = net_now_ms() + 10000;
            std::uint8_t buffer[max_packet_size];
            while (net_now_ms() < deadline && !s.stop_requested) {
                const std::size_t size = encode_header(buffer, packet_join, s.session_id);
                s.udp.send_to(s.remote, buffer, size);
                NetAddress from;
                const int got = s.udp.receive(buffer, sizeof(buffer), from, 100);
                PacketHeader header;
                if (got > 0 && from.same_host(host) && decode_header(buffer, static_cast<std::size_t>(got), header) &&
                    header.type == packet_joined && header.session == s.session_id) {
                    log_line("joined, input delay %u frames", s.delay);
                    s.is_host = false;
                    s.phase = Phase::ready;
                    return;
                }
            }
            fail("the host's game traffic (UDP port " + std::to_string(udp_port) +
                 ") is blocked. The host must forward UDP as well as TCP.");
        }

        // ------------------------------------------------------------ frame driver

        void write_record_frame(std::uint32_t frame, const NetInput &a, const NetInput &b) {
            Session &s = session();
            std::uint8_t entry[17];
            entry[0] = 1;
            std::memcpy(entry + 1, &frame, 4);
            std::memcpy(entry + 5, &a.buttons, 4);
            entry[9] = static_cast<std::uint8_t>(a.left_x);
            entry[10] = static_cast<std::uint8_t>(a.left_y);
            std::memcpy(entry + 11, &b.buttons, 4);
            entry[15] = static_cast<std::uint8_t>(b.left_x);
            entry[16] = static_cast<std::uint8_t>(b.left_y);
            std::fwrite(entry, 1, sizeof(entry), s.record);
        }

        void write_record_checkpoint(std::uint32_t frame, const StateHash &hash) {
            Session &s = session();
            const std::uint32_t words[7] = {frame, hash.total, hash.rng, hash.players, hash.world, hash.timer, 0};
            const std::uint8_t tag = 2;
            std::fwrite(&tag, 1, 1, s.record);
            std::fwrite(words, 4, 6, s.record);
            std::fflush(s.record);
        }

        bool load_replay(const std::string &path, ReplayData &out, std::string &error) {
            std::ifstream file(path, std::ios::binary);
            if (!file) {
                error = "could not open " + path;
                return false;
            }
            char magic[8] = {};
            file.read(magic, 8);
            if (std::memcmp(magic, "TCSR\x01\0\0\0", 8) != 0) {
                error = path + " is not a CouchSaga input recording";
                return false;
            }
            std::uint8_t tag = 0;
            while (file.read(reinterpret_cast<char *>(&tag), 1)) {
                if (tag == 1) {
                    std::uint8_t entry[16];
                    if (!file.read(reinterpret_cast<char *>(entry), 16))
                        break;
                    std::uint32_t frame;
                    std::memcpy(&frame, entry, 4);
                    if (frame != out.port0.size())
                        break;
                    NetInput a;
                    NetInput b;
                    std::memcpy(&a.buttons, entry + 4, 4);
                    a.left_x = static_cast<std::int8_t>(entry[8]);
                    a.left_y = static_cast<std::int8_t>(entry[9]);
                    std::memcpy(&b.buttons, entry + 10, 4);
                    b.left_x = static_cast<std::int8_t>(entry[14]);
                    b.left_y = static_cast<std::int8_t>(entry[15]);
                    out.port0.push_back(a);
                    out.port1.push_back(b);
                } else if (tag == 2) {
                    std::uint32_t words[6];
                    if (!file.read(reinterpret_cast<char *>(words), sizeof(words)))
                        break;
                    out.checkpoints[words[0]] = StateHash{words[1], words[2], words[3], words[4], words[5]};
                } else {
                    break;
                }
            }
            return true;
        }

        NetInput sample(int port) {
            std::uint32_t buttons = 0;
            float x = 0.0f;
            float y = 0.0f;
            HostInputSampleLocal(port, &buttons, &x, &y);
            return NetInput{buttons, NetInput::quantize_axis(x), NetInput::quantize_axis(y)};
        }

        void apply(const NetInput &port0, const NetInput &port1) {
            HostInputSetNetFrame(0, port0.buttons, NetInput::axis_value(port0.left_x), NetInput::axis_value(port0.left_y));
            HostInputSetNetFrame(1, port1.buttons, NetInput::axis_value(port1.left_x), NetInput::axis_value(port1.left_y));
        }

        void send_inputs() {
            Session &s = session();
            std::uint8_t buffer[max_packet_size];
            const auto now = static_cast<std::uint32_t>(net_now_ms());
            const std::size_t size = encode_input_packet(buffer, s.session_id, s.lockstep->make_packet(now | 1u));
            s.udp.send_to(s.remote, buffer, size);
            s.last_send_ms = net_now_ms();
        }

        void mark_disconnected(const char *why) {
            Session &s = session();
            if (s.disconnected || s.lockstep == nullptr)
                return;
            s.disconnected = true;
            log_line("friend disconnected at frame %u: %s", s.frame, why);
            set_status("Online co-op: your friend disconnected. You can keep playing; player 2 stands still.");
        }

        void receive_inputs(int first_timeout_ms) {
            Session &s = session();
            std::uint8_t buffer[max_packet_size];
            int timeout = first_timeout_ms;
            for (;;) {
                NetAddress from;
                const int size = s.udp.receive(buffer, sizeof(buffer), from, timeout);
                timeout = 0;
                if (size <= 0)
                    return;
                PacketHeader header;
                if (!from.same_host(s.remote) || !decode_header(buffer, static_cast<std::size_t>(size), header) ||
                    header.session != s.session_id)
                    continue;
                if (header.type == packet_join && s.is_host) {
                    // The guest has not seen our reply yet.
                    const std::size_t reply = encode_header(buffer, packet_joined, s.session_id);
                    s.udp.send_to(from, buffer, reply);
                    continue;
                }
                if (header.type == packet_bye) {
                    mark_disconnected("they closed the game");
                    return;
                }
                InputPacket packet;
                if (header.type == packet_inputs && decode_input_packet(buffer, static_cast<std::size_t>(size), packet) &&
                    s.lockstep->receive(packet, static_cast<std::uint32_t>(net_now_ms()))) {
                    s.remote.port = from.port; // follow NAT rebinding
                    s.last_receive_ms = net_now_ms();
                }
            }
        }

        void update_network_status() {
            Session &s = session();
            if (s.disconnected)
                return;
            const Lockstep &lockstep = *s.lockstep;
            std::string text = "Online co-op: " + std::string(s.is_host ? "hosting" : "joined") + " | ping " +
                               std::to_string(lockstep.rtt_ms()) + " ms | delay " + std::to_string(lockstep.delay());
            if (lockstep.desync_state() == DesyncState::desynced)
                text += " | OUT OF SYNC since frame " + std::to_string(lockstep.desync_frame()) +
                        " (send both netplay.log files)";
            else
                text += " | in sync";
            set_status(text);
        }

        void network_frame(std::uint32_t frame, const NetInput &local) {
            Session &s = session();
            NetInput mine = local;
            NetInput theirs{};
            if (!s.disconnected && !s.lockstep->submit_local(frame, local))
                mark_disconnected("the other game stopped acknowledging inputs");
            if (!s.disconnected) {
                receive_inputs(0);
                send_inputs();
                const std::uint64_t wait_start = net_now_ms();
                bool waiting_shown = false;
                while (!s.lockstep->ready(frame)) {
                    receive_inputs(2);
                    const std::uint64_t now = net_now_ms();
                    if (now - s.last_send_ms >= resend_interval_ms)
                        send_inputs();
                    if (!waiting_shown && now - wait_start > 400) {
                        set_status("Online co-op: waiting for your friend...");
                        waiting_shown = true;
                    }
                    if (now - s.last_receive_ms > disconnect_after_ms) {
                        mark_disconnected("no packets for 15 seconds");
                        break;
                    }
                    if (s.stop_requested) {
                        mark_disconnected("this game is closing");
                        break;
                    }
                }
                if (waiting_shown)
                    log_line("waited %llu ms for frame %u", static_cast<unsigned long long>(net_now_ms() - wait_start),
                             frame);
            }
            if (!s.disconnected) {
                mine = s.lockstep->local_input(frame);
                theirs = s.lockstep->remote_input(frame);
                if (s.lockstep->desync_state() == DesyncState::desynced && !s.desync_logged) {
                    s.desync_logged = true;
                    log_line("OUT OF SYNC: checksums differ at frame %u", s.lockstep->desync_frame());
                }
            }
            apply(s.is_host ? mine : theirs, s.is_host ? theirs : mine);
        }

        // Game-thread entry point, once per engine frame.
        void lockstep_frame() {
            Session &s = session();
            const std::uint32_t frame = s.frame++;

            if (frame % checkpoint_interval == 0) {
                const StateHash hash = hash_game_state();
                log_line("frame %u %s", frame, hash.describe().c_str());
                switch (s.config.mode) {
                    case Mode::host:
                    case Mode::join:
                        if (!s.disconnected)
                            s.lockstep->add_local_checkpoint(Checkpoint{frame, hash.total});
                        break;
                    case Mode::record:
                        write_record_checkpoint(frame, hash);
                        break;
                    case Mode::replay: {
                        const auto expected = s.replay.checkpoints.find(frame);
                        if (expected != s.replay.checkpoints.end() && !s.replay_mismatch) {
                            if (expected->second.total == hash.total) {
                                ++s.replay_matched;
                            } else {
                                s.replay_mismatch = true;
                                s.replay_mismatch_frame = frame;
                                log_line("REPLAY MISMATCH at frame %u", frame);
                                log_line("  recorded %s", expected->second.describe().c_str());
                                log_line("  replayed %s", hash.describe().c_str());
                            }
                        }
                        break;
                    }
                    case Mode::off:
                        break;
                }
            }

            const NetInput local = sample(0);
            switch (s.config.mode) {
                case Mode::host:
                case Mode::join:
                    network_frame(frame, local);
                    if (frame % 30 == 0)
                        update_network_status();
                    break;
                case Mode::record: {
                    const NetInput second = sample(1);
                    write_record_frame(frame, local, second);
                    apply(local, second);
                    if (frame % 60 == 0)
                        set_status("Recording inputs: frame " + std::to_string(frame));
                    break;
                }
                case Mode::replay: {
                    if (frame < s.replay.port0.size()) {
                        apply(s.replay.port0[frame], s.replay.port1[frame]);
                    } else {
                        apply(local, sample(1));
                    }
                    if (frame % 30 == 0) {
                        std::string text;
                        if (s.replay_mismatch)
                            text = "Replay: game state DIFFERS from the recording at frame " +
                                   std::to_string(s.replay_mismatch_frame) + " (see netplay.log)";
                        else if (frame < s.replay.port0.size())
                            text = "Replaying: " + std::to_string(s.replay_matched) + " checkpoints match so far";
                        else
                            text = "Replay finished: all " + std::to_string(s.replay_matched) +
                                   " checkpoints matched. The game is deterministic here.";
                        set_status(text);
                    }
                    if (frame == s.replay.port0.size())
                        log_line("replay finished: %u checkpoints matched, mismatch=%d", s.replay_matched,
                                 s.replay_mismatch ? 1 : 0);
                    break;
                }
                case Mode::off:
                    break;
            }
        }

    } // namespace

    void start(const Config &config) {
        Session &s = session();
        s.config = config;
        s.config.documents_path = with_slash(config.documents_path);
        {
            std::lock_guard lock(s.text_mutex);
            s.documents = s.config.documents_path;
            s.failure.clear();
        }
        if (config.mode == Mode::off)
            return;
        open_log();
        net_startup();

        switch (config.mode) {
            case Mode::host:
                s.phase = Phase::connecting;
                s.lobby = std::thread(host_lobby);
                break;
            case Mode::join:
                s.phase = Phase::connecting;
                s.lobby = std::thread(join_lobby);
                break;
            case Mode::record:
                s.record = std::fopen(config.file.c_str(), "wb");
                if (s.record == nullptr) {
                    fail("could not create " + config.file);
                    return;
                }
                std::fwrite("TCSR\x01\0\0\0", 1, 8, s.record);
                s.phase = Phase::ready;
                break;
            case Mode::replay: {
                std::string error;
                if (!load_replay(config.file, s.replay, error)) {
                    fail(error);
                    return;
                }
                log_line("replaying %zu frames, %zu checkpoints", s.replay.port0.size(), s.replay.checkpoints.size());
                s.phase = Phase::ready;
                break;
            }
            case Mode::off:
                break;
        }
    }

    Phase phase() { return session().phase.load(); }

    std::string status_text() {
        Session &s = session();
        std::lock_guard lock(s.text_mutex);
        return s.status;
    }

    std::string failure_text() {
        Session &s = session();
        std::lock_guard lock(s.text_mutex);
        return s.failure;
    }

    std::string documents_path() {
        Session &s = session();
        std::lock_guard lock(s.text_mutex);
        return s.documents;
    }

    void begin_game() {
        Session &s = session();
        if (s.config.mode == Mode::off || s.phase != Phase::ready)
            return;
        if (s.lobby.joinable())
            s.lobby.join();
        if (s.config.mode == Mode::host || s.config.mode == Mode::join) {
            s.lockstep = std::make_unique<Lockstep>(s.delay);
            s.last_receive_ms = net_now_ms();
            update_network_status();
        }
        apply_deterministic_engine_settings();
        // Until the first lockstep frame, both games see the same neutral input
        // (not whatever happened to be held while the engine booted).
        apply(NetInput{}, NetInput{});
        HostInputSetLockstepFrame(lockstep_frame);
        s.phase = Phase::playing;
        log_line("game started");
    }

    void stop() {
        Session &s = session();
        s.stop_requested = true;
        // The lobby may be blocked on a network read; the process is exiting anyway.
        if (s.lobby.joinable())
            s.lobby.detach();
        if (s.udp.is_open() && s.session_id != 0 && s.phase == Phase::playing) {
            std::uint8_t buffer[16];
            const std::size_t size = encode_header(buffer, packet_bye, s.session_id);
            for (int i = 0; i < 3; ++i)
                s.udp.send_to(s.remote, buffer, size);
        }
        // The game thread may still be inside a frame, so flush rather than close.
        if (s.record != nullptr)
            std::fflush(s.record);
        if (s.log != nullptr)
            std::fflush(s.log);
        s.phase = Phase::ended;
    }

} // namespace saga::netplay
