// Lien fiable et ordonné vers un pair, au-dessus d'un Transport non fiable.
//
// Paquet : magic u32 | protocole u16 | ack u32 | n u8 | n × (msg_id u32 | message)
//  - msg_id > 0 : message fiable, numéroté 1, 2, 3… ; renvoyé tant qu'il n'est pas acquitté,
//    délivré dans l'ordre, une seule fois.
//  - msg_id = 0 : message non fiable (ping, positions), délivré dès réception.
//  - ack : plus grand msg_id reçu en continu (acquittement cumulatif).
// Un message fiable plus gros qu'un fragment (état complet du combat…) est découpé en fragments fiables
// (type réservé kFragmentType : total u32 | morceau), réassemblés à la livraison, dans l'ordre.
// Tout paquet invalide est signalé via on_invalid et ignoré.
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "../protocol/messages.hpp"
#include "transport.hpp"

namespace e33::net {

inline constexpr uint16_t kFragmentType = 0xFFFE;
inline constexpr size_t kFragmentPayload = 1000;   // octets de message par fragment (paquet < MTU Internet)
inline constexpr size_t kMaxMessage = 4 + 65535;   // en-tête + charge utile maximale

class Link {
public:
    Link(Transport& t, std::string peer) : t_(t), peer_(std::move(peer)) {}

    const std::string& peer() const { return peer_; }

    void send(const proto::Message& m, bool reliable = true);
    void on_datagram(const uint8_t* data, size_t len, double now);
    // Renvois, keepalive. À appeler souvent (chaque tick).
    void update(double now);
    bool pop(proto::Message& out);

    double last_heard() const { return last_heard_; }
    bool timed_out(double now, double timeout_s = 10.0) const { return last_heard_ >= 0 && now - last_heard_ > timeout_s; }
    size_t unacked() const { return outbox_.size(); }

    std::function<void(const std::string&)> on_invalid;
    std::function<void(const proto::Message&, bool reliable)> on_sent;      // pour journal / enregistrement
    std::function<void(const proto::Message&)> on_delivered;

private:
    struct Pending { uint32_t id; std::vector<uint8_t> bytes; };
    void flush(double now, bool force);
    void deliver(const std::vector<uint8_t>& raw);   // message fiable brut, dans l'ordre (fragments compris)
    void invalid(const std::string& why) { if (on_invalid) on_invalid(why); }

    Transport& t_;
    std::string peer_;
    uint32_t next_id_ = 1;
    uint32_t recv_contiguous_ = 0;                       // dernier msg_id délivré
    std::map<uint32_t, std::vector<uint8_t>> out_of_order_;
    std::deque<Pending> outbox_;                         // fiables non acquittés
    std::deque<std::vector<uint8_t>> unreliable_;        // non fiables à envoyer
    std::deque<proto::Message> inbox_;
    std::vector<uint8_t> frag_buf_;                      // réassemblage en cours
    uint32_t frag_total_ = 0;
    double last_sent_ = -1;
    double last_heard_ = -1;
    bool dirty_ = false;
};

} // namespace e33::net
