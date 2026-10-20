// Encodage binaire little-endian, avec lecture bornée : toute lecture hors limites met le lecteur
// en erreur (ok() == false) au lieu de lire de la mémoire invalide. Aucune exception.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace e33::wire {

class Writer {
public:
    void u8(uint8_t v) { buf_.push_back(v); }
    void u16(uint16_t v) { raw(&v, 2); }
    void u32(uint32_t v) { raw(&v, 4); }
    void u64(uint64_t v) { raw(&v, 8); }
    void i32(int32_t v) { raw(&v, 4); }
    void f64(double v) { raw(&v, 8); }
    void boolean(bool v) { u8(v ? 1 : 0); }
    // Chaîne : longueur u16 + octets UTF-8 (tronquée à 65535)
    void str(std::string_view s)
    {
        const size_t n = s.size() > 0xFFFF ? 0xFFFF : s.size();
        u16(static_cast<uint16_t>(n));
        raw(s.data(), n);
    }
    void bytes(const void* p, size_t n) { raw(p, n); }
    const std::vector<uint8_t>& data() const { return buf_; }
    std::vector<uint8_t>& data() { return buf_; }
    size_t size() const { return buf_.size(); }

private:
    void raw(const void* p, size_t n)
    {
        const auto* b = static_cast<const uint8_t*>(p);
        buf_.insert(buf_.end(), b, b + n);
    }
    std::vector<uint8_t> buf_;
};

class Reader {
public:
    Reader(const uint8_t* p, size_t n) : p_(p), n_(n) {}
    bool ok() const { return ok_; }
    size_t remaining() const { return ok_ ? n_ - i_ : 0; }
    bool at_end() const { return ok_ && i_ == n_; }

    uint8_t u8() { uint8_t v = 0; raw(&v, 1); return v; }
    uint16_t u16() { uint16_t v = 0; raw(&v, 2); return v; }
    uint32_t u32() { uint32_t v = 0; raw(&v, 4); return v; }
    uint64_t u64() { uint64_t v = 0; raw(&v, 8); return v; }
    int32_t i32() { int32_t v = 0; raw(&v, 4); return v; }
    double f64() { double v = 0; raw(&v, 8); return v; }
    bool boolean()
    {
        const uint8_t v = u8();
        if (v > 1) ok_ = false;
        return v == 1;
    }
    std::string str(size_t max_len = 1024)
    {
        const uint16_t n = u16();
        if (!ok_ || n > max_len || n > remaining()) { ok_ = false; return {}; }
        std::string s(reinterpret_cast<const char*>(p_ + i_), n);
        i_ += n;
        return s;
    }
    // Compteur d'éléments borné (protège contre les tableaux géants annoncés).
    uint16_t count(uint16_t max)
    {
        const uint16_t n = u16();
        if (n > max) ok_ = false;
        return ok_ ? n : 0;
    }
    // Octets bruts préfixés par leur longueur (u16), bornés.
    std::vector<uint8_t> blob(size_t max_len)
    {
        const uint16_t n = u16();
        if (!ok_ || n > max_len || n > remaining()) { ok_ = false; return {}; }
        std::vector<uint8_t> v(p_ + i_, p_ + i_ + n);
        i_ += n;
        return v;
    }
    void fail() { ok_ = false; }

private:
    void raw(void* out, size_t k)
    {
        if (!ok_ || k > n_ - i_) { ok_ = false; return; }
        std::memcpy(out, p_ + i_, k);
        i_ += k;
    }
    const uint8_t* p_;
    size_t n_;
    size_t i_ = 0;
    bool ok_ = true;
};

// FNV-1a 64 bits (hash d'état pour la détection de désync)
class Fnv64 {
public:
    void bytes(const void* p, size_t n)
    {
        const auto* b = static_cast<const uint8_t*>(p);
        for (size_t i = 0; i < n; ++i) { h_ ^= b[i]; h_ *= 1099511628211ull; }
    }
    void u64(uint64_t v) { bytes(&v, 8); }
    void i64(int64_t v) { bytes(&v, 8); }
    void str(std::string_view s) { u64(s.size()); bytes(s.data(), s.size()); }
    uint64_t value() const { return h_; }

private:
    uint64_t h_ = 1469598103934665603ull;
};

} // namespace e33::wire
