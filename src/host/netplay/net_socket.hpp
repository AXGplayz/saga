#pragma once

// Minimal cross-platform TCP/UDP sockets for netplay (Winsock on Windows,
// BSD sockets elsewhere). IPv4 only: home port forwarding and VPN overlays
// such as Tailscale/ZeroTier both hand out IPv4 addresses.

#include <cstddef>
#include <cstdint>
#include <string>

namespace saga::netplay {

    struct NetAddress final {
        std::uint32_t ip = 0;   // network byte order
        std::uint16_t port = 0; // host byte order

        [[nodiscard]] bool same_host(const NetAddress &other) const noexcept { return ip == other.ip; }
        [[nodiscard]] bool operator==(const NetAddress &other) const noexcept {
            return ip == other.ip && port == other.port;
        }
        [[nodiscard]] std::string to_string() const;
    };

    // Initialises the socket library once per process (no-op off Windows).
    bool net_startup();

    // Resolves "1.2.3.4", "host.name" or "host.name:port". `default_port` is used
    // when no port is given. Returns false with a message in `error`.
    bool net_resolve(const std::string &text, std::uint16_t default_port, NetAddress &out, std::string &error);

    // Milliseconds from a monotonic clock.
    std::uint64_t net_now_ms();

    class UdpSocket final {
      public:
        UdpSocket() = default;
        ~UdpSocket();
        UdpSocket(const UdpSocket &) = delete;
        UdpSocket &operator=(const UdpSocket &) = delete;

        // Binds to 0.0.0.0:port (port 0 picks any free port).
        bool open(std::uint16_t port, std::string &error);
        void close();
        [[nodiscard]] bool is_open() const noexcept { return handle_ != invalid_handle; }
        [[nodiscard]] std::uint16_t local_port() const;

        bool send_to(const NetAddress &to, const void *data, std::size_t size);
        // Waits up to timeout_ms for one datagram. Returns its size, 0 on timeout, -1 on error.
        int receive(void *buffer, std::size_t capacity, NetAddress &from, int timeout_ms);

      private:
        static constexpr std::intptr_t invalid_handle = -1;
        std::intptr_t handle_ = invalid_handle;
    };

    class TcpStream final {
      public:
        TcpStream() = default;
        explicit TcpStream(std::intptr_t handle, NetAddress peer) : handle_(handle), peer_(peer) {}
        ~TcpStream();
        TcpStream(const TcpStream &) = delete;
        TcpStream &operator=(const TcpStream &) = delete;
        TcpStream(TcpStream &&other) noexcept;
        TcpStream &operator=(TcpStream &&other) noexcept;

        bool connect(const NetAddress &to, int timeout_ms, std::string &error);
        void close();
        [[nodiscard]] bool is_open() const noexcept { return handle_ != invalid_handle; }
        [[nodiscard]] const NetAddress &peer() const noexcept { return peer_; }

        bool send_all(const void *data, std::size_t size);
        // Reads exactly `size` bytes or fails (timeout covers the whole read).
        bool receive_all(void *data, std::size_t size, int timeout_ms);

      private:
        static constexpr std::intptr_t invalid_handle = -1;
        std::intptr_t handle_ = invalid_handle;
        NetAddress peer_{};
    };

    class TcpListener final {
      public:
        TcpListener() = default;
        ~TcpListener();
        TcpListener(const TcpListener &) = delete;
        TcpListener &operator=(const TcpListener &) = delete;

        bool open(std::uint16_t port, std::string &error);
        void close();
        // Waits up to timeout_ms for one connection. Returns an unopened stream on timeout.
        TcpStream accept(int timeout_ms);

      private:
        static constexpr std::intptr_t invalid_handle = -1;
        std::intptr_t handle_ = invalid_handle;
    };

} // namespace saga::netplay
