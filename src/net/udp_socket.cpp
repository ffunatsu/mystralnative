#include "mystral/net/udp_socket.h"

#include <cerrno>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using socket_t = SOCKET;
using address_len_t = int;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
using address_len_t = socklen_t;
constexpr socket_t kInvalidSocket = -1;
#endif

namespace mystral {
namespace net {

namespace {

struct UDPSocketState {
    uint32_t id = 0;
    socket_t fd = kInvalidSocket;
    int family = AF_UNSPEC;
};

std::map<uint32_t, std::unique_ptr<UDPSocketState>> g_sockets;
uint32_t g_nextId = 1;
js::Engine* g_engine = nullptr;
js::JSValueHandle g_dispatch{};
bool g_hasDispatch = false;

void closeSocket(socket_t fd) {
#ifdef _WIN32
    closesocket(fd);
#else
    close(fd);
#endif
}

bool setNonBlocking(socket_t fd) {
#ifdef _WIN32
    u_long mode = 1;
    return ioctlsocket(fd, FIONBIO, &mode) == 0;
#else
    int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

bool wouldBlock() {
#ifdef _WIN32
    int error = WSAGetLastError();
    return error == WSAEWOULDBLOCK;
#else
    return errno == EWOULDBLOCK || errno == EAGAIN;
#endif
}

std::string socketError() {
#ifdef _WIN32
    return "socket error " + std::to_string(WSAGetLastError());
#else
    return std::strerror(errno);
#endif
}

socket_t createSocket(int family) {
    socket_t fd = socket(family, SOCK_DGRAM, IPPROTO_UDP);
    if (fd == kInvalidSocket) return kInvalidSocket;
    if (!setNonBlocking(fd)) {
        closeSocket(fd);
        return kInvalidSocket;
    }
    int enabled = 1;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST,
               reinterpret_cast<const char*>(&enabled), sizeof(enabled));
    return fd;
}

void dispatchError(uint32_t id, const std::string& message) {
    if (!g_engine || !g_hasDispatch) return;
    std::vector<js::JSValueHandle> args = {
        g_engine->newString("error"),
        g_engine->newNumber(id),
        g_engine->newString(message.c_str()),
        g_engine->newUndefined(),
        g_engine->newUndefined(),
    };
    g_engine->call(g_dispatch, g_engine->newUndefined(), args);
}

void dispatchMessage(uint32_t id, const uint8_t* data, size_t size,
                     const char* address, uint16_t port) {
    if (!g_engine || !g_hasDispatch) return;
    std::vector<js::JSValueHandle> args = {
        g_engine->newString("message"),
        g_engine->newNumber(id),
        g_engine->createUint8Array(data, size),
        g_engine->newString(address),
        g_engine->newNumber(port),
    };
    g_engine->call(g_dispatch, g_engine->newUndefined(), args);
}

int bindSocket(UDPSocketState* state, const std::string& address, uint16_t port) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_PASSIVE;

    addrinfo* results = nullptr;
    std::string portString = std::to_string(port);
    const char* host = address.empty() ? nullptr : address.c_str();
    if (getaddrinfo(host, portString.c_str(), &hints, &results) != 0) return -1;

    int result = -1;
    for (addrinfo* candidate = results; candidate; candidate = candidate->ai_next) {
        socket_t fd = createSocket(candidate->ai_family);
        if (fd == kInvalidSocket) continue;

        int reuse = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&reuse), sizeof(reuse));
        if (bind(fd, candidate->ai_addr, static_cast<address_len_t>(candidate->ai_addrlen)) == 0) {
            if (state->fd != kInvalidSocket) closeSocket(state->fd);
            state->fd = fd;
            state->family = candidate->ai_family;
            result = 0;
            break;
        }
        closeSocket(fd);
    }
    freeaddrinfo(results);
    return result;
}

