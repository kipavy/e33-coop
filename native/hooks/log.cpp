#include "log.hpp"

#include <cwchar>
#include <filesystem>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define NOGDI
#include <Windows.h>
#include <shellapi.h>

namespace e33 {

JsonLog& log()
{
    static JsonLog l;
    return l;
}

namespace {

std::wstring arg_value(const std::wstring& arg, const wchar_t* key)
{
    const std::wstring k = std::wstring(L"-") + key + L"=";
    if (arg.size() < k.size()) return {};
    for (size_t i = 0; i < k.size(); ++i)
        if (towlower(arg[i]) != towlower(k[i])) return {};
    std::wstring v = arg.substr(k.size());
    if (v.size() >= 2 && v.front() == L'"' && v.back() == L'"') v = v.substr(1, v.size() - 2);
    return v;
}

LaunchArgs parse()
{
    LaunchArgs a;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 0; argv && i < argc; ++i) {
        const std::wstring s = argv[i];
        if (auto v = arg_value(s, L"e33run"); !v.empty()) a.run_dir = v;
        if (auto v = arg_value(s, L"e33test"); !v.empty()) a.test = v;
        if (auto v = arg_value(s, L"e33role"); !v.empty()) a.role = v;
        if (auto v = arg_value(s, L"e33inst"); !v.empty()) a.instance = v;
    }
    if (argv) LocalFree(argv);
    if (a.role.empty()) a.role = L"solo";
    if (a.instance.empty()) a.instance = a.role;
    if (a.run_dir.empty()) {
        // Partie normale (pas de test) : un dossier par lancement, pour qu'une relance ne recouvre pas le journal précédent.
        SYSTEMTIME t{};
        GetLocalTime(&t);
        wchar_t stamp[32];
        swprintf_s(stamp, L"%04u%02u%02u-%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        a.run_dir = (std::filesystem::path(mod_dir()) / L"logs" / stamp).wstring();
    }
    return a;
}

} // namespace

const LaunchArgs& launch_args()
{
    static const LaunchArgs a = parse();
    return a;
}

std::wstring mod_dir()
{
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&mod_dir), &self);
    wchar_t buf[MAX_PATH * 2]{};
    GetModuleFileNameW(self, buf, static_cast<DWORD>(std::size(buf)));
    // …/Mods/E33Coop/dlls/main.dll -> …/Mods/E33Coop
    return std::filesystem::path(buf).parent_path().parent_path().wstring();
}

} // namespace e33
