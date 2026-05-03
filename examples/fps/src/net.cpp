#include "net.hpp"

#include <array>
#include <stdexcept>

namespace fps {

void close_socket(SocketHandle socket) {
    if (socket == invalid_socket_handle) {
        return;
    }
#ifdef _WIN32
    closesocket(socket);
#else
    close(socket);
#endif
}

void set_nonblocking(SocketHandle socket) {
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(socket, FIONBIO, &mode);
#else
    fcntl(socket, F_SETFL, fcntl(socket, F_GETFL, 0) | O_NONBLOCK);
#endif
}

SocketHandle make_udp_socket(std::uint16_t port) {
    SocketHandle socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket == invalid_socket_handle) {
        throw std::runtime_error("failed to create UDP socket");
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = port == 0 ? htonl(INADDR_ANY) : htonl(INADDR_ANY);
    address.sin_port = htons(port);
    if (bind(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        close_socket(socket);
        throw std::runtime_error("failed to bind UDP socket");
    }
    set_nonblocking(socket);
    return socket;
}

sockaddr_in make_address(const std::string& host, std::uint16_t port) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1) {
        throw std::runtime_error("invalid IPv4 address: " + host);
    }
    return address;
}

kage::sync::ClientId peer_id(const sockaddr_in& address) {
    return (static_cast<kage::sync::ClientId>(ntohs(address.sin_port)) << 32U) |
        static_cast<kage::sync::ClientId>(ntohl(address.sin_addr.s_addr));
}

bool receive_packet(SocketHandle socket, kage::sync::BitBuffer& packet, sockaddr_in* sender) {
    std::array<char, 2048> bytes{};
    sockaddr_in source{};
#ifdef _WIN32
    int source_size = sizeof(source);
#else
    socklen_t source_size = sizeof(source);
#endif
    const int received = recvfrom(
        socket,
        bytes.data(),
        static_cast<int>(bytes.size()),
        0,
        reinterpret_cast<sockaddr*>(&source),
        &source_size);
    if (received <= 0) {
        return false;
    }
    packet.clear();
    packet.push_bytes(bytes.data(), static_cast<std::size_t>(received));
    if (sender != nullptr) {
        *sender = source;
    }
    return true;
}

void send_packet(SocketHandle socket, const sockaddr_in& target, const kage::sync::BitBuffer& packet) {
    const auto* data = reinterpret_cast<const char*>(packet.data());
    sendto(socket, data, static_cast<int>(packet.byte_size()), 0, reinterpret_cast<const sockaddr*>(&target), sizeof(target));
}

}  // namespace fps
