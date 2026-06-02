#pragma once

#include "ashiato/sync/sync.hpp"

#include <cstdint>
#include <string>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#define NOGDI
#define NOUSER
#define NOMMIDS
#include <winsock2.h>
#include <ws2tcpip.h>
using SocketHandle = SOCKET;
inline constexpr SocketHandle invalid_socket_handle = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using SocketHandle = int;
inline constexpr SocketHandle invalid_socket_handle = -1;
#endif

namespace fps {

void close_socket(SocketHandle socket);
void set_nonblocking(SocketHandle socket);
SocketHandle make_udp_socket(std::uint16_t port);
sockaddr_in make_address(const std::string& host, std::uint16_t port);
ashiato::sync::PeerId peer_id(const sockaddr_in& address);
bool receive_packet(SocketHandle socket, ashiato::BitBuffer& packet, sockaddr_in* sender = nullptr);
void send_packet(SocketHandle socket, const sockaddr_in& target, const ashiato::BitBuffer& packet);

}  // namespace fps
