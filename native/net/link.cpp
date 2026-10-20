#include "link.hpp"

#include <algorithm>

namespace e33::net {

namespace {
constexpr size_t kMaxPacket = 1200;
constexpr double kResendInterval = 0.1;
constexpr double kKeepAlive = 0.25;
constexpr size_t kMaxOutOfOrder = 256;

void put_u16(std::vector<uint8_t>& v, uint16_t x) { v.push_back(static_cast<uint8_t>(x)); v.push_back(static_cast<uint8_t>(x >> 8)); }
void put_u32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back(static_cast<uint8_t>(x >> (8 * i))); }
uint16_t get_u16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t get_u32(const uint8_t* p) { return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24); }
} // namespace

void Link::send(const proto::Message& m, bool reliable)
{
    wire::Writer w;
    proto::encode(m, w);
    if (on_sent) on_sent(m, reliable);
    dirty_ = true;
    if (!reliable) { unreliable_.push_back(std::move(w.data())); return; }
    if (w.size() <= kFragmentPayload) { outbox_.push_back(Pending{next_id_++, std::move(w.data())}); return; }
    // Gros message : fragments fiables (type réservé), réassemblés dans l'ordre par le destinataire.
    const auto& all = w.data();
    for (size_t off = 0; off < all.size(); off += kFragmentPayload) {
        const size_t n = std::min(kFragmentPayload, all.size() - off);
        std::vector<uint8_t> f;
        f.reserve(8 + n);
        put_u16(f, kFragmentType);
        put_u16(f, static_cast<uint16_t>(4 + n));
        put_u32(f, static_cast<uint32_t>(all.size()));
        f.insert(f.end(), all.begin() + static_cast<std::ptrdiff_t>(off), all.begin() + static_cast<std::ptrdiff_t>(off + n));
        outbox_.push_back(Pending{next_id_++, std::move(f)});
    }
}

void Link::deliver(const std::vector<uint8_t>& raw)
{
    if (raw.size() >= 8 && get_u16(raw.data()) == kFragmentType) {
        const uint32_t total = get_u32(raw.data() + 4);
        if (total == 0 || total > kMaxMessage || (frag_total_ != 0 && total != frag_total_)) {
            invalid("bad fragment");
            frag_buf_.clear();
            frag_total_ = 0;
            return;
        }
        frag_total_ = total;
        frag_buf_.insert(frag_buf_.end(), raw.begin() + 8, raw.end());
        if (frag_buf_.size() < frag_total_) return;
        std::vector<uint8_t> whole;
        whole.swap(frag_buf_);
        frag_total_ = 0;
        if (whole.size() != total) { invalid("fragment overflow"); return; }
        deliver(whole);
        return;
    }
    if (frag_total_ != 0) {   // message ordinaire au milieu d'un réassemblage : le flux est incohérent
        invalid("message inside fragmented message");
        frag_buf_.clear();
        frag_total_ = 0;
    }
    wire::Reader r(raw.data(), raw.size());
    std::string err;
    auto m = proto::decode(r, err);
    if (!m) { invalid("invalid message: " + err); return; }   // message ignoré, la suite reste lisible
    inbox_.push_back(std::move(*m));
    if (on_delivered) on_delivered(inbox_.back());
}

void Link::on_datagram(const uint8_t* data, size_t len, double now)
{
    wire::Reader r(data, len);
    const uint32_t magic = r.u32();
    const uint16_t proto_v = r.u16();
    const uint32_t ack = r.u32();
    const uint8_t n = r.u8();
    if (!r.ok() || magic != proto::kMagic) { invalid("bad packet header"); return; }
    if (proto_v != proto::kProtocolVersion) { invalid("protocol version " + std::to_string(proto_v)); return; }
    last_heard_ = now;

    // Acquittement cumulatif
    while (!outbox_.empty() && outbox_.front().id <= ack) outbox_.pop_front();

    size_t pos = 4 + 2 + 4 + 1;
    for (uint8_t i = 0; i < n; ++i) {
        // msg_id u32 | type u16 | longueur u16 | charge utile
        if (len - pos < 8) { invalid("truncated packet"); return; }
        const uint32_t id = get_u32(data + pos);
        const uint16_t body = get_u16(data + pos + 6);
        if (len - pos - 8 < body) { invalid("truncated packet"); return; }
        std::vector<uint8_t> raw(data + pos + 4, data + pos + 8 + body);
        pos += 8 + body;
        if (id == 0) {   // non fiable : jamais fragmenté
            wire::Reader mr(raw.data(), raw.size());
            std::string err;
            auto m = proto::decode(mr, err);
            if (!m) { invalid("invalid message: " + err); continue; }
            inbox_.push_back(std::move(*m));
            if (on_delivered) on_delivered(inbox_.back());
            continue;
        }
        dirty_ = true;   // il faut acquitter
        if (id <= recv_contiguous_) continue;                                          // doublon
        if (id == recv_contiguous_ + 1) {
            ++recv_contiguous_;
            deliver(raw);
            for (auto it = out_of_order_.find(recv_contiguous_ + 1); it != out_of_order_.end(); it = out_of_order_.find(recv_contiguous_ + 1)) {
                ++recv_contiguous_;
                deliver(it->second);
                out_of_order_.erase(it);
            }
        } else if (out_of_order_.size() < kMaxOutOfOrder) {
            out_of_order_.emplace(id, std::move(raw));
        }
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
        // Message plus gros qu'un paquet nominal (non fiable seulement : les fiables sont fragmentés) : seul.
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
        if (rel_index >= 64) break;   // pas plus de 64 messages fiables renvoyés par vague
    }
    last_sent_ = now;
    dirty_ = false;
}

} // namespace e33::net
