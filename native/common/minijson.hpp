// Analyseur JSON minimal (lecture seule), sans exception : une erreur donne un Value nul + message.
// Suffisant pour les scénarios de test et la configuration.
#pragma once

#include <cstdint>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace e33::json {

struct Value {
    enum class Type { Null, Bool, Number, String, Array, Object } type = Type::Null;
    bool b = false;
    double num = 0;
    std::string str;
    std::vector<Value> arr;
    std::map<std::string, Value, std::less<>> obj;

    bool is_null() const { return type == Type::Null; }
    bool is_object() const { return type == Type::Object; }
    bool is_array() const { return type == Type::Array; }
    bool is_string() const { return type == Type::String; }
    bool is_number() const { return type == Type::Number; }

    const Value& operator[](std::string_view k) const
    {
        static const Value null_v;
        if (type != Type::Object) return null_v;
        auto it = obj.find(k);
        return it == obj.end() ? null_v : it->second;
    }
    const Value& operator[](size_t i) const
    {
        static const Value null_v;
        return (type == Type::Array && i < arr.size()) ? arr[i] : null_v;
    }
    size_t size() const { return type == Type::Array ? arr.size() : type == Type::Object ? obj.size() : 0; }
    std::string as_string(std::string_view def = "") const { return type == Type::String ? str : std::string(def); }
    double as_number(double def = 0) const { return type == Type::Number ? num : def; }
    int64_t as_int(int64_t def = 0) const { return type == Type::Number ? static_cast<int64_t>(num) : def; }
    bool as_bool(bool def = false) const { return type == Type::Bool ? b : def; }
};

class Parser {
public:
    explicit Parser(std::string_view s) : s_(s) {}

    bool parse(Value& out, std::string& err)
    {
        ws();
        if (!value(out, 0)) { err = err_ + " at offset " + std::to_string(i_); return false; }
        ws();
        if (i_ != s_.size()) { err = "trailing characters at offset " + std::to_string(i_); return false; }
        return true;
    }

private:
    std::string_view s_;
    size_t i_ = 0;
    std::string err_;

    void ws() { while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r')) ++i_; }
    bool fail(const char* m) { err_ = m; return false; }

    bool value(Value& v, int depth)
    {
        if (depth > 64) return fail("too deep");
        if (i_ >= s_.size()) return fail("unexpected end");
        const char c = s_[i_];
        if (c == '{') return object(v, depth);
        if (c == '[') return array(v, depth);
        if (c == '"') { v.type = Value::Type::String; return string(v.str); }
        if (c == 't' && s_.substr(i_, 4) == "true") { i_ += 4; v.type = Value::Type::Bool; v.b = true; return true; }
        if (c == 'f' && s_.substr(i_, 5) == "false") { i_ += 5; v.type = Value::Type::Bool; v.b = false; return true; }
        if (c == 'n' && s_.substr(i_, 4) == "null") { i_ += 4; v.type = Value::Type::Null; return true; }
        if (c == '-' || (c >= '0' && c <= '9')) return number(v);
        return fail("unexpected character");
    }

    bool number(Value& v)
    {
        const size_t start = i_;
        if (s_[i_] == '-') ++i_;
        while (i_ < s_.size() && ((s_[i_] >= '0' && s_[i_] <= '9') || s_[i_] == '.' || s_[i_] == 'e' || s_[i_] == 'E' || s_[i_] == '+' || s_[i_] == '-')) ++i_;
        const std::string tmp(s_.substr(start, i_ - start));
        char* end = nullptr;
        v.num = std::strtod(tmp.c_str(), &end);
        if (!end || *end != '\0') return fail("bad number");
        v.type = Value::Type::Number;
        return true;
    }

    static void put_utf8(std::string& o, uint32_t cp)
    {
        if (cp < 0x80) o += static_cast<char>(cp);
        else if (cp < 0x800) { o += static_cast<char>(0xC0 | (cp >> 6)); o += static_cast<char>(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { o += static_cast<char>(0xE0 | (cp >> 12)); o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); o += static_cast<char>(0x80 | (cp & 0x3F)); }
        else { o += static_cast<char>(0xF0 | (cp >> 18)); o += static_cast<char>(0x80 | ((cp >> 12) & 0x3F)); o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); o += static_cast<char>(0x80 | (cp & 0x3F)); }
    }

    bool hex4(uint32_t& cp)
    {
        if (i_ + 4 > s_.size()) return fail("bad \\u escape");
        cp = 0;
        for (int k = 0; k < 4; ++k) {
            const char h = s_[i_++];
            cp <<= 4;
            if (h >= '0' && h <= '9') cp |= h - '0';
            else if (h >= 'a' && h <= 'f') cp |= h - 'a' + 10;
            else if (h >= 'A' && h <= 'F') cp |= h - 'A' + 10;
            else return fail("bad hex digit");
        }
        return true;
    }

    bool string(std::string& o)
    {
        ++i_; // "
        while (i_ < s_.size()) {
            const char c = s_[i_++];
            if (c == '"') return true;
            if (c != '\\') { o += c; continue; }
            if (i_ >= s_.size()) break;
            const char e = s_[i_++];
            switch (e) {
            case '"': o += '"'; break;
            case '\\': o += '\\'; break;
            case '/': o += '/'; break;
            case 'b': o += '\b'; break;
            case 'f': o += '\f'; break;
            case 'n': o += '\n'; break;
            case 'r': o += '\r'; break;
            case 't': o += '\t'; break;
            case 'u': {
                uint32_t cp;
                if (!hex4(cp)) return false;
                if (cp >= 0xD800 && cp <= 0xDBFF && i_ + 6 <= s_.size() && s_[i_] == '\\' && s_[i_ + 1] == 'u') {
                    i_ += 2;
                    uint32_t lo;
                    if (!hex4(lo)) return false;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                put_utf8(o, cp);
                break;
            }
            default: return fail("bad escape");
            }
        }
        return fail("unterminated string");
    }

    bool array(Value& v, int depth)
    {
        v.type = Value::Type::Array;
        ++i_;
        ws();
        if (i_ < s_.size() && s_[i_] == ']') { ++i_; return true; }
        while (true) {
            Value e;
            ws();
            if (!value(e, depth + 1)) return false;
            v.arr.push_back(std::move(e));
            ws();
            if (i_ >= s_.size()) return fail("unterminated array");
            if (s_[i_] == ',') { ++i_; continue; }
            if (s_[i_] == ']') { ++i_; return true; }
            return fail("expected , or ]");
        }
    }

    bool object(Value& v, int depth)
    {
        v.type = Value::Type::Object;
        ++i_;
        ws();
        if (i_ < s_.size() && s_[i_] == '}') { ++i_; return true; }
        while (true) {
            ws();
            if (i_ >= s_.size() || s_[i_] != '"') return fail("expected key");
            std::string k;
            if (!string(k)) return false;
            ws();
            if (i_ >= s_.size() || s_[i_] != ':') return fail("expected :");
            ++i_;
            ws();
            Value e;
            if (!value(e, depth + 1)) return false;
            v.obj[std::move(k)] = std::move(e);
            ws();
            if (i_ >= s_.size()) return fail("unterminated object");
            if (s_[i_] == ',') { ++i_; continue; }
            if (s_[i_] == '}') { ++i_; return true; }
            return fail("expected , or }");
        }
    }
};

inline bool parse(std::string_view text, Value& out, std::string& err)
{
    // tolère un BOM UTF-8
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) text.remove_prefix(3);
    return Parser(text).parse(out, err);
}

} // namespace e33::json
