#include "link.hpp"

namespace e33::net {

namespace {
constexpr size_t kMaxPacket = 1200;
constexpr double kResendInterval = 0.1;
constexpr double kKeepAlive = 0.25;
constexpr size_t kMaxOutOfOrder = 256;
} // namespace

void Link::send(const proto::Message& m, bool reliable)
{
    wire::Writer w;
    proto::encode(m, w);
    if (reliable) outbox_.push_back(Pending{next_id_++, std::move(w.data())});
    else unreliable_.push_back(std::move(w.data()));
    dirty_ = true;
    if (on_sent) on_sent(m, reliable);
}

void Link::on_datagram(const uint8_t* data, size_t len, double now)
{
    wire::Reader r(data, len);
    const uint32_t magic = r.u32();
    const uint16_t proto_v = r.u16();
    const uint32_t ack = r.u32();
    const uint8_t n = r.u8();
    if (!r.ok() || magic != proto::kMagic) { if (on_invalid) on_invalid("bad packet header"); return; }
    if (proto_v != proto::kProtocolVersion) { if (on_invalid) on_invalid("protocol version " + std::to_string(proto_v)); return; }
    last_heard_ = now;

    // Acquittement cumulatif
    while (!outbox_.empty() && outbox_.front().id <= ack) outbox_.pop_front();

    for (uint8_t i = 0; i < n; ++i) {
        const uint32_t id = r.u32();
        std::string err;
        auto m = proto::decode(r, err);
        if (!r.ok()) { if (on_invalid) on_invalid("truncated packet: " + err); return; }
        if (!m) { if (on_invalid) on_invalid("invalid message: " + err); continue; }   // message ignoré, la suite reste lisible
        if (id == 0) { inbox_.push_back(std::move(*m)); if (on_delivered) on_delivered(inbox_.back()); continue; }
        if (id <= recv_contiguous_) continue;                                          // doublon
        if (id == recv_contiguous_ + 1) {
            inbox_.push_back(std::move(*m));
            if (on_delivered) on_delivered(inbox_.back());
            ++recv_contiguous_;
            for (auto it = out_of_order_.find(recv_contiguous_ + 1); it != out_of_order_.end(); it = out_of_order_.find(recv_contiguous_ + 1)) {
                inbox_.push_back(std::move(it->second));
                if (on_delivered) on_delivered(inbox_.back());
                out_of_order_.erase(it);
                ++recv_contiguous_;
            }
        } else if (out_of_order_.size() < kMaxOutOfOrder) {
            out_of_order_.emplace(id, std::move(*m));
        }
        dirty_ = true;   // il faut acquitter
    }
}

bool Link::pop(proto::Message& out)
{
    if (inbox_.empty()) return false;
    out = std::move(inbox_.front());
    inbox_.pop_front();
    return true;
}

void Link::update(double now)
{
    const bool resend_due = !outbox_.empty() && (last_sent_ < 0 || now - last_sent_ >= kResendInterval);
    const bool keepalive_due = last_sent_ < 0 || now - last_sent_ >= kKeepAlive;
    if (dirty_ || resend_due || keepalive_due) flush(now, true);
}

void Link::flush(double now, bool)
{
    // Un ou plusieurs paquets : non fiables d'abord, puis les fiables non acquittés (dans l'ordre).
    size_t rel_index = 0;
    bool sent_any = false;
    while (!sent_any || !unreliable_.empty() || rel_index < outbox_.size()) {
        wire::Writer w;
        w.u32(proto::kMagic);
        w.u16(proto::kProtocolVersion);
        w.u32(recv_contiguous_);
        const size_t count_pos = w.size();
        w.u8(0);
        uint8_t count = 0;
        while (!unreliable_.empty() && count < 255 && w.size() + 4 + unreliable_.front().size() <= kMaxPacket) {
            w.u32(0);
            w.bytes(unreliable_.front().data(), unreliable_.front().size());
            unreliable_.pop_front();
            ++count;
        }
        while (rel_index < outbox_.size() && count < 255 && w.size() + 4 + outbox_[rel_index].bytes.size() <= kMaxPacket) {
            w.u32(outbox_[rel_index].id);
            w.bytes(outbox_[rel_index].bytes.data(), outbox_[rel_index].bytes.size());
            ++rel_index;
            ++count;
        }
        // Message plus gros qu'un paquet nominal : on l'envoie seul plutôt que de boucler.
        if (count == 0 && !unreliable_.empty()) {
            w.u32(0); w.bytes(unreliable_.front().data(), unreliable_.front().size()); unreliable_.pop_front(); ++count;
        } else if (count == 0 && rel_index < outbox_.size()) {
            w.u32(outbox_[rel_index].id); w.bytes(outbox_[rel_index].bytes.data(), outbox_[rel_index].bytes.size()); ++rel_index; ++count;
        }
        w.data()[count_pos] = count;
        t_.send(peer_, w.data().data(), w.size());
        sent_any = true;
        if (count == 0) break;   // paquet vide = keepalive/ack seul
        if (rel_index >= outbox_.size() && unreliable_.empty()) break;
        if (rel_index >= 32) break;   // pas plus de 32 messages fiables renvoyés par vague
    }
    last_sent_ = now;
    dirty_ = false;
}

} // namespace e33::net
