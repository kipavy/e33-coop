// Transport abstrait : datagrammes non fiables entre pairs identifiés par une chaîne opaque
// ("ip:port" pour UDP, SteamID pour Steam Networking Sockets en phase 6).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace e33::net {

struct Datagram {
    std::string peer;
    std::vector<uint8_t> data;
};

class Transport {
public:
    virtual ~Transport() = default;
    virtual bool send(const std::string& peer, const uint8_t* data, size_t len) = 0;
    // Non bloquant : renvoie false s'il n'y a rien à lire.
    virtual bool poll(Datagram& out) = 0;
    virtual std::string local_address() const = 0;
};

// UDP direct (Winsock). bind_port = 0 -> port éphémère.
std::unique_ptr<Transport> make_udp(uint16_t bind_port, std::string& err);

} // namespace e33::net
