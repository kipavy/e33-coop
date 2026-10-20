#include "transport.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>

#include <mutex>

namespace e33::net {

namespace {

std::once_flag g_wsa_once;

std::string addr_to_string(const sockaddr_in& a)
{
    char ip[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &a.sin_addr, ip, sizeof ip);
    return std::string(ip) + ":" + std::to_string(ntohs(a.sin_port));
}

bool string_to_addr(const std::string& s, sockaddr_in& a)
{
    const auto colon = s.rfind(':');
    if (colon == std::string::npos) return false;
    const std::string host = s.substr(0, colon);
    const int port = std::atoi(s.c_str() + colon + 1);
    if (port <= 0 || port > 65535) return false;
    a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<u_short>(port));
    if (inet_pton(AF_INET, host.c_str(), &a.sin_addr) == 1) return true;
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) return false;
    a.sin_addr = reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr;
    freeaddrinfo(res);
    return true;
}

class UdpTransport final : public Transport {
public:
    ~UdpTransport() override { if (s_ != INVALID_SOCKET) closesocket(s_); }

    bool open(uint16_t port, std::string& err)
    {
        std::call_once(g_wsa_once, [] { WSADATA d; WSAStartup(MAKEWORD(2, 2), &d); });
        s_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s_ == INVALID_SOCKET) { err = "socket() failed: " + std::to_string(WSAGetLastError()); return false; }
        u_long nb = 1;
        ioctlsocket(s_, FIONBIO, &nb);
        // Évite que Windows ferme la socket sur un ICMP "port unreachable" (WSAECONNRESET).
        BOOL no = FALSE;
        DWORD ret = 0;
        WSAIoctl(s_, _WSAIOW(IOC_VENDOR, 12) /* SIO_UDP_CONNRESET */, &no, sizeof no, nullptr, 0, &ret, nullptr, nullptr);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(s_, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) { err = "bind() failed on port " + std::to_string(port) + ": " + std::to_string(WSAGetLastError()); return false; }
        int len = sizeof a;
        getsockname(s_, reinterpret_cast<sockaddr*>(&a), &len);
        local_ = "0.0.0.0:" + std::to_string(ntohs(a.sin_port));
        return true;
    }

    bool send(const std::string& peer, const uint8_t* data, size_t len) override
    {
        sockaddr_in a{};
        if (!string_to_addr(peer, a)) return false;
        return sendto(s_, reinterpret_cast<const char*>(data), static_cast<int>(len), 0, reinterpret_cast<sockaddr*>(&a), sizeof a) == static_cast<int>(len);
    }

    bool poll(Datagram& out) override
    {
        // Les messages fiables sont fragmentés (net::Link) : un datagramme dépasse rarement 1,3 Ko, mais le tampon
        // couvre tout datagramme UDP possible pour ne jamais tronquer.
        static thread_local std::vector<char> buf(65536);
        for (int attempts = 0; attempts < 64; ++attempts) {
            sockaddr_in from{};
            int fl = sizeof from;
            const int n = recvfrom(s_, buf.data(), static_cast<int>(buf.size()), 0, reinterpret_cast<sockaddr*>(&from), &fl);
            if (n > 0) {
                out.peer = addr_to_string(from);
                out.data.assign(reinterpret_cast<uint8_t*>(buf.data()), reinterpret_cast<uint8_t*>(buf.data()) + n);
                return true;
            }
            // WSAECONNRESET (ICMP « port injoignable » d'un envoi précédent) ou WSAEMSGSIZE : datagramme ignoré, on continue.
            const int e = WSAGetLastError();
            if (n == 0 || (e != WSAECONNRESET && e != WSAEMSGSIZE)) return false;
        }
        return false;
    }

    std::string local_address() const override { return local_; }

private:
    SOCKET s_ = INVALID_SOCKET;
    std::string local_;
};

} // namespace

std::unique_ptr<Transport> make_udp(uint16_t bind_port, std::string& err)
{
    auto t = std::make_unique<UdpTransport>();
    if (!t->open(bind_port, err)) return nullptr;
    return t;
}

} // namespace e33::net
