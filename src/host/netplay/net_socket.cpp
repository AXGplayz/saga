#include "host/netplay/net_socket.hpp"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
using socklen_type = int;
#define SAGA_NET_INVALID INVALID_SOCKET
#define SAGA_NET_CLOSE closesocket
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
using socket_t = int;
using socklen_type = socklen_t;
#define SAGA_NET_INVALID (-1)
#define SAGA_NET_CLOSE ::close
#endif

namespace saga::netplay {
    namespace {

        socket_t to_socket(std::intptr_t handle) { return static_cast<socket_t>(handle); }
        std::intptr_t to_handle(socket_t socket) { return static_cast<std::intptr_t>(socket); }

        bool valid(socket_t socket) { return socket != SAGA_NET_INVALID; }

        std::string last_error_text() {
#ifdef _WIN32
            return "socket error " + std::to_string(WSAGetLastError());
#else
            return std::strerror(errno);
#endif
        }

        void set_nonblocking(socket_t socket, bool enabled) {
#ifdef _WIN32
            u_long mode = enabled ? 1 : 0;
            ioctlsocket(socket, FIONBIO, &mode);
#else
            const int flags = fcntl(socket, F_GETFL, 0);
            fcntl(socket, F_SETFL, enabled ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
#endif
        }

        // Returns >0 when readable/writable, 0 on timeout, <0 on error.
        int wait_socket(socket_t socket, bool for_write, int timeout_ms) {
            fd_set set;
            FD_ZERO(&set);
            FD_SET(socket, &set);
            timeval tv{};
            tv.tv_sec = timeout_ms / 1000;
            tv.tv_usec = (timeout_ms % 1000) * 1000;
            const int nfds = static_cast<int>(socket) + 1;
            return for_write ? select(nfds, nullptr, &set, nullptr, &tv) : select(nfds, &set, nullptr, nullptr, &tv);
        }

        sockaddr_in to_sockaddr(const NetAddress &address) {
            sockaddr_in result{};
            result.sin_family = AF_INET;
            result.sin_addr.s_addr = address.ip;
            result.sin_port = htons(address.port);
            return result;
        }

        NetAddress from_sockaddr(const sockaddr_in &address) {
            NetAddress result;
            result.ip = address.sin_addr.s_addr;
            result.port = ntohs(address.sin_port);
            return result;
        }

        socket_t open_bound(int type, std::uint16_t port, std::string &error) {
            socket_t socket = ::socket(AF_INET, type, type == SOCK_STREAM ? IPPROTO_TCP : IPPROTO_UDP);
            if (!valid(socket)) {
                error = "could not create socket: " + last_error_text();
                return SAGA_NET_INVALID;
            }
            const int reuse = 1;
            setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&reuse), sizeof(reuse));
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_ANY);
            address.sin_port = htons(port);
            if (bind(socket, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0) {
                error = "could not use port " + std::to_string(port) + ": " + last_error_text() +
                        " (is another copy already running?)";
                SAGA_NET_CLOSE(socket);
                return SAGA_NET_INVALID;
            }
            return socket;
        }

    } // namespace

    std::string NetAddress::to_string() const {
        in_addr address{};
        address.s_addr = ip;
        char text[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &address, text, sizeof(text));
        return std::string(text) + ":" + std::to_string(port);
    }

    bool net_startup() {
#ifdef _WIN32
        static std::once_flag once;
        static bool ok = false;
        std::call_once(once, [] {
            WSADATA data;
            ok = WSAStartup(MAKEWORD(2, 2), &data) == 0;
        });
        return ok;
#else
        return true;
#endif
    }

    bool net_resolve(const std::string &text, std::uint16_t default_port, NetAddress &out, std::string &error) {
        if (!net_startup()) {
            error = "networking is unavailable";
            return false;
        }
        std::string host = text;
        std::uint16_t port = default_port;
        const std::size_t colon = text.rfind(':');
        if (colon != std::string::npos && text.find(':') == colon) {
            host = text.substr(0, colon);
            const std::string port_text = text.substr(colon + 1);
            char *end = nullptr;
            const unsigned long value = std::strtoul(port_text.c_str(), &end, 10);
            if (port_text.empty() || *end != '\0' || value == 0 || value > 65535) {
                error = "invalid port in '" + text + "'";
                return false;
            }
            port = static_cast<std::uint16_t>(value);
        }
        if (host.empty()) {
            error = "no address given";
            return false;
        }

        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        addrinfo *result = nullptr;
        if (getaddrinfo(host.c_str(), nullptr, &hints, &result) != 0 || result == nullptr) {
            error = "could not find '" + host + "'";
            return false;
        }
        out.ip = reinterpret_cast<const sockaddr_in *>(result->ai_addr)->sin_addr.s_addr;
        out.port = port;
        freeaddrinfo(result);
        return true;
    }

    std::uint64_t net_now_ms() {
        using namespace std::chrono;
        return static_cast<std::uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
    }

    // ---------------------------------------------------------------- UDP

    UdpSocket::~UdpSocket() { close(); }

    bool UdpSocket::open(std::uint16_t port, std::string &error) {
        close();
        if (!net_startup()) {
            error = "networking is unavailable";
            return false;
        }
        const socket_t socket = open_bound(SOCK_DGRAM, port, error);
        if (!valid(socket))
            return false;
        set_nonblocking(socket, true);
        handle_ = to_handle(socket);
        return true;
    }

    void UdpSocket::close() {
        if (handle_ != invalid_handle) {
            SAGA_NET_CLOSE(to_socket(handle_));
            handle_ = invalid_handle;
        }
    }

    std::uint16_t UdpSocket::local_port() const {
        sockaddr_in address{};
        socklen_type length = sizeof(address);
        if (getsockname(to_socket(handle_), reinterpret_cast<sockaddr *>(&address), &length) != 0)
            return 0;
        return ntohs(address.sin_port);
    }

    bool UdpSocket::send_to(const NetAddress &to, const void *data, std::size_t size) {
        const sockaddr_in address = to_sockaddr(to);
        const auto sent = sendto(to_socket(handle_), static_cast<const char *>(data), static_cast<int>(size), 0,
                                 reinterpret_cast<const sockaddr *>(&address), sizeof(address));
        return static_cast<long long>(sent) == static_cast<long long>(size);
    }

    int UdpSocket::receive(void *buffer, std::size_t capacity, NetAddress &from, int timeout_ms) {
        const socket_t socket = to_socket(handle_);
        if (timeout_ms > 0) {
            const int ready = wait_socket(socket, false, timeout_ms);
            if (ready <= 0)
                return ready == 0 ? 0 : -1;
        }
        sockaddr_in address{};
        socklen_type length = sizeof(address);
        const auto received = recvfrom(socket, static_cast<char *>(buffer), static_cast<int>(capacity), 0,
                                       reinterpret_cast<sockaddr *>(&address), &length);
        if (received < 0) {
#ifdef _WIN32
            const int code = WSAGetLastError();
            // ICMP "port unreachable" surfaces as WSAECONNRESET on Windows; it is not fatal.
            if (code == WSAEWOULDBLOCK || code == WSAECONNRESET)
                return 0;
#else
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ECONNREFUSED)
                return 0;
#endif
            return -1;
        }
        from = from_sockaddr(address);
        return static_cast<int>(received);
    }

    // ---------------------------------------------------------------- TCP

    TcpStream::~TcpStream() { close(); }

    TcpStream::TcpStream(TcpStream &&other) noexcept : handle_(other.handle_), peer_(other.peer_) {
        other.handle_ = invalid_handle;
    }

    TcpStream &TcpStream::operator=(TcpStream &&other) noexcept {
        if (this != &other) {
            close();
            handle_ = other.handle_;
            peer_ = other.peer_;
            other.handle_ = invalid_handle;
        }
        return *this;
    }

    bool TcpStream::connect(const NetAddress &to, int timeout_ms, std::string &error) {
        close();
        if (!net_startup()) {
            error = "networking is unavailable";
            return false;
        }
        const socket_t socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (!valid(socket)) {
            error = "could not create socket: " + last_error_text();
            return false;
        }
        set_nonblocking(socket, true);
        const sockaddr_in address = to_sockaddr(to);
        ::connect(socket, reinterpret_cast<const sockaddr *>(&address), sizeof(address));
        if (wait_socket(socket, true, timeout_ms) <= 0) {
            error = "no answer from " + to.to_string();
            SAGA_NET_CLOSE(socket);
            return false;
        }
        int socket_error = 0;
        socklen_type length = sizeof(socket_error);
        getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&socket_error), &length);
        if (socket_error != 0) {
            error = "could not connect to " + to.to_string() + " (is the host waiting, and is the port open?)";
            SAGA_NET_CLOSE(socket);
            return false;
        }
        set_nonblocking(socket, false);
        const int nodelay = 1;
        setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&nodelay), sizeof(nodelay));
        handle_ = to_handle(socket);
        peer_ = to;
        return true;
    }

    void TcpStream::close() {
        if (handle_ != invalid_handle) {
            SAGA_NET_CLOSE(to_socket(handle_));
            handle_ = invalid_handle;
        }
    }

    bool TcpStream::send_all(const void *data, std::size_t size) {
        const char *cursor = static_cast<const char *>(data);
        while (size > 0) {
            const auto sent = send(to_socket(handle_), cursor, static_cast<int>(size), 0);
            if (sent <= 0)
                return false;
            cursor += sent;
            size -= static_cast<std::size_t>(sent);
        }
        return true;
    }

    bool TcpStream::receive_all(void *data, std::size_t size, int timeout_ms) {
        char *cursor = static_cast<char *>(data);
        const std::uint64_t deadline = net_now_ms() + static_cast<std::uint64_t>(timeout_ms);
        while (size > 0) {
            const std::uint64_t now = net_now_ms();
            if (now >= deadline)
                return false;
            if (wait_socket(to_socket(handle_), false, static_cast<int>(deadline - now)) <= 0)
                return false;
            const auto received = recv(to_socket(handle_), cursor, static_cast<int>(size), 0);
            if (received <= 0)
                return false;
            cursor += received;
            size -= static_cast<std::size_t>(received);
        }
        return true;
    }

    TcpListener::~TcpListener() { close(); }

    bool TcpListener::open(std::uint16_t port, std::string &error) {
        close();
        if (!net_startup()) {
            error = "networking is unavailable";
            return false;
        }
        const socket_t socket = open_bound(SOCK_STREAM, port, error);
        if (!valid(socket))
            return false;
        if (listen(socket, 1) != 0) {
            error = "could not listen on port " + std::to_string(port) + ": " + last_error_text();
            SAGA_NET_CLOSE(socket);
            return false;
        }
        handle_ = to_handle(socket);
        return true;
    }

    void TcpListener::close() {
        if (handle_ != invalid_handle) {
            SAGA_NET_CLOSE(to_socket(handle_));
            handle_ = invalid_handle;
        }
    }

    TcpStream TcpListener::accept(int timeout_ms) {
        const socket_t listener = to_socket(handle_);
        if (wait_socket(listener, false, timeout_ms) <= 0)
            return TcpStream{};
        sockaddr_in address{};
        socklen_type length = sizeof(address);
        const socket_t client = ::accept(listener, reinterpret_cast<sockaddr *>(&address), &length);
        if (!valid(client))
            return TcpStream{};
        const int nodelay = 1;
        setsockopt(client, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&nodelay), sizeof(nodelay));
        return TcpStream(to_handle(client), from_sockaddr(address));
    }

} // namespace saga::netplay