int sendDatagram(UDPSocketState* state, const uint8_t* data, size_t size,
                 const std::string& address, uint16_t port) {
    addrinfo hints{};
    hints.ai_family = state->fd == kInvalidSocket ? AF_UNSPEC : state->family;
    hints.ai_socktype = SOCK_DGRAM;

    addrinfo* results = nullptr;
    std::string portString = std::to_string(port);
    if (getaddrinfo(address.c_str(), portString.c_str(), &hints, &results) != 0) return -1;

    int sent = -1;
    for (addrinfo* candidate = results; candidate; candidate = candidate->ai_next) {
        if (state->fd == kInvalidSocket) {
            state->fd = createSocket(candidate->ai_family);
            if (state->fd == kInvalidSocket) continue;
            state->family = candidate->ai_family;
        }
        sent = static_cast<int>(sendto(
            state->fd, reinterpret_cast<const char*>(data), static_cast<int>(size), 0,
            candidate->ai_addr, static_cast<address_len_t>(candidate->ai_addrlen)));
        if (sent >= 0) break;
    }
    freeaddrinfo(results);
    return sent;
}

}  // namespace

bool initUDPBindings(js::Engine* engine) {
    if (!engine) return false;
    g_engine = engine;

    engine->setGlobalProperty("__udpCreate",
        engine->newFunction("__udpCreate", [](void*, const std::vector<js::JSValueHandle>&) {
            auto socket = std::make_unique<UDPSocketState>();
            socket->id = g_nextId++;
            uint32_t id = socket->id;
            g_sockets[id] = std::move(socket);
            return g_engine->newNumber(id);
        }));

    engine->setGlobalProperty("__udpBind",
        engine->newFunction("__udpBind", [](void*, const std::vector<js::JSValueHandle>& args) {
            if (args.size() < 3) return g_engine->newNumber(-1);
            uint32_t id = static_cast<uint32_t>(g_engine->toNumber(args[0]));
            auto it = g_sockets.find(id);
            if (it == g_sockets.end()) return g_engine->newNumber(-1);
            std::string address = g_engine->toString(args[1]);
            uint16_t port = static_cast<uint16_t>(g_engine->toNumber(args[2]));
            return g_engine->newNumber(bindSocket(it->second.get(), address, port));
        }));

    engine->setGlobalProperty("__udpSend",
        engine->newFunction("__udpSend", [](void*, const std::vector<js::JSValueHandle>& args) {
            if (args.size() < 4) return g_engine->newNumber(-1);
            uint32_t id = static_cast<uint32_t>(g_engine->toNumber(args[0]));
            auto it = g_sockets.find(id);
            if (it == g_sockets.end()) return g_engine->newNumber(-1);
            size_t size = 0;
            auto* data = static_cast<const uint8_t*>(g_engine->getArrayBufferData(args[1], &size));
            if (!data && size) return g_engine->newNumber(-1);
            std::string address = g_engine->toString(args[2]);
            uint16_t port = static_cast<uint16_t>(g_engine->toNumber(args[3]));
            return g_engine->newNumber(sendDatagram(it->second.get(), data, size, address, port));
        }));

    engine->setGlobalProperty("__udpClose",
        engine->newFunction("__udpClose", [](void*, const std::vector<js::JSValueHandle>& args) {
            if (args.empty()) return g_engine->newUndefined();
            uint32_t id = static_cast<uint32_t>(g_engine->toNumber(args[0]));
            auto it = g_sockets.find(id);
            if (it != g_sockets.end()) {
                if (it->second->fd != kInvalidSocket) closeSocket(it->second->fd);
                g_sockets.erase(it);
            }
            return g_engine->newUndefined();
        }));

    static const char* kPolyfill = R"JS(
(function () {
  if (typeof globalThis.UDPSocket !== 'undefined') return;

  const sockets = new Map();

  function toBytes(data) {
    if (data instanceof Uint8Array) return data;
    if (data instanceof ArrayBuffer) return new Uint8Array(data);
    if (ArrayBuffer.isView(data)) {
      return new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
    }
    throw new TypeError('UDP data must be an ArrayBuffer or typed array');
  }

  globalThis.__udpDispatch = function (type, id, data, address, port) {
    const socket = sockets.get(id);
    if (!socket) return;
    if (type === 'message') {
      if (socket.onmessage) socket.onmessage({ type, data, address, port });
    } else if (type === 'error') {
      if (socket.onerror) socket.onerror({ type, message: data });
    }
  };

  class UDPSocket {
    constructor() {
      this.onmessage = null;
      this.onerror = null;
      this._id = __udpCreate();
      this._closed = false;
      sockets.set(this._id, this);
    }

    async bind(options) {
      if (this._closed) throw new Error('UDPSocket is closed');
      const address = options && options.address != null ? String(options.address) : '0.0.0.0';
      const port = Number(options && options.port);
      if (!Number.isInteger(port) || port < 0 || port > 65535) throw new RangeError('Invalid UDP port');
      if (__udpBind(this._id, address, port) < 0) throw new Error('Failed to bind UDP socket');
    }

    async send(data, options) {
      if (this._closed) throw new Error('UDPSocket is closed');
      const address = options && options.address != null ? String(options.address) : '';
      const port = Number(options && options.port);
      if (!address) throw new TypeError('UDP destination address is required');
      if (!Number.isInteger(port) || port < 1 || port > 65535) throw new RangeError('Invalid UDP port');
      const sent = __udpSend(this._id, toBytes(data), address, port);
      if (sent < 0) throw new Error('Failed to send UDP datagram');
      return sent;
    }

    close() {
      if (this._closed) return;
      this._closed = true;
      sockets.delete(this._id);
      __udpClose(this._id);
    }
  }

  globalThis.UDPSocket = UDPSocket;
})();
)JS";

    if (!engine->eval(kPolyfill, "<udp-polyfill>")) {
        std::cerr << "[UDP] polyfill eval failed: " << engine->getException() << std::endl;
        return false;
    }
    g_dispatch = engine->getGlobalProperty("__udpDispatch");
    engine->protect(g_dispatch);
    g_hasDispatch = true;
    return true;
}

