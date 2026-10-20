#include "log.hpp"

#include <cwchar>
#include <filesystem>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define NOGDI
#include <Windows.h>
#include <shellapi.h>

#include "../common/cmdline.hpp"

namespace e33 {

JsonLog& log()
{
    static JsonLog l;
    return l;
}

namespace {

LaunchArgs parse()
{
    LaunchArgs a;
    a.run_dir = cmdline::arg(L"e33run");
    a.test = cmdline::arg(L"e33test");
    a.role = cmdline::arg(L"e33role");
    a.instance = cmdline::arg(L"e33inst");
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
