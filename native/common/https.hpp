#pragma once
// Téléchargement HTTPS (WinHTTP) restreint aux hôtes GitHub de release::allowed_host : redirections suivies à la main
// (HTTPS seulement, hôte vérifié à chaque saut), taille plafonnée. Partagé par le mod (vérification des versions) et
// l'installateur. Lier winhttp.lib.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>

#include <atomic>
#include <functional>
#include <string>

#include "minijson.hpp"
#include "release.hpp"

namespace e33::https {

struct Options {
    size_t max_bytes = 1 << 20;
    int timeout_ms = 8000;
    std::function<void(uint64_t done, uint64_t total)> progress;
    const std::atomic<bool>* cancel = nullptr;
};

inline std::string narrow(const std::wstring& w)
{
    std::string s;
    for (wchar_t c : w) s.push_back(c < 128 ? static_cast<char>(c) : '?');
    return s;
}

namespace detail {
struct Handle {
    HINTERNET h = nullptr;
    ~Handle() { if (h) WinHttpCloseHandle(h); }
};
} // namespace detail

// GET de url (https://<hôte autorisé>/...) ; body reçoit le contenu. false + err sinon.
inline bool get(std::wstring url, std::string& body, std::string& err, const Options& o = {})
{
    body.clear();
    detail::Handle session;
    session.h = WinHttpOpen(L"E33Coop-kipavy", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.h) session.h = WinHttpOpen(L"E33Coop-kipavy", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.h) { err = "WinHttpOpen " + std::to_string(GetLastError()); return false; }
    WinHttpSetTimeouts(session.h, o.timeout_ms, o.timeout_ms, o.timeout_ms, o.timeout_ms);
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
    protocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
    WinHttpSetOption(session.h, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols);
    for (int hop = 0; hop < 6; ++hop) {
        URL_COMPONENTS uc{};
        uc.dwStructSize = sizeof uc;
        wchar_t host[256]{}, path[4096]{};
        uc.lpszHostName = host; uc.dwHostNameLength = static_cast<DWORD>(std::size(host));
        uc.lpszUrlPath = path; uc.dwUrlPathLength = static_cast<DWORD>(std::size(path));
        wchar_t extra[4096]{};
        uc.lpszExtraInfo = extra; uc.dwExtraInfoLength = static_cast<DWORD>(std::size(extra));
        if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) { err = "URL invalide"; return false; }
        const std::string h = release::to_lower(narrow(host));
        if (uc.nScheme != INTERNET_SCHEME_HTTPS || uc.nPort != INTERNET_DEFAULT_HTTPS_PORT || !release::allowed_host(h)) {
            err = "hôte refusé : " + h;
            return false;
        }
        detail::Handle conn, req;
        conn.h = WinHttpConnect(session.h, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (!conn.h) { err = "connexion à " + h + " impossible (" + std::to_string(GetLastError()) + ")"; return false; }
        const std::wstring object = std::wstring(path) + extra;
        req.h = WinHttpOpenRequest(conn.h, L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (!req.h) { err = "requête impossible"; return false; }
        DWORD no_redirect = WINHTTP_DISABLE_REDIRECTS;
        WinHttpSetOption(req.h, WINHTTP_OPTION_DISABLE_FEATURE, &no_redirect, sizeof no_redirect);
        const wchar_t* headers = L"Accept: application/vnd.github+json, application/octet-stream, */*\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
        if (!WinHttpSendRequest(req.h, headers, static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(req.h, nullptr)) {
            err = "réseau indisponible (" + std::to_string(GetLastError()) + ")";
            return false;
        }
        DWORD status = 0, len = sizeof status;
        WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);
        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            wchar_t loc[4096]{};
            DWORD ll = sizeof loc;
            if (!WinHttpQueryHeaders(req.h, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX, loc, &ll, WINHTTP_NO_HEADER_INDEX)) { err = "redirection sans adresse"; return false; }
            url = loc;
            continue;
        }
        if (status != 200) { err = "HTTP " + std::to_string(status) + " (" + h + ")"; return false; }
        uint64_t total = 0;
        wchar_t cl[32]{};
        DWORD cll = sizeof cl;
        if (WinHttpQueryHeaders(req.h, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, cl, &cll, WINHTTP_NO_HEADER_INDEX)) total = _wcstoui64(cl, nullptr, 10);
        if (total > o.max_bytes) { err = "fichier trop gros"; return false; }
        if (total) body.reserve(static_cast<size_t>(total));
        for (;;) {
            if (o.cancel && o.cancel->load()) { err = "annulé"; return false; }
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(req.h, &avail)) { err = "téléchargement interrompu (" + std::to_string(GetLastError()) + ")"; return false; }
            if (avail == 0) break;
            if (body.size() + avail > o.max_bytes) { err = "fichier trop gros"; return false; }
            const size_t at = body.size();
            body.resize(at + avail);
            DWORD got = 0;
            if (!WinHttpReadData(req.h, body.data() + at, avail, &got)) { err = "lecture interrompue"; return false; }
            body.resize(at + got);
            if (o.progress) o.progress(body.size(), total);
        }
        if (total && body.size() != total) { err = "téléchargement incomplet"; return false; }
        return true;
    }
    err = "trop de redirections";
    return false;
}

// Tag de la dernière release (API GitHub) ; vide + err si indisponible ou tag invalide.
inline std::string latest_tag(std::string& err, int timeout_ms)
{
    std::string body;
    Options o;
    o.timeout_ms = timeout_ms;
    o.max_bytes = 2u << 20;
    if (!get(L"https://api.github.com/repos/kipavy/e33-coop/releases/latest", body, err, o)) return {};
    json::Value v;
    std::string perr;
    if (!json::parse(body, v, perr)) { err = "réponse GitHub illisible"; return {}; }
    const std::string tag = v["tag_name"].as_string("");
    if (!release::valid_tag(tag)) { err = "tag de release invalide"; return {}; }
    return tag;
}

inline std::wstring asset_url(const std::string& tag, const std::string& asset)
{
    std::wstring w = L"https://github.com/kipavy/e33-coop/releases/download/";
    for (char c : tag) w.push_back(static_cast<wchar_t>(c));
    w.push_back(L'/');
    for (char c : asset) w.push_back(static_cast<wchar_t>(c));
    return w;
}

} // namespace e33::https