void initUDPNetworking() {
#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif
}

void processUDPEvents() {
    uint8_t buffer[65536];
    for (auto& [id, state] : g_sockets) {
        if (state->fd == kInvalidSocket) continue;
        while (true) {
            sockaddr_storage from{};
            address_len_t fromLength = sizeof(from);
            int received = static_cast<int>(recvfrom(
                state->fd, reinterpret_cast<char*>(buffer), sizeof(buffer), 0,
                reinterpret_cast<sockaddr*>(&from), &fromLength));
            if (received < 0) {
                if (!wouldBlock()) dispatchError(id, socketError());
                break;
            }

            char address[NI_MAXHOST]{};
            char service[NI_MAXSERV]{};
            if (getnameinfo(reinterpret_cast<sockaddr*>(&from), fromLength,
                            address, sizeof(address), service, sizeof(service),
                            NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
                dispatchError(id, "Failed to resolve UDP sender address");
                continue;
            }
            dispatchMessage(id, buffer, static_cast<size_t>(received), address,
                            static_cast<uint16_t>(std::stoi(service)));
        }
    }
}

bool hasActiveUDPSockets() {
    return !g_sockets.empty();
}

void shutdownUDPNetworking() {
    for (auto& [id, state] : g_sockets) {
        if (state->fd != kInvalidSocket) closeSocket(state->fd);
    }
    g_sockets.clear();
    if (g_engine && g_hasDispatch) g_engine->unprotect(g_dispatch);
    g_dispatch = {};
    g_hasDispatch = false;
    g_engine = nullptr;
#ifdef _WIN32
    WSACleanup();
#endif
}

}  // namespace net
}  // namespace mystral