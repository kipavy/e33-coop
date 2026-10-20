// Lien fiable et ordonné vers un pair, au-dessus d'un Transport non fiable.
//
// Paquet : magic u32 | protocole u16 | époque u32 | ack u32 | n u8 | n × (msg_id u32 | message)
//  - msg_id > 0 : message fiable, numéroté 1, 2, 3… ; renvoyé tant qu'il n'est pas acquitté,
//    délivré dans l'ordre, une seule fois.
//  - msg_id = 0 : message non fiable (ping, positions), délivré dès réception.
//  - ack : plus grand msg_id reçu en continu (acquittement cumulatif).
//  - époque : tirée au hasard à la création du lien chez l'émetteur. Si elle change, le pair a redémarré
//    (nouveau processus, reconnexion) : l'ancien échange n'a plus de sens (remote_reset).
//  - ack = kBusyAck et n = 0 : « occupé » (battement envoyé par un autre thread pendant que le thread de jeu est
//    bloqué, p. ex. un chargement de niveau de 30 à 60 s) : le pair est vivant, rien à acquitter.
// Les 6 premiers octets (magic, protocole) ne changeront jamais : un pair d'une autre version de protocole est
// reconnu (on_protocol_mismatch) et peut être refusé avec un message clair.
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
inline constexpr uint32_t kBusyAck = 0xFFFFFFFFu;
inline constexpr size_t kHeaderSize = 4 + 2 + 4 + 4 + 1;

class Link {
public:
    Link(Transport& t, std::string peer);

    const std::string& peer() const { return peer_; }
    uint32_t epoch() const { return epoch_; }
    uint32_t remote_epoch() const { return remote_epoch_; }

    void send(const proto::Message& m, bool reliable = true);
    void on_datagram(const uint8_t* data, size_t len, double now);
    // Renvois, keepalive. À appeler souvent (chaque tick).
    void update(double now);
    bool pop(proto::Message& out);

    double last_heard() const { return last_heard_; }
    // Silencieux depuis timeout_s : ni paquet normal, ni battement « occupé » récent. Un pair seulement « occupé »
    // est déclaré perdu après busy_max_s (jeu bloqué pour de bon).
    bool timed_out(double now, double timeout_s = 15.0, double busy_max_s = 300.0) const;
    bool busy(double now) const { return last_busy_ >= 0 && last_busy_ > last_heard_ && now - last_busy_ < 5.0; }
    size_t unacked() const { return outbox_.size(); }
    int resets() const { return resets_; }

    // Époque de l'émetteur d'un datagramme (0 si ce n'est pas un paquet de notre format / version).
    static uint32_t packet_epoch(const uint8_t* data, size_t len);
    // Version de protocole annoncée par un datagramme (0 si ce n'est pas un paquet E33C).
    static uint16_t packet_protocol(const uint8_t* data, size_t len);
    // Battement « occupé » (thread de jeu bloqué) : envoyable depuis un autre thread (aucun état du lien lu).
    static std::vector<uint8_t> busy_packet(uint32_t epoch);
    // Paquet vide de notre version (réponse à un pair d'une autre version de protocole : il saura laquelle).
    static std::vector<uint8_t> version_packet(uint32_t epoch);

    std::function<void(const std::string&)> on_invalid;
    std::function<void(const proto::Message&, bool reliable)> on_sent;      // pour journal / enregistrement
    std::function<void(const proto::Message&)> on_delivered;
    std::function<void(uint16_t remote_protocol)> on_protocol_mismatch;
    std::function<void()> on_remote_reset;                                    // le pair a redémarré (nouvelle époque)

private:
    struct Pending { uint32_t id; std::vector<uint8_t> bytes; };
    void flush(double now, bool force);
    void deliver(const std::vector<uint8_t>& raw);   // message fiable brut, dans l'ordre (fragments compris)
    void invalid(const std::string& why) { if (on_invalid) on_invalid(why); }
    void reset_exchange();

    Transport& t_;
    std::string peer_;
    uint32_t epoch_ = 0;
    uint32_t remote_epoch_ = 0;
    int resets_ = 0;
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
    double last_busy_ = -1;
    bool dirty_ = false;
    bool mismatch_reported_ = false;
};

} // namespace e33::net
