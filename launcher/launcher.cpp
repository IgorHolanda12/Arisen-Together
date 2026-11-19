#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include <bcrypt.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <winhttp.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <string>
#include <vector>

#include "../src/version.h"
#include "i18n.h"
#include "theme.h"

namespace {

const wchar_t kTitle[] = L"Arisen Together";
const char kSupportedSha[] = "19FACC2642F79A09D14CE55EACE5F228161F2F8BAB60EC67EB3C6196CC6277B8";
const char kGogSha[] = "27140654C0FC5B78C26E377799E53B1C1CACACB4E4635918711D8EB0AE86BDE8";
const int kLanPort = 27101;
const char kOurMarker[] = "arisen-together loaded";
const char kOldMarker[] = "ddda-pawn-coop loaded";

enum {
    ID_GAME_EDIT = 100, ID_BROWSE, ID_HOST, ID_GUEST, ID_OWN_CHAR, ID_DEBUG, ID_LAUNCH, ID_REMOVE,
    ID_BACKUP, ID_EXPORT, ID_EVENTS, ID_SOLO, ID_KEEP, ID_TIMER, ID_PLAYERS, ID_HELP, ID_EVENTS_TOGGLE, ID_FIRST_HELP,
    ID_OPEN_BACKUPS, ID_COPY_CODE, ID_CODE_EDIT, ID_CODE_CLEAR, ID_BACKUP_LIST, ID_UPDATE, ID_CHECK_UPDATES, ID_PROJECT, ID_LANG,
    ID_LAN, ID_STORE_STEAM, ID_STORE_GOG, ID_GOG_HELP, ID_UNSUPPORTED_HELP,
    ID_PARTY,
    ID_NAV = 200
};
enum Page { PAGE_PLAY, PAGE_SALON, PAGE_SAVES, PAGE_SETTINGS, PAGE_LOG, PAGE_COUNT };
const wchar_t *const kPageNames[PAGE_COUNT] = {L"Jouer", L"Salon", L"Sauvegardes", L"Paramètres", L"Journal"};
int g_page = PAGE_PLAY;
uint64_t g_self_steam = 0;
HWND g_code_edit = nullptr, g_copy_code = nullptr, g_code_clear = nullptr, g_backup_list = nullptr;
std::vector<std::wstring> g_backup_dirs;

const char *const kCode = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
std::wstring code_from_steam(uint64_t id)
{
    uint32_t acct = uint32_t(id & 0xFFFFFFFFu);
    wchar_t c[9];
    for (int k = 6; k >= 0; --k) {
        c[k < 3 ? k : k + 1] = wchar_t(kCode[acct & 31]);
        acct >>= 5;
    }
    c[3] = L'-';
    c[8] = 0;
    return c;
}
bool steam_from_code(const std::wstring &text, uint64_t &id)
{
    uint64_t v = 0;
    int n = 0;
    for (wchar_t ch : text) {
        if (ch == L'-' || ch == L' ')
            continue;
        wchar_t u = wchar_t(towupper(ch));
        if (u == L'O')
            u = L'0';
        if (u == L'I' || u == L'L')
            u = L'1';
        const char *at = u < 128 ? strchr(kCode, char(u)) : nullptr;
        if (!at || !*at)
            return false;
        v = v * 32 + uint64_t(at - kCode);
        ++n;
    }
    if (n != 7 || v > 0xFFFFFFFFull || v == 0)
        return false;
    id = 76561197960265728ull + v;
    return true;
}
enum Role { ROLE_HOST, ROLE_GUEST, ROLE_SOLO };
int g_role = ROLE_HOST;
struct Switch {
    HWND h;
    bool on;
    std::wstring label;
};
std::vector<Switch> g_switches;
int g_game_state = 0;
bool g_log_unseen = false;

HWND g_wnd, g_game_edit, g_version, g_host, g_guest, g_solo, g_own_char, g_keep, g_debug, g_launch, g_state, g_events,
    g_players, g_party, g_backup_note, g_update, g_check_updates, g_lan, g_project;
HFONT g_font, g_bold, g_head, g_tab, g_wordmark, g_icon, g_small, g_small_icon, g_code_font;
HBRUSH g_bg_brush, g_card_brush;
int g_dpi = 96;
RECT g_state_rect = {};

const COLORREF kBg = theme::kBg, kCard = theme::kSurface, kText = theme::kText, kMuted = theme::kMuted,
               kOk = theme::kOk, kWarn = theme::kWarn, kBad = theme::kBad, kInfo = theme::kInfo;
int px(int v) { return MulDiv(v, g_dpi, 96); }
const int kW = 960, kH = 600, kTopH = 64, kX = 40, kCW = kW - 2 * kX;
const int kTabX = 300, kTabW = 108, kLangW = 80;
const int kConnY = 384, kConnH = 116;
const int kDangerY = 378;
const int kSavesListY = kTopH + 22 + 168;
const int kLogCardY = kTopH + 50;
const int kRoleY = kTopH + 56, kRoleH = 130;
std::wstring g_exe_dir, g_game_dir, g_steam_dir, g_docs_dir;
bool g_no_launch = false;
std::wstring g_test_update;
std::wstring g_test_releases;
int g_start_page = 0;
bool g_supported = false;
std::string g_bad_sha;
std::wstring g_bad_told_for;
bool g_gog = false;
int g_store = 0;
std::wstring g_steam_game, g_gog_game, g_gog_arg, g_gog_storage_arg;
HWND g_store_steam = nullptr, g_store_gog = nullptr;
COLORREF g_state_color = kMuted;
FILETIME g_launch_time = {};
long long g_log_pos = 0;
bool g_watching = false;
int g_friends = 0;
bool g_world_received = false;
DWORD g_launch_tick = 0, g_steam_wait_since = 0;
bool g_mod_loaded = false, g_link_up = false, g_host_found = false, g_game_seen = false, g_told_no_mod = false,
     g_told_steam = false, g_told_no_host = false, g_told_closed = false;

std::wstring widen(const std::string &s)
{
    if (s.empty())
        return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), &w[0], n);
    return w;
}

bool exists(const std::wstring &p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
bool is_dir(const std::wstring &p)
{
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool read_file(const std::wstring &p, std::string &out, long long from = 0)
{
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER size = {}, pos = {};
    GetFileSizeEx(h, &size);
    out.clear();
    if (size.QuadPart > from) {
        pos.QuadPart = from;
        SetFilePointerEx(h, pos, nullptr, FILE_BEGIN);
        out.resize(size_t(size.QuadPart - from));
        DWORD got = 0;
        ReadFile(h, &out[0], DWORD(out.size()), &got, nullptr);
        out.resize(got);
    }
    CloseHandle(h);
    return true;
}

bool write_file(const std::wstring &p, const std::string &data)
{
    HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    DWORD put = 0;
    bool ok = WriteFile(h, data.data(), DWORD(data.size()), &put, nullptr) && put == data.size();
    CloseHandle(h);
    return ok;
}

std::string sha256_file(const std::wstring &p)
{
    std::string data;
    if (!read_file(p, data))
        return {};
    BCRYPT_ALG_HANDLE alg = nullptr;
    unsigned char digest[32] = {};
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
        return {};
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
        BCryptHashData(hash, reinterpret_cast<PUCHAR>(&data[0]), ULONG(data.size()), 0);
        BCryptFinishHash(hash, digest, sizeof digest, 0);
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    std::string hex;
    char b[3];
    for (unsigned char c : digest) {
        snprintf(b, sizeof b, "%02X", c);
        hex += b;
    }
    return hex;
}

bool game_running()
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return false;
    PROCESSENTRY32W pe = {sizeof pe};
    bool found = false;
    for (BOOL ok = Process32FirstW(snap, &pe); ok && !found; ok = Process32NextW(snap, &pe))
        found = _wcsicmp(pe.szExeFile, L"DDDA.exe") == 0;
    CloseHandle(snap);
    return found;
}

std::wstring window_text(HWND h)
{
    int n = GetWindowTextLengthW(h);
    std::wstring s(n, 0);
    if (n)
        GetWindowTextW(h, &s[0], n + 1);
    return s;
}

int ask(const wchar_t *text, UINT flags) { return MessageBoxW(g_wnd, text, kTitle, flags); }

std::wstring steam_dir()
{
    wchar_t buf[MAX_PATH] = {};
    DWORD size = sizeof buf;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr, buf, &size))
        return {};
    std::wstring s = buf;
    for (auto &c : s)
        if (c == L'/')
            c = L'\\';
    return s;
}

std::vector<std::string> vdf_values(const std::string &text, const char *key)
{
    std::vector<std::string> out;
    std::string k = std::string("\"") + key + "\"";
    for (size_t at = text.find(k); at != std::string::npos; at = text.find(k, at + 1)) {
        size_t q1 = text.find('"', at + k.size());
        if (q1 == std::string::npos)
            break;
        std::string v;
        size_t i = q1 + 1;
        for (; i < text.size() && text[i] != '"' && text[i] != '\n'; ++i) {
            if (text[i] == '\\' && i + 1 < text.size())
                ++i;
            v += text[i];
        }
        out.push_back(v);
    }
    return out;
}

std::wstring find_game(const std::wstring &steam)
{
    if (steam.empty())
        return {};
    std::vector<std::wstring> libs = {steam};
    std::string vdf;
    if (read_file(steam + L"\\steamapps\\libraryfolders.vdf", vdf))
        for (auto &p : vdf_values(vdf, "path"))
            libs.push_back(widen(p));
    for (auto &lib : libs) {
        std::wstring dir = L"DDDA";
        std::string acf;
        if (read_file(lib + L"\\steamapps\\appmanifest_367500.acf", acf)) {
            auto v = vdf_values(acf, "installdir");
            if (!v.empty())
                dir = widen(v[0]);
        }
        std::wstring game = lib + L"\\steamapps\\common\\" + dir;
        if (exists(game + L"\\DDDA.exe"))
            return game;
    }
    return {};
}

std::wstring find_gog_game()
{
    if (!g_gog_arg.empty())
        return exists(g_gog_arg + L"\\DDDA.exe") ? g_gog_arg : std::wstring();
    wchar_t buf[MAX_PATH] = {};
    DWORD size = sizeof buf;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\GOG.com\\Games\\1242384383", L"path", RRF_RT_REG_SZ,
                     nullptr, buf, &size))
        return {};
    std::wstring game = buf;
    return exists(game + L"\\DDDA.exe") ? game : std::wstring();
}

std::wstring settings_path()
{
    std::wstring p = g_exe_dir + L"\\ArisenTogether.ini";
    static bool moved = false;
    if (!moved) {
        moved = true;
        std::wstring old = g_exe_dir + L"\\DDDA-Coop.ini";
        if (!exists(p) && exists(old))
            CopyFileW(old.c_str(), p.c_str(), TRUE);
    }
    return p;
}

std::wstring setting(const wchar_t *key, const wchar_t *def)
{
    wchar_t buf[1024];
    GetPrivateProfileStringW(L"launcher", key, def, buf, 1024, settings_path().c_str());
    return buf;
}

bool switch_on(HWND h)
{
    for (const Switch &w : g_switches)
        if (w.h == h)
            return w.on;
    return false;
}

void save_settings()
{
    auto p = settings_path();
    WritePrivateProfileStringW(L"launcher", L"game", g_game_dir.c_str(), p.c_str());
    WritePrivateProfileStringW(L"launcher", L"role", g_role == ROLE_GUEST ? L"guest" : g_role == ROLE_SOLO ? L"solo" : L"host",
                               p.c_str());
    WritePrivateProfileStringW(L"launcher", L"keep_progress", switch_on(g_keep) ? L"1" : L"0", p.c_str());
    WritePrivateProfileStringW(L"launcher", L"own_character", switch_on(g_own_char) ? L"1" : L"0", p.c_str());
    WritePrivateProfileStringW(L"launcher", L"debug_log", switch_on(g_debug) ? L"1" : L"0", p.c_str());
    WritePrivateProfileStringW(L"launcher", L"check_updates", switch_on(g_check_updates) ? L"1" : L"0", p.c_str());
    WritePrivateProfileStringW(L"launcher", L"direct_ip", switch_on(g_lan) ? L"1" : L"0", p.c_str());
}

const wchar_t kPublicRepo[] = L"IgorHolanda12/Arisen-Together";
const std::wstring kRepoUrl = std::wstring(L"https://github.com/") + kPublicRepo;
const UINT WM_UPDATE_FOUND = WM_APP + 7;
std::wstring g_update_tag;
std::wstring g_update_url;
std::string g_update_sha;
void add_event(const std::wstring &text);

bool parse_version(const std::string &t, int out[3])
{
    size_t i = t.size() && (t[0] == 'v' || t[0] == 'V') ? 1 : 0;
    for (int k = 0; k < 3; ++k) {
        if (i >= t.size() || !isdigit(static_cast<unsigned char>(t[i])))
            return false;
        out[k] = 0;
        while (i < t.size() && isdigit(static_cast<unsigned char>(t[i])))
            out[k] = out[k] * 10 + (t[i++] - '0');
        if (k < 2 && (i >= t.size() || t[i++] != '.'))
            return false;
    }
    return true;
}

DWORD WINAPI check_updates(void *)
{
    if (g_test_update.empty() && !kPublicRepo[0])
        return 0;
    std::string body;
    if (!g_test_update.empty())
        body = "[{\"tag_name\": \"" + std::string(g_test_update.begin(), g_test_update.end()) + "\"}]";
    else if (!g_test_releases.empty())
        read_file(g_test_releases, body);
    else {
    HINTERNET s = WinHttpOpen((L"ArisenTogether/" + widen(DDDA_COOP_VERSION)).c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET c = s ? WinHttpConnect(s, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0) : nullptr;
    std::wstring path = std::wstring(L"/repos/") + kPublicRepo + L"/releases?per_page=1";
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path.c_str(), nullptr,
                                         WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                    : nullptr;
    DWORD status = 0, size = sizeof status;
    if (r) {
        WinHttpSetTimeouts(r, 5000, 5000, 5000, 5000);
        if (WinHttpSendRequest(r, L"Accept: application/vnd.github+json\r\n", DWORD(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(r, nullptr) &&
            WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status,
                                &size, WINHTTP_NO_HEADER_INDEX) &&
            status == 200) {
            char buf[4096];
            DWORD got = 0;
            while (body.size() < 256 * 1024 && WinHttpReadData(r, buf, sizeof buf, &got) && got)
                body.append(buf, got);
        }
    }
    for (HINTERNET x : {r, c, s})
        if (x)
            WinHttpCloseHandle(x);
    }
    size_t k = body.find("\"tag_name\"");
    size_t q1 = k == std::string::npos ? k : body.find('"', body.find(':', k) + 1);
    size_t q2 = q1 == std::string::npos ? q1 : body.find('"', q1 + 1);
    if (q2 == std::string::npos || q2 - q1 > 32)
        return 0;
    std::string tag = body.substr(q1 + 1, q2 - q1 - 1);
    int have[3], got[3];
    if (!parse_version(DDDA_COOP_VERSION, have) || !parse_version(tag, got))
        return 0;
    if (std::lexicographical_compare(have, have + 3, got, got + 3)) {
        size_t n = body.find("\"name\"", q2);
        static const char kZip[] = "ArisenTogether";
        while (n != std::string::npos && body.compare(body.find('"', body.find(':', n) + 1) + 1, sizeof kZip - 1, kZip) != 0)
            n = body.find("\"name\"", n + 6);
        auto value = [&](const char *field) -> std::string {
            size_t f = n == std::string::npos ? n : body.find(field, n);
            size_t a = f == std::string::npos ? f : body.find('"', body.find(':', f) + 1);
            size_t b = a == std::string::npos ? a : body.find('"', a + 1);
            return b == std::string::npos || b - a > 512 ? std::string() : body.substr(a + 1, b - a - 1);
        };
        std::string url = value("\"browser_download_url\""), digest = value("\"digest\"");
        std::wstring wurl = widen(url), prefix = kRepoUrl + L"/releases/download/";
        if (kPublicRepo[0] && wurl.rfind(prefix, 0) == 0 && digest.rfind("sha256:", 0) == 0) {
            g_update_url = widen(url);
            g_update_sha = digest.substr(7);
        }
        g_update_tag = widen(tag);
        PostMessageW(g_wnd, WM_UPDATE_FOUND, 0, 0);
    }
    return 0;
}

const UINT WM_UPDATE_DONE = WM_APP + 8;
std::wstring g_update_error;
bool g_updating = false;
std::wstring g_test_update_zip;
std::string g_test_update_sha;

bool download(const std::wstring &url, const std::wstring &to)
{
    URL_COMPONENTS u = {};
    u.dwStructSize = sizeof u;
    wchar_t host[256] = {}, path[2048] = {};
    u.lpszHostName = host;
    u.dwHostNameLength = 256;
    u.lpszUrlPath = path;
    u.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &u) || u.nScheme != INTERNET_SCHEME_HTTPS)
        return false;
    HINTERNET s = WinHttpOpen((L"ArisenTogether/" + widen(DDDA_COOP_VERSION)).c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET c = s ? WinHttpConnect(s, host, u.nPort, 0) : nullptr;
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         WINHTTP_FLAG_SECURE)
                    : nullptr;
    bool ok = false;
    DWORD status = 0, size = sizeof status;
    if (r) {
        WinHttpSetTimeouts(r, 10000, 10000, 30000, 30000);
        if (WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(r, nullptr) &&
            WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status,
                                &size, WINHTTP_NO_HEADER_INDEX) &&
            status == 200) {
            HANDLE f = CreateFileW(to.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f != INVALID_HANDLE_VALUE) {
                char buf[64 * 1024];
                DWORD got = 0, wrote = 0;
                long long total = 0;
                ok = true;
                while (ok && WinHttpReadData(r, buf, sizeof buf, &got) && got) {
                    total += got;
                    ok = total < 64LL * 1024 * 1024 && WriteFile(f, buf, got, &wrote, nullptr) && wrote == got;
                }
                CloseHandle(f);
            }
        }
    }
    for (HINTERNET x : {r, c, s})
        if (x)
            WinHttpCloseHandle(x);
    return ok;
}

bool run_wait(std::wstring cmd)
{
    STARTUPINFOW si = {};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return false;
    CloseHandle(pi.hThread);
    DWORD code = 1;
    if (WaitForSingleObject(pi.hProcess, 60000) == WAIT_OBJECT_0)
        GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    return code == 0;
}

DWORD WINAPI install_update(void *)
{
    auto fail = [](const wchar_t *why) {
        g_update_error = why;
        PostMessageW(g_wnd, WM_UPDATE_DONE, 0, 0);
        return DWORD(0);
    };
    wchar_t tmp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmp);
    std::wstring dir = std::wstring(tmp) + L"ArisenTogether-update", zip = dir + L"\\update.zip", files = dir + L"\\files";
    run_wait(L"cmd.exe /c rd /s /q \"" + dir + L"\"");
    CreateDirectoryW(dir.c_str(), nullptr);
    CreateDirectoryW(files.c_str(), nullptr);
    std::string want = g_test_update_zip.empty() ? g_update_sha : g_test_update_sha;
    if (g_test_update_zip.empty() ? !download(g_update_url, zip) : !CopyFileW(g_test_update_zip.c_str(), zip.c_str(), FALSE))
        return fail(tr(L"téléchargement impossible"));
    std::string sha = sha256_file(zip);
    if (sha.empty() || _stricmp(sha.c_str(), want.c_str()) != 0)
        return fail(tr(L"le fichier téléchargé n'est pas celui publié (SHA-256 différent)"));
    wchar_t sys[MAX_PATH] = {};
    GetSystemDirectoryW(sys, MAX_PATH);
    if (!run_wait(L"\"" + std::wstring(sys) + L"\\tar.exe\" -xf \"" + zip + L"\" -C \"" + files + L"\"") ||
        !exists(files + L"\\ArisenTogether.exe") || !exists(files + L"\\dinput8.dll"))
        return fail(tr(L"le zip ne contient pas ArisenTogether.exe et dinput8.dll"));
    WIN32_FIND_DATAW fd;
    HANDLE fh = FindFirstFileW((files + L"\\*").c_str(), &fd);
    bool ok = fh != INVALID_HANDLE_VALUE;
    for (bool more = ok; more; more = FindNextFileW(fh, &fd)) {
        std::wstring n = fd.cFileName;
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || _wcsicmp(n.c_str(), L"ArisenTogether.exe") == 0)
            continue;
        if (_wcsicmp(n.c_str(), L"coop.ini") == 0 && exists(g_exe_dir + L"\\coop.ini"))
            continue;
        ok = CopyFileW((files + L"\\" + n).c_str(), (g_exe_dir + L"\\" + n).c_str(), FALSE) && ok;
    }
    if (fh != INVALID_HANDLE_VALUE)
        FindClose(fh);
    std::wstring self = g_exe_dir + L"\\ArisenTogether.exe", old = self + L".old";
    if (ok) {
        DeleteFileW(old.c_str());
        ok = MoveFileExW(self.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
        if (ok && !CopyFileW((files + L"\\ArisenTogether.exe").c_str(), self.c_str(), FALSE)) {
            MoveFileExW(old.c_str(), self.c_str(), MOVEFILE_REPLACE_EXISTING);
            ok = false;
        }
    }
    if (!ok)
        return fail(tr(L"impossible de remplacer les fichiers du dossier de ArisenTogether.exe"));
    PostMessageW(g_wnd, WM_UPDATE_DONE, 1, 0);
    return 0;
}

void start_update()
{
    if (g_updating || (g_update_url.empty() && g_test_update_zip.empty()))
        return;
    g_updating = true;
    EnableWindow(g_update, FALSE);
    SetWindowTextW(g_update, (tr(L"Mise à jour ") + g_update_tag + tr(L" : installation…")).c_str());
    add_event(tr(L"Mise à jour vers ") + g_update_tag + tr(L" : téléchargement et vérification…"));
    if (HANDLE t = CreateThread(nullptr, 0, install_update, nullptr, 0, nullptr))
        CloseHandle(t);
}

void restart_launcher(HWND h)
{
    std::wstring cmd = GetCommandLineW();
    for (const wchar_t *opt : {L" --page ", L" --test-update ", L" --test-update-zip "})
        for (size_t at; (at = cmd.find(opt)) != std::wstring::npos;) {
            size_t end = cmd.find(L' ', at + wcslen(opt));
            if (wcscmp(opt, L" --test-update-zip ") == 0 && end != std::wstring::npos)
                end = cmd.find(L' ', end + 1);
            cmd.erase(at, end == std::wstring::npos ? std::wstring::npos : end - at);
        }
    cmd += L" --page " + std::to_wstring(g_page);
    STARTUPINFOW si = {};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi = {};
    if (CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        DestroyWindow(h);
    }
}

HBRUSH g_state_brush = nullptr;

void set_state(const std::wstring &text, COLORREF color)
{
    g_state_color = color;
    if (g_state_brush)
        DeleteObject(g_state_brush);
    g_state_brush = CreateSolidBrush(theme::mix(theme::kSurface, color, 0.08f));
    const wchar_t *sym = color == kOk ? L"\u2714  " : color == kBad ? L"\u2716  " : color == kWarn ? L"\u26A0  " : L"\u25CF  ";
    SetWindowTextW(g_state, (sym + text).c_str());
    InvalidateRect(g_state, nullptr, TRUE);
    InvalidateRect(g_wnd, &g_state_rect, TRUE);
}

int event_kind(const std::wstring &t)
{
    auto has = [&](const wchar_t *w) { return t.find(w) != std::wstring::npos; };
    if (has(L"Impossible") || has(L"non prise en charge") || has(L"autre version") || has(L"planté") || has(L"pas chargé") ||
        has(L"ne démarre pas") || has(L"ne répond pas") || has(L"échoué"))
        return 3;
    if (has(L"déconnecté") || has(L"perdue") || has(L"pas maintenant") || has(L"en attente") || has(L"Aucun") ||
        has(L"introuvable") || has(L"s'est fermé"))
        return 2;
    if (has(L"connecté") || has(L"Connecté") || has(L"trouvé") || has(L"reçu") || has(L"copiées") || has(L"installé") ||
        has(L"est arrivé") || has(L"De retour") || has(L"revenu") || has(L"ajoutée") || has(L"enregistrée") || has(L"copié"))
        return 1;
    for (int kind = 3; kind >= 1; --kind)
        for (const TrKeywords &k : kTrKeywords)
            if (k.lang == g_lang && k.kind == kind)
                for (const wchar_t *const *w = k.words; *w; ++w)
                    if (has(*w))
                        return kind;
    return 0;
}

void layout_events();

void add_event(const std::wstring &text)
{
    wchar_t stamp[16];
    SYSTEMTIME t;
    GetLocalTime(&t);
    swprintf(stamp, 16, L"%02d:%02d:%02d  ", t.wHour, t.wMinute, t.wSecond);
    int i = int(SendMessageW(g_events, LB_ADDSTRING, 0, LPARAM((stamp + text).c_str())));
    SendMessageW(g_events, LB_SETITEMDATA, i, LPARAM(event_kind(text)));
    SendMessageW(g_events, LB_SETTOPINDEX, i, 0);
    if (g_page == PAGE_LOG) {
        ShowWindow(g_events, SW_SHOW);
        InvalidateRect(g_wnd, nullptr, FALSE);
    }
    if (event_kind(text) >= 2 && g_page != PAGE_LOG) {
        g_log_unseen = true;
        if (HWND nav = GetDlgItem(g_wnd, ID_NAV + PAGE_LOG))
            InvalidateRect(nav, nullptr, FALSE);
    }
    layout_events();
}

void draw_backup(const DRAWITEMSTRUCT *d)
{
    if (d->itemID == UINT(-1))
        return;
    bool sel = d->itemState & ODS_SELECTED;
    RECT r = d->rcItem;
    FillRect(d->hDC, &r, g_card_brush);
    RECT row = r;
    InflateRect(&row, -px(4), -px(2));
    if (sel)
        theme::round_rect(d->hDC, row, px(theme::kRadiusSmall), theme::kSurfaceHover, theme::kAccent);
    wchar_t text[128] = L"";
    SendMessageW(d->hwndItem, LB_GETTEXT, d->itemID, LPARAM(text));
    SetBkMode(d->hDC, TRANSPARENT);
    RECT t = row;
    t.left += px(12);
    SetTextColor(d->hDC, kMuted);
    HGDIOBJ old = SelectObject(d->hDC, g_small_icon);
    DrawTextW(d->hDC, L"\uE74E", -1, &t, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    SelectObject(d->hDC, g_font);
    t.left += px(26);
    SetTextColor(d->hDC, kText);
    DrawTextW(d->hDC, text, -1, &t, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    if (d->itemID == 0) {
        SetTextColor(d->hDC, kMuted);
        RECT n = row;
        n.right -= px(12);
        DrawTextW(d->hDC, tr(L"la plus récente"), -1, &n, DT_RIGHT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    }
    SelectObject(d->hDC, old);
}

void draw_event(const DRAWITEMSTRUCT *d)
{
    if (d->itemID == UINT(-1))
        return;
    static const COLORREF colors[4] = {kInfo, kOk, kWarn, kBad};
    static const wchar_t *icons[4] = {L"\u25CF", L"\u2714", L"\u26A0", L"\u2716"};
    int kind = int(d->itemData) & 3;
    wchar_t text[600] = L"";
    SendMessageW(d->hwndItem, LB_GETTEXT, d->itemID, LPARAM(text));
    FillRect(d->hDC, &d->rcItem, g_card_brush);
    SetBkMode(d->hDC, TRANSPARENT);
    RECT r = d->rcItem;
    r.left += px(4);
    std::wstring all = text;
    std::wstring stamp = all.substr(0, all.size() >= 10 ? 10 : all.size()), rest = all.size() > 10 ? all.substr(10) : L"";
    SetTextColor(d->hDC, kMuted);
    DrawTextW(d->hDC, stamp.c_str(), -1, &r, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    SIZE sz = {};
    GetTextExtentPoint32W(d->hDC, stamp.c_str(), int(stamp.size()), &sz);
    r.left += sz.cx;
    SetTextColor(d->hDC, colors[kind]);
    DrawTextW(d->hDC, icons[kind], -1, &r, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    r.left += px(18);
    SetTextColor(d->hDC, kind == 3 ? kBad : kind == 2 ? kWarn : kText);
    DrawTextW(d->hDC, rest.c_str(), -1, &r, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
}

bool guest_role() { return g_role == ROLE_GUEST; }
bool solo_role() { return g_role == ROLE_SOLO; }
bool lan_mode() { return g_gog || switch_on(g_lan); }

bool parse_address(const std::wstring &text, std::string &ip, int &port)
{
    std::wstring t;
    for (wchar_t c : text)
        if (c != L' ')
            t += c;
    port = kLanPort;
    size_t colon = t.find(L':');
    if (colon != std::wstring::npos) {
        std::wstring p = t.substr(colon + 1);
        if (p.empty() || p.size() > 5 || p.find_first_not_of(L"0123456789") != std::wstring::npos)
            return false;
        port = _wtoi(p.c_str());
        if (port < 1 || port > 65535)
            return false;
        t = t.substr(0, colon);
    }
    if (t.empty() || t.find_first_not_of(L"0123456789.") != std::wstring::npos)
        return false;
    in_addr a;
    std::string n(t.begin(), t.end());
    if (inet_pton(AF_INET, n.c_str(), &a) != 1)
        return false;
    ip = n;
    return true;
}

std::vector<std::wstring> local_addresses()
{
    std::vector<std::wstring> out, later;
    ULONG size = 16 * 1024;
    std::vector<unsigned char> buf(size);
    auto *a = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buf.data());
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_INCLUDE_GATEWAYS;
    if (GetAdaptersAddresses(AF_INET, flags, nullptr, a, &size) == ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        a = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buf.data());
    }
    if (GetAdaptersAddresses(AF_INET, flags, nullptr, a, &size) != NO_ERROR)
        return out;
    for (; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
            continue;
        std::wstring card = std::wstring(a->Description ? a->Description : L"") + L" " + (a->FriendlyName ? a->FriendlyName : L"");
        if (card.find(L"VirtualBox") != std::wstring::npos || card.find(L"VMware") != std::wstring::npos ||
            card.find(L"Hyper-V") != std::wstring::npos || card.find(L"vEthernet") != std::wstring::npos)
            continue;
        for (auto *u = a->FirstUnicastAddress; u; u = u->Next) {
            wchar_t txt[64] = {};
            auto *sin = reinterpret_cast<sockaddr_in *>(u->Address.lpSockaddr);
            if (InetNtopW(AF_INET, &sin->sin_addr, txt, 64) && wcsncmp(txt, L"169.254.", 8) != 0)
                (a->FirstGatewayAddress ? out : later).push_back(txt);
        }
    }
    out.insert(out.end(), later.begin(), later.end());
    return out;
}

void update_controls()
{
    if (g_copy_code) {
        bool salon = g_page == PAGE_SALON;
        ShowWindow(g_copy_code, salon && g_role == ROLE_HOST ? SW_SHOW : SW_HIDE);
        EnableWindow(g_copy_code, lan_mode() || g_self_steam != 0);
        SetWindowTextW(g_copy_code, lan_mode() ? tr(L"&Copier l'adresse") : tr(L"&Copier le code"));
        ShowWindow(g_code_edit, salon && g_role == ROLE_GUEST ? SW_SHOW : SW_HIDE);
        ShowWindow(g_code_clear, salon && g_role == ROLE_GUEST ? SW_SHOW : SW_HIDE);
    }
    ShowWindow(g_own_char, g_page == PAGE_PLAY && guest_role() ? SW_SHOW : SW_HIDE);
    ShowWindow(g_keep, g_page == PAGE_PLAY && guest_role() ? SW_SHOW : SW_HIDE);
    ShowWindow(g_update, g_page == PAGE_PLAY && !g_update_tag.empty() ? SW_SHOW : SW_HIDE);
    EnableWindow(g_launch, g_supported);
    if (g_lan) {
        EnableWindow(g_lan, !g_gog);
        for (Switch &sw : g_switches)
            if (sw.h == g_lan) {
                SetWindowTextW(g_lan, (sw.label + (sw.on || g_gog ? tr(L" : activé") : tr(L" : désactivé"))).c_str());
                InvalidateRect(g_lan, nullptr, FALSE);
            }
    }
    if (g_code_edit)
        SendMessageW(g_code_edit, EM_SETCUEBANNER, TRUE, LPARAM(lan_mode() ? L"192.168.1.20" : L"ABC-DEFG"));
}

void check_game()
{
    g_game_dir = window_text(g_game_edit);
    g_supported = false;
    g_game_state = 0;
    if (g_game_dir.empty() || !exists(g_game_dir + L"\\DDDA.exe")) {
        SetWindowTextW(g_version, tr(L"DDDA.exe introuvable dans ce dossier."));
    } else {
        SetWindowTextW(g_version, tr(L"Vérification de DDDA.exe…"));
        UpdateWindow(g_version);
        std::string sha = sha256_file(g_game_dir + L"\\DDDA.exe");
        g_gog = sha == kGogSha;
        g_supported = sha == kSupportedSha || g_gog;
        SetWindowTextW(g_version, g_gog         ? tr(L"Version GOG du jeu prise en charge (réseau local ou IP directe, sans Steam).")
                                  : g_supported ? tr(L"Version du jeu prise en charge.")
                                                : tr(L"Version de DDDA.exe non prise en charge : le mod ne marcherait pas."));
        g_game_state = g_supported ? 2 : 1;
        if (g_supported)
            g_store = g_gog ? 1 : 0;
        if (!g_supported && g_wnd && g_bad_told_for != g_game_dir) {
            g_bad_told_for = g_game_dir;
            g_bad_sha = sha;
            PostMessageW(g_wnd, WM_COMMAND, ID_UNSUPPORTED_HELP, 0);
        }
        if (g_gog && g_wnd && setting(L"gog_help_seen", L"0") != L"1") {
            WritePrivateProfileStringW(L"launcher", L"gog_help_seen", L"1", settings_path().c_str());
            PostMessageW(g_wnd, WM_COMMAND, ID_GOG_HELP, 0);
        }
    }
    if (g_store_steam) {
        SetWindowTextW(g_store_steam, (std::wstring(L"Steam") + (g_store == 0 ? tr(L", choisi") : L"")).c_str());
        SetWindowTextW(g_store_gog, (std::wstring(L"GOG") + (g_store == 1 ? tr(L", choisi") : L"")).c_str());
        InvalidateRect(g_store_steam, nullptr, FALSE);
        InvalidateRect(g_store_gog, nullptr, FALSE);
    }
    update_controls();
    if (g_wnd)
        InvalidateRect(g_wnd, nullptr, FALSE);
}

bool copy_tree(const std::wstring &from, const std::wstring &to)
{
    if (SHCreateDirectoryExW(nullptr, to.c_str(), nullptr) != ERROR_SUCCESS && !is_dir(to))
        return false;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((from + L"\\*").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE)
        return true;
    bool ok = true;
    do {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..")
            continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            ok = copy_tree(from + L"\\" + n, to + L"\\" + n) && ok;
        else
            ok = CopyFileW((from + L"\\" + n).c_str(), (to + L"\\" + n).c_str(), TRUE) && ok;
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    return ok;
}

std::wstring documents_dir()
{
    if (!g_docs_dir.empty())
        return g_docs_dir;
    PWSTR p = nullptr;
    std::wstring s;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &p)))
        s = p;
    CoTaskMemFree(p);
    return s;
}

std::wstring gog_storage_dir()
{
    if (!g_gog_storage_arg.empty())
        return g_gog_storage_arg;
    wchar_t buf[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH))
        return {};
    return std::wstring(buf) + L"\\GOG.com\\Galaxy\\Applications\\49987265717041704\\Storage";
}

std::wstring backup_saves(int *accounts)
{
    *accounts = 0;
    if (g_gog) {
        std::wstring src = gog_storage_dir();
        if (src.empty() || !is_dir(src))
            return {};
        wchar_t stamp[32];
        SYSTEMTIME t;
        GetLocalTime(&t);
        swprintf(stamp, 32, L"%04d-%02d-%02d_%02d-%02d-%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        std::wstring dest = documents_dir() + L"\\Arisen Together\\Backups\\" + stamp;
        for (int n = 2; exists(dest); ++n)
            dest = documents_dir() + L"\\Arisen Together\\Backups\\" + stamp + L"_" + std::to_wstring(n);
        if (!copy_tree(src, dest + L"\\GOG"))
            return {};
        *accounts = 1;
        return dest;
    }
    if (g_steam_dir.empty())
        return {};
    wchar_t stamp[32];
    SYSTEMTIME t;
    GetLocalTime(&t);
    swprintf(stamp, 32, L"%04d-%02d-%02d_%02d-%02d-%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    std::wstring dest = documents_dir() + L"\\Arisen Together\\Backups\\" + stamp;
    for (int n = 2; exists(dest); ++n)
        dest = documents_dir() + L"\\Arisen Together\\Backups\\" + stamp + L"_" + std::to_wstring(n);
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((g_steam_dir + L"\\userdata\\*").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE)
        return {};
    bool ok = true;
    do {
        std::wstring id = fd.cFileName;
        std::wstring remote = g_steam_dir + L"\\userdata\\" + id + L"\\367500\\remote";
        if (id == L"." || id == L".." || !is_dir(remote))
            continue;
        ok = copy_tree(remote, dest + L"\\" + id) && ok;
        ++*accounts;
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    return ok && *accounts ? dest : std::wstring();
}

bool our_dll(const std::wstring &p)
{
    std::string data;
    return read_file(p, data) && (data.find(kOurMarker) != std::string::npos || data.find(kOldMarker) != std::string::npos);
}

bool write_coop_ini()
{
    std::string base, old;
    bool have_pkg = read_file(g_exe_dir + L"\\coop.ini", base);
    bool have_old = read_file(g_game_dir + L"\\coop.ini", old);
    if (have_pkg && have_old) {
        auto key_of = [](const std::string &ln) {
            size_t eq = ln.find('=');
            return eq == std::string::npos || ln.empty() || ln[0] == '#' || ln[0] == ';' ? std::string() : ln.substr(0, eq);
        };
        auto each_line = [](const std::string &text, const std::function<void(const std::string &)> &fn) {
            size_t a = 0;
            while (a < text.size()) {
                size_t b = text.find('\n', a);
                std::string ln = text.substr(a, b == std::string::npos ? std::string::npos : b - a);
                if (!ln.empty() && ln.back() == '\r')
                    ln.pop_back();
                fn(ln);
                a = b == std::string::npos ? text.size() : b + 1;
            }
        };
        std::vector<std::string> named;
        each_line(base, [&](const std::string &ln) {
            if (!key_of(ln).empty())
                named.push_back(key_of(ln));
        });
        const char *test_only[] = {"block_saves", "bridge", "multi_instance", "keep_active", "private_appdata", "window",
                                   "steam_mock", "port", "peer", "dump_writes"};
        std::string kept;
        each_line(old, [&](const std::string &ln) {
            std::string k = key_of(ln);
            if (k.empty() || std::find(named.begin(), named.end(), k) != named.end())
                return;
            if (k.compare(0, 5, "test_") == 0 || k.compare(0, 5, "mock_") == 0)
                return;
            for (const char *t : test_only)
                if (k == t)
                    return;
            kept += ln + "\n";
        });
        if (!base.empty() && base.back() != '\n')
            base += "\n";
        if (!kept.empty())
            base += "# Kept from this folder's earlier coop.ini\n" + kept;
    } else if (!have_pkg && have_old) {
        base = old;
    } else if (!have_pkg) {
        base = "p2_pad=1\nidle_pad=3\nauto_possess=0\nblock_saves=0\nguest_cam_pawn=1\n";
    }
    const char *ours[] = {"role=", "#role=", "transport=", "debug_log=", "discord=", "send_character=", "keep_progress=", "peer_steamid=", "peer=", "port=",
                          "# Lines below are set by ArisenTogether.exe"};
    std::string out, line;
    for (size_t i = 0; i <= base.size(); ++i) {
        if (i < base.size() && base[i] != '\n') {
            line += base[i];
            continue;
        }
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        bool drop = false;
        for (const char *k : ours)
            drop = drop || line.compare(0, strlen(k), k) == 0;
        if (!drop && !(line.empty() && i == base.size()))
            out += line + "\n";
        line.clear();
    }
    bool guest = guest_role();
    out += "# Lines below are set by ArisenTogether.exe\n";
    if (!solo_role()) {
        out += guest ? "role=guest\n" : "role=host\n";
        if (lan_mode()) {
            std::string ip;
            int port = kLanPort;
            out += "transport=udp\n";
            if (guest && g_code_edit && parse_address(window_text(g_code_edit), ip, port))
                out += "peer=" + ip + "\n";
            out += "port=" + std::to_string(port) + "\n";
        } else {
            out += "transport=steam\n";
        }
    }
    out += switch_on(g_keep) ? "keep_progress=1\n" : "keep_progress=0\n";
    out += switch_on(g_debug) ? "debug_log=1\n" : "debug_log=0\n";
    if (guest && !switch_on(g_own_char))
        out += "send_character=0\n";
    uint64_t host_id = 0;
    if (guest && !lan_mode() && g_code_edit && steam_from_code(window_text(g_code_edit), host_id))
        out += "peer_steamid=" + std::to_string(host_id) + "\n";
    return write_file(g_game_dir + L"\\coop.ini", out);
}

bool install_dll()
{
    std::wstring src = g_exe_dir + L"\\dinput8.dll", dst = g_game_dir + L"\\dinput8.dll";
    if (!exists(src)) {
        ask(tr(L"dinput8.dll manque à côté de ArisenTogether.exe : garde les deux fichiers dans le même dossier."), MB_ICONERROR);
        return false;
    }
    wchar_t fs[MAX_PATH] = L"", fd[MAX_PATH] = L"";
    if (GetFullPathNameW(src.c_str(), MAX_PATH, fs, nullptr) && GetFullPathNameW(dst.c_str(), MAX_PATH, fd, nullptr) &&
        _wcsicmp(fs, fd) == 0)
        return true;
    if (exists(dst) && !our_dll(dst)) {
        ask(tr(L"Le dossier du jeu contient déjà un autre dinput8.dll (un autre mod ?).\n\nDéplace-le ailleurs toi-même, puis réessaie. Arisen Together ne le remplace pas."),
            MB_ICONWARNING);
        return false;
    }
    if (!CopyFileW(src.c_str(), dst.c_str(), FALSE)) {
        DWORD err = GetLastError();
        ask((std::wstring(tr(L"Impossible de copier dinput8.dll dans le dossier du jeu.")) + L" (Windows: " + std::to_wstring(err) + L")").c_str(),
            MB_ICONERROR);
        return false;
    }
    return true;
}

void show_backup_note()
{
    std::wstring when = setting(L"last_backup_time", L""), where = setting(L"last_backup", L"");
    std::wstring text = when.empty() ? tr(L"Copie de secours automatique de tes sauvegardes à chaque lancement (aucune encore).")
                                     : tr(L"Dernière copie de secours : ") + when + L" \u2192 " + where;
    if (g_backup_note)
        SetWindowTextW(g_backup_note, text.c_str());
}

void refresh_backups()
{
    if (!g_backup_list)
        return;
    std::wstring root = documents_dir() + L"\\Arisen Together\\Backups";
    std::vector<std::wstring> names;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((root + L"\\*").c_str(), &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != L'.')
                names.push_back(fd.cFileName);
        while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    std::sort(names.rbegin(), names.rend());
    g_backup_dirs.clear();
    SendMessageW(g_backup_list, LB_RESETCONTENT, 0, 0);
    for (const std::wstring &n : names) {
        int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0;
        std::wstring shown = n;
        if (swscanf(n.c_str(), L"%d-%d-%d_%d-%d-%d", &y, &mo, &d, &h, &mi, &se) == 6) {
            wchar_t t[64];
            swprintf(t, 64, L"%02d/%02d/%04d  %02d:%02d:%02d", d, mo, y, h, mi, se);
            shown = t;
        }
        g_backup_dirs.push_back(root + L"\\" + n);
        SendMessageW(g_backup_list, LB_ADDSTRING, 0, LPARAM(shown.c_str()));
    }
    if (!names.empty())
        SendMessageW(g_backup_list, LB_SETCURSEL, 0, 0);
    if (g_page == PAGE_SAVES) {
        ShowWindow(g_backup_list, names.empty() ? SW_HIDE : SW_SHOW);
        InvalidateRect(g_wnd, nullptr, FALSE);
    }
}

void note_backup(const std::wstring &where)
{
    wchar_t when[32];
    SYSTEMTIME t;
    GetLocalTime(&t);
    swprintf(when, 32, L"%02d/%02d %02d:%02d", t.wDay, t.wMonth, t.wHour, t.wMinute);
    WritePrivateProfileStringW(L"launcher", L"last_backup_time", when, settings_path().c_str());
    WritePrivateProfileStringW(L"launcher", L"last_backup", where.c_str(), settings_path().c_str());
    show_backup_note();
    refresh_backups();
}

void show_help()
{
    MessageBoxW(g_wnd,
                tr(L"Dans le jeu\n  \u2022 Invité : attends « Connecté ! » dans cette fenêtre avant d'appuyer sur Start.\n  \u2022 Explorer seul / revenir : LB + RB + Y maintenus 1,5 s (ou F7).\n  \u2022 Relever un joueur à terre : maintiens B à côté de lui (3 s).\n  \u2022 Saisir un joueur ou un petit monstre : RT.\n\nIci\n  \u2022 Une copie de secours de tes sauvegardes est faite à chaque lancement ;\n    « Copier mes sauvegardes » en fait une tout de suite et ouvre le dossier.\n  \u2022 En cas de souci : « Exporter le journal » et envoie le fichier.\n  \u2022 Clavier : Alt + la lettre soulignée, Tab pour passer d'un élément à l'autre."),
                tr(L"Arisen Together \u2013 Aide"), MB_ICONINFORMATION | MB_OK);
}

void show_gog_help()
{
    MessageBoxW(g_wnd,
                tr(L"Sans Steam, on se rejoint par adresse IP (port UDP 27101). \u00ab\u00a0Jouer sans Steam\u00a0\u00bb reste donc activ\u00e9.\n\n1. L'h\u00f4te lance le jeu. Le Salon affiche ses adresses : \u00ab\u00a0Copier l'adresse\u00a0\u00bb et envoie-la \u00e0 ton ami.\n2. L'invit\u00e9 choisit \u00ab\u00a0Invit\u00e9\u00a0\u00bb, colle cette adresse dans le Salon (par exemple 192.168.1.20), puis lance le jeu.\n3. Sur le m\u00eame r\u00e9seau (m\u00eame box) : rien d'autre \u00e0 faire.\n    Par internet : l'h\u00f4te ouvre le port UDP 27101 sur sa box (redirection vers son PC), ou vous utilisez tous les deux un VPN de type r\u00e9seau local et l'adresse qu'il donne.\n\n\u00c0 savoir\n  \u2022 Garde GOG Galaxy ouvert et connect\u00e9 : sans lui, le jeu peut rester bloqu\u00e9 sur \u00ab\u00a0Accessing server...\u00a0\u00bb.\n  \u2022 En chargeant ta partie, le jeu GOG peut afficher \u00ab\u00a0\u00c9chec de la connexion\u00a0\u00bb (le serveur des pions de GOG) : appuie sur A ou Entr\u00e9e, la partie se charge.\n  \u2022 Tes sauvegardes GOG Galaxy sont copi\u00e9es \u00e0 chaque lancement dans Documents\\Arisen Together\\Backups.\n  \u2022 Un ami sur Steam peut te rejoindre s'il active \u00ab\u00a0Jouer sans Steam\u00a0\u00bb dans ses Param\u00e8tres."),
                tr(L"Arisen Together \u2013 La version GOG"), MB_ICONINFORMATION | MB_OK);
}

void show_unsupported_help()
{
    std::wstring sha(g_bad_sha.begin(), g_bad_sha.end());
    MessageBoxW(g_wnd,
                (tr(L"Ce DDDA.exe n'est pas une version prise en charge : le mod ne marcherait pas.\n\nArisen Together marche avec :\n  \u2022 Steam : la version actuelle (build 2364871). Dans Steam : clic droit sur le jeu > Propriétés > Fichiers installés > Vérifier l'intégrité des fichiers.\n  \u2022 GOG : la version actuelle (GOG Galaxy ou l'installeur GOG).\nUn exe modifié (patch, crack, version d'un autre magasin) n'est pas reconnu : remets celui d'origine.\n\nEmpreinte de ton DDDA.exe (à joindre si tu signales le problème) :\n") +
                 sha)
                    .c_str(),
                tr(L"Arisen Together \u2013 Version du jeu"), MB_ICONWARNING | MB_OK);
}

void on_launch()
{
    check_game();
    if (!g_supported)
        return;
    if (game_running()) {
        ask(tr(L"Le jeu est déjà lancé. Quitte-le d'abord : les réglages ne s'appliquent qu'au démarrage."), MB_ICONINFORMATION);
        return;
    }
    if (guest_role() && lan_mode()) {
        std::string ip;
        int port = 0;
        if (!g_code_edit || !parse_address(window_text(g_code_edit), ip, port)) {
            ask(tr(L"Entre d'abord l'adresse de ton hôte dans le Salon (par exemple 192.168.1.20)."), MB_ICONINFORMATION);
            return;
        }
    }
    int accounts = 0;
    std::wstring backup = backup_saves(&accounts);
    if (backup.empty() &&
        ask(tr(L"La copie de secours de tes sauvegardes n'a pas marché (aucune trouvée, ou erreur).\n\nLancer quand même ?"),
            MB_ICONWARNING | MB_YESNO) != IDYES)
        return;
    if (!backup.empty()) {
        add_event(tr(L"Sauvegardes copiées dans ") + backup);
        note_backup(backup);
    }
    if (!install_dll())
        return;
    if (!write_coop_ini()) {
        ask(tr(L"Impossible d'écrire coop.ini dans le dossier du jeu."), MB_ICONERROR);
        return;
    }
    save_settings();
    bool guest = guest_role();
    add_event(guest ? tr(L"Mod installé en invité.") : solo_role() ? tr(L"Mod installé pour jouer seul.") : tr(L"Mod installé en hôte."));
    if (solo_role() && exists(g_game_dir + L"\\coop_progress\\pending.txt"))
        add_event(tr(L"Ta progression co-op sera ajoutée à ta partie quand tu la chargeras."));

    GetSystemTimeAsFileTime(&g_launch_time);
    g_launch_tick = GetTickCount();
    g_steam_wait_since = 0;
    g_mod_loaded = g_link_up = g_host_found = g_game_seen = false;
    g_told_no_mod = g_told_steam = g_told_no_host = g_told_closed = false;
    g_log_pos = 0;
    g_watching = true;
    g_friends = 0;
    g_world_received = false;
    if (g_gog) {
        if (!g_no_launch)
            ShellExecuteW(g_wnd, L"open", (g_game_dir + L"\\DDDA.exe").c_str(), nullptr, g_game_dir.c_str(), SW_SHOWNORMAL);
        add_event(tr(L"Lancement du jeu (GOG)…"));
    } else {
        if (!g_no_launch)
            ShellExecuteW(g_wnd, L"open", L"steam://rungameid/367500", nullptr, nullptr, SW_SHOWNORMAL);
        add_event(tr(L"Lancement du jeu par Steam…"));
    }
    set_state(guest ? tr(L"Lancement… Reste sur l'écran titre jusqu'à ce que l'hôte soit trouvé.")
                    : tr(L"Lancement… Charge ta partie normalement."),
              kMuted);
}

void on_remove()
{
    check_game();
    if (g_game_dir.empty())
        return;
    if (game_running()) {
        ask(tr(L"Quitte le jeu d'abord."), MB_ICONINFORMATION);
        return;
    }
    std::wstring dll = g_game_dir + L"\\dinput8.dll", ini = g_game_dir + L"\\coop.ini";
    if (!exists(dll) && !exists(ini)) {
        ask(tr(L"Le mod n'est pas installé dans ce dossier."), MB_ICONINFORMATION);
        return;
    }
    if (ask((tr(L"Retirer Arisen Together de ce dossier ?\n\n") + g_game_dir +
                tr(L"\n\nLe jeu redémarrera en solo normal. Tes sauvegardes ne sont pas touchées ; tu pourras réinstaller le mod en relançant depuis cette fenêtre."))
                .c_str(),
            MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) != IDYES)
        return;
    if (exists(dll) && !our_dll(dll)) {
        ask(tr(L"Le dinput8.dll du dossier du jeu n'est pas celui de Arisen Together : je n'y touche pas."), MB_ICONWARNING);
        return;
    }
    if (exists(g_game_dir + L"\\coop_progress\\pending.txt") &&
        ask(tr(L"Ta progression gagnée en co-op n'est pas encore dans ta partie.\n\nPour l'ajouter : choisis « Seul », lance le jeu, charge ta partie puis sauvegarde. Si tu retires le mod maintenant, elle reste en attente pour plus tard.\n\nRetirer le mod quand même ?"),
            MB_ICONQUESTION | MB_YESNO) != IDYES)
        return;
    bool ok = (!exists(dll) || DeleteFileW(dll.c_str())) && (!exists(ini) || DeleteFileW(ini.c_str()));
    g_watching = false;
    add_event(ok ? tr(L"Mod retiré : le jeu redémarre en solo normal.") : tr(L"Impossible de supprimer dinput8.dll ou coop.ini."));
    set_state(ok ? tr(L"Mod retiré.") : tr(L"Erreur en retirant le mod."), ok ? kMuted : kBad);
}

void on_backup()
{
    int accounts = 0;
    std::wstring dest = backup_saves(&accounts);
    if (dest.empty()) {
        ask(tr(L"Aucune sauvegarde de Dragon's Dogma trouvée dans Steam, ou la copie a échoué."), MB_ICONWARNING);
        return;
    }
    add_event(tr(L"Sauvegardes copiées dans ") + dest);
    note_backup(dest);
    ShellExecuteW(g_wnd, L"open", dest.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void on_export()
{
    std::wstring log = g_game_dir + L"\\coop.log";
    if (!exists(log)) {
        ask(tr(L"Pas encore de coop.log dans le dossier du jeu : lance le jeu avec le mod d'abord."), MB_ICONINFORMATION);
        return;
    }
    wchar_t stamp[32];
    SYSTEMTIME t;
    GetLocalTime(&t);
    swprintf(stamp, 32, L"%04d-%02d-%02d_%02d-%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute);
    std::wstring dir = documents_dir() + L"\\Arisen Together\\Logs";
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    std::wstring dest = dir + L"\\coop-" + (guest_role() ? L"invite-" : L"hote-") + stamp + L".log";
    if (!CopyFileW(log.c_str(), dest.c_str(), FALSE)) {
        ask(tr(L"La copie du journal a échoué."), MB_ICONERROR);
        return;
    }
    std::wstring prev = g_game_dir + L"\\coop.prev.log";
    if (exists(prev))
        CopyFileW(prev.c_str(), (dest.substr(0, dest.size() - 4) + L"-previous.log").c_str(), FALSE);
    add_event(tr(L"Journal copié : ") + dest);
    std::wstring arg = L"/select,\"" + dest + L"\"";
    ShellExecuteW(g_wnd, L"open", L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
}

void on_browse()
{
    BROWSEINFOW bi = {};
    bi.hwndOwner = g_wnd;
    bi.lpszTitle = tr(L"Choisis le dossier qui contient DDDA.exe");
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    if (PIDLIST_ABSOLUTE id = SHBrowseForFolderW(&bi)) {
        wchar_t path[MAX_PATH];
        if (SHGetPathFromIDListW(id, path)) {
            SetWindowTextW(g_game_edit, path);
            check_game();
            save_settings();
        }
        CoTaskMemFree(id);
    }
}

void pick_store(int store)
{
    g_store = store;
    std::wstring before = g_game_dir;
    const std::wstring &dir = store == 1 ? g_gog_game : g_steam_game;
    if (dir.empty()) {
        ask(store == 1 ? tr(L"La version GOG du jeu n'a pas été trouvée sur ce PC. Choisis le dossier qui contient son DDDA.exe.")
                       : tr(L"La version Steam du jeu n'a pas été trouvée sur ce PC. Choisis le dossier qui contient son DDDA.exe."),
            MB_ICONINFORMATION);
        on_browse();
    } else {
        SetWindowTextW(g_game_edit, dir.c_str());
        check_game();
        save_settings();
    }
    check_game();
    if (g_supported && g_game_dir != before && (store == 1) != g_gog)
        ask(store == 1 ? tr(L"Ce dossier contient la version Steam du jeu, pas celle de GOG.")
                       : tr(L"Ce dossier contient la version GOG du jeu, pas celle de Steam."),
            MB_ICONWARNING);
}

int number_after(const std::string &line, const char *word)
{
    size_t at = line.find(word);
    return at == std::string::npos ? 0 : atoi(line.c_str() + at + strlen(word));
}

bool g_tutorial_wait_told = false;

void on_log_line(const std::string &l)
{
    bool guest = guest_role();
    auto has = [&](const char *s) { return l.find(s) != std::string::npos; };
    if (has(kOurMarker))
        g_mod_loaded = true;
    if (has("steam: link up"))
        g_link_up = true;
    if (has("steam: host found"))
        g_host_found = true;
    if (has("waiting for Steam") && !g_steam_wait_since)
        g_steam_wait_since = GetTickCount() | 1;
    if (has("a player has an older version of the mod")) {
        add_event(tr(L"Un joueur a une autre version du mod : vous ne vous voyez pas. Prenez tous le même paquet."));
        set_state(tr(L"Un joueur a une autre version du mod."), kBad);
    } else if (has("unsupported DDDA.exe")) {
        add_event(tr(L"Version du jeu non prise en charge : le mod s'est désactivé."));
        set_state(tr(L"Mod désactivé : version du jeu non prise en charge."), kBad);
    } else if (has("net: bind failed")) {
        add_event(tr(L"Le port UDP de la co-op est déjà pris sur ce PC (un autre jeu lancé en hôte ?) : ferme-le, puis relance le jeu."));
        set_state(tr(L"Port UDP déjà utilisé."), kBad);
    } else if (has("net: host, listening on port")) {
        g_link_up = true;
        add_event(tr(L"Prêt en IP directe, port UDP ") + std::to_wstring(number_after(l, "listening on port ")) + L".");
        set_state(tr(L"Prêt : en attente de tes amis. Tu peux jouer."), kMuted);
    } else if (has("net: guest, ") && has(" port ")) {
        g_link_up = true;
        std::string ip = l.substr(l.find("net: guest, ") + 12);
        ip = ip.substr(0, ip.find(' '));
        add_event(tr(L"Connexion à l'hôte en IP directe : ") + std::wstring(ip.begin(), ip.end()));
        set_state(tr(L"Recherche de l'hôte à son adresse… Reste sur l'écran titre."), kMuted);
    } else if (has("net: save: receiving the host's save") && !g_host_found) {
        g_host_found = true;
        add_event(tr(L"Hôte trouvé, réception de son monde…"));
        set_state(tr(L"Hôte trouvé. Réception de son monde… Reste sur l'écran titre."), kMuted);
    } else if (has("waiting for Steam")) {
        set_state(tr(L"En attente de Steam…"), kMuted);
    } else if (has("steam: link up")) {
        add_event(tr(L"Connecté à Steam."));
        set_state(guest ? tr(L"Recherche de l'hôte parmi tes amis Steam… Reste sur l'écran titre.")
                        : tr(L"Prêt : en attente de tes amis. Tu peux jouer."),
                  kMuted);
    } else if (has("steam: host found")) {
        add_event(tr(L"Hôte trouvé, réception de son monde…"));
        set_state(tr(L"Hôte trouvé. Réception de son monde… Reste sur l'écran titre."), kMuted);
    } else if (has("before the host's save arrived: waiting for it")) {
        add_event(tr(L"Start appuyé trop tôt : le jeu attend le monde de son hôte (écran figé, 20 s au plus)…"));
        set_state(tr(L"Start appuyé trop tôt : le jeu attend le monde de son hôte (écran figé, 20 s au plus)…"), kWarn);
    } else if (has("storage: the host's save arrived after")) {
        add_event(tr(L"Le monde de son hôte est arrivé : le chargement continue."));
    } else if (has("storage: the host's save did NOT arrive")) {
        add_event(tr(L"Pas de monde reçu en 20 s : tu es dans ta propre partie. Quitte le jeu et relance en invité."));
        set_state(tr(L"Pas de monde reçu en 20 s : tu es dans ta propre partie. Quitte le jeu et relance en invité."), kBad);
    } else if (has("net: save: the host's save version") && has("arrived")) {
        if (!g_world_received)
            set_state(tr(L"Connecté ! Appuie sur Start, puis choisis « Charger »."), kOk);
        add_event(g_world_received ? tr(L"Monde de l'hôte mis à jour.") : tr(L"Monde de l'hôte reçu."));
        g_world_received = true;
    } else if (has("it does not join; play the tutorial alone first")) {
        add_event(tr(L"Impossible de rejoindre : ton perso n'a pas encore fini le tutoriel. Joue-le d'abord seul (jusqu'à ton pion principal), puis relance en invité."));
        set_state(tr(L"Impossible de rejoindre : ton perso n'a pas encore fini le tutoriel. Joue-le d'abord seul (jusqu'à ton pion principal), puis relance en invité."), kBad);
    } else if (has("net: a guest waits: this game is before the end of the tutorial")) {
        if (!g_tutorial_wait_told) {
            g_tutorial_wait_told = true;
            add_event(tr(L"Un ami attend : la co-op démarre quand tu auras fini le tutoriel (ton pion principal créé)."));
        }
        set_state(tr(L"Un ami attend : la co-op démarre quand tu auras fini le tutoriel (ton pion principal créé)."), kWarn);
    } else if (has("past the tutorial (an Arisen and a main pawn): guests may join") && g_tutorial_wait_told) {
        add_event(tr(L"Tutoriel terminé : tes amis peuvent te rejoindre."));
        set_state(tr(L"Tutoriel terminé : tes amis peuvent te rejoindre."), kOk);
    } else if (has("net: guest connected from")) {
        int k = number_after(l, "(guest ");
        ++g_friends;
        add_event(tr(L"Ami ") + std::to_wstring(k) + tr(L" connecté."));
        set_state(std::to_wstring(g_friends) + (plural(g_friends) ? tr(L" amis connectés.") : tr(L" ami connecté.")), kOk);
    } else if (has("net: guest ") && has(", dropped")) {
        int k = number_after(l, "net: guest ");
        g_friends = g_friends > 0 ? g_friends - 1 : 0;
        add_event(tr(L"Ami ") + std::to_wstring(k) + tr(L" déconnecté (plus de nouvelles depuis 10 s). Il peut revenir : il relance le jeu et charge, comme au début."));
        set_state(g_friends ? std::to_wstring(g_friends) + (plural(g_friends) ? tr(L" amis connectés.") : tr(L" ami connecté."))
                            : tr(L"En attente de tes amis. Tu peux jouer."),
                  g_friends ? kOk : kMuted);
    } else if (has("'s character arrived")) {
        int k = number_after(l, "net: guest ");
        add_event(tr(L"Le perso de l'ami ") + std::to_wstring(k) + tr(L" est arrivé : il remplace un pion."));
    } else if (has("rejoin: idle -> saving")) {
        add_event(tr(L"Sauvegarde et rechargement pour faire entrer un ami (quelques secondes)…"));
    } else if (has("rejoin: world reloaded")) {
        add_event(tr(L"Monde rechargé."));
    } else if (has("net: peer silent")) {
        add_event(tr(L"Connexion à l'hôte perdue : reconnexion automatique… Si ça dure plus d'une minute, vérifie ta connexion internet et que l'hôte est toujours en jeu."));
        set_state(tr(L"Connexion à l'hôte perdue, reconnexion automatique…"), kWarn);
        g_world_received = false;
    } else if (has("net: stage: guest in ") && has(" jumps to ")) {
        add_event(tr(L"Changement de zone avec l'hôte."));
    } else if (has("net: send_character=0")) {
        add_event(tr(L"Tu joues le pion de l'hôte tel quel (sans ton propre perso)."));
    } else if (has("steam: friends in Dragon's Dogma:")) {
        std::string list = l.substr(l.find("Dragon's Dogma:") + 15), fr;
        size_t at = 0;
        while ((at = list.find("...", at)) != std::string::npos) {
            std::string id = list.substr(at, 7), role = list.substr(list.find('(', at) + 1);
            role = role.substr(0, role.find(')'));
            fr += (fr.empty() ? "" : ", ") + id + " (" +
                  (role == "host" ? "hôte" : role == "guest" ? "invité" : "sans le mod") + ")";
            at += 3;
        }
        int n = MultiByteToWideChar(CP_UTF8, 0, fr.c_str(), -1, nullptr, 0);
        std::wstring w(n > 0 ? n - 1 : 0, L'\0');
        if (n > 1)
            MultiByteToWideChar(CP_UTF8, 0, fr.c_str(), -1, &w[0], n);
        add_event(fr.empty() ? tr(L"Aucun ami Steam dans Dragon's Dogma pour l'instant.")
                             : tr(L"Amis Steam dans le jeu (fin de leur identifiant) : ") + w);
    } else if (has("roam: exploring alone")) {
        add_event(tr(L"Tu explores seul, avec ton perso et ton sac. LB + RB + Y (ou F7) pour revenir chez l'hôte."));
        set_state(tr(L"Exploration libre : ce que tu fais ne compte que chez toi (sauf ta progression)."), kInfo);
    } else if (has("roam: back with the host")) {
        add_event(tr(L"De retour avec l'hôte."));
        set_state(tr(L"Connecté : tu joues avec l'hôte."), kOk);
    } else if (has("roam: not now")) {
        add_event(tr(L"Exploration : pas maintenant (dialogue, chargement ou changement de zone en cours)."));
    } else if (has("explores on its own")) {
        add_event(tr(L"Ami ") + std::to_wstring(number_after(l, "net: guest ")) + tr(L" explore seul : son pion repasse à l'IA."));
    } else if (has("is back: it drives its pawn again")) {
        add_event(tr(L"Ami ") + std::to_wstring(number_after(l, "net: guest ")) + tr(L" est revenu : il reprend son pion."));
    } else if (has("progress: free roam:")) {
        long long exp = 0, jp = 0;
        int kinds = 0;
        size_t at = l.find("free roam: +");
        if (at != std::string::npos)
            sscanf(l.c_str() + at, "free roam: +%lld exp, +%lld discipline, %d new item", &exp, &jp, &kinds);
        add_event(tr(L"Gagné en exploration : +") + std::to_wstring(exp) + tr(L" exp, +") + std::to_wstring(jp) +
                  tr(L" discipline, ") + std::to_wstring(kinds) + tr(L" sorte(s) d'objets (gardé pour ta partie)."));
    } else if (has("progress: from the host:")) {
        long long exp = 0, jp = 0;
        int kinds = 0;
        size_t at = l.find("pending in total +");
        if (at != std::string::npos)
            sscanf(l.c_str() + at, "pending in total +%lld exp, +%lld discipline, %d item", &exp, &jp, &kinds);
        add_event(tr(L"Progression gardée pour ton perso : +") + std::to_wstring(exp) + tr(L" exp, +") + std::to_wstring(jp) +
                  tr(L" discipline, ") + std::to_wstring(kinds) + tr(L" sorte(s) d'objets."));
    } else if (has("progress: added to ")) {
        add_event(tr(L"Progression co-op ajoutée à ta partie (copie de secours dans coop_progress\\backups)."));
    } else if (has("progress: the game saved with the gains")) {
        add_event(tr(L"Progression co-op enregistrée dans ta sauvegarde."));
    } else if (has("progress: pending gains are for")) {
        add_event(tr(L"Progression co-op en attente : elle appartient à un autre perso, rien n'a été ajouté."));
    }
}

void poll_log()
{
    if (!g_watching || g_game_dir.empty())
        return;
    std::wstring log = g_game_dir + L"\\coop.log";
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(log.c_str(), GetFileExInfoStandard, &a))
        return;
    if (CompareFileTime(&a.ftLastWriteTime, &g_launch_time) < 0)
        return;
    long long size = ((long long)a.nFileSizeHigh << 32) | a.nFileSizeLow;
    if (size < g_log_pos)
        g_log_pos = 0;
    std::string chunk;
    if (!read_file(log, chunk, g_log_pos))
        return;
    size_t end = chunk.rfind('\n');
    if (end == std::string::npos)
        return;
    g_log_pos += (long long)(end + 1);
    size_t start = 0;
    for (size_t nl; (nl = chunk.find('\n', start)) != std::string::npos && nl <= end; start = nl + 1)
        on_log_line(chunk.substr(start, nl - start));
}

void tell(bool &told, const std::wstring &state, const std::wstring &event, COLORREF color)
{
    if (told)
        return;
    told = true;
    set_state(state, color);
    add_event(event);
}

void check_watchers()
{
    if (!g_watching || !g_launch_tick)
        return;
    DWORD since = GetTickCount() - g_launch_tick;
    bool running = game_running();
    if (running)
        g_game_seen = true;
    if (g_game_seen && !running && !g_told_closed) {
        tell(g_told_closed, tr(L"Le jeu s'est fermé."),
             tr(L"Le jeu s'est fermé. S'il a planté, clique sur « Exporter le journal » et envoie le fichier à l'hôte."), kWarn);
        g_watching = false;
        return;
    }
    if (g_game_seen && !g_mod_loaded && since > 90000)
        tell(g_told_no_mod, tr(L"Le mod ne s'est pas chargé."),
             tr(L"Le jeu tourne depuis 1 min 30 mais le mod ne s'est pas chargé : vérifie que dinput8.dll est bien dans le dossier du jeu (relance avec « Lancer le jeu ») et qu'aucun antivirus ne l'a supprimé."),
             kBad);
    if (!g_game_seen && since > 120000)
        tell(g_told_no_mod, tr(L"Le jeu ne démarre pas."),
             g_gog ? tr(L"Le jeu n'a pas démarré en 2 minutes : vérifie que GOG Galaxy est ouvert, puis relance.")
                   : tr(L"Le jeu n'a pas démarré en 2 minutes : vérifie que Steam est ouvert et connecté, puis relance."),
             kBad);
    if (g_steam_wait_since && !g_link_up && GetTickCount() - g_steam_wait_since > 30000)
        tell(g_told_steam, tr(L"Steam ne répond pas."),
             tr(L"Le mod attend Steam depuis 30 s : vérifie que Steam est ouvert, connecté (pas en mode hors ligne), puis relance le jeu."),
             kBad);
    if (guest_role() && g_link_up && !g_host_found && since > 90000 && lan_mode())
        tell(g_told_no_host, tr(L"Hôte introuvable pour l'instant."),
             tr(L"Aucune réponse de l'hôte : vérifie l'adresse dans le Salon, que l'hôte a lancé le jeu en « Hôte » et chargé sa partie, et, par internet, que son port UDP 27101 est ouvert. La recherche continue."),
             kWarn);
    if (guest_role() && g_link_up && !g_host_found && since > 90000 && !lan_mode())
        tell(g_told_no_host, tr(L"Hôte introuvable pour l'instant."),
             tr(L"Aucun hôte trouvé : vérifie que l'hôte a lancé le jeu avec « L'hôte », que vous êtes amis sur Steam, et que son statut Steam n'est pas « Invisible ». La recherche continue."),
             kWarn);
}

struct Seat {
    bool used = false, me = false, down = false, host = false;
    std::wstring role, steam, character, zone, state, link;
    COLORREF link_color = 0;
    int level = 0, ping = 0;
};

Seat g_seats[4];
std::wstring g_salon_info = tr(L"Le jeu n'est pas lancé.");
HBITMAP g_avatar[4] = {};
FILETIME g_avatar_time[4] = {};

std::wstring widen_utf8(const std::string &s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n > 0 ? n : 0), L'\0');
    if (n > 0)
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), &w[0], n);
    return w;
}

void load_avatar(int k, bool used)
{
    std::wstring path = g_game_dir + L"\\coop_avatar_" + std::to_wstring(k) + L".bmp";
    WIN32_FILE_ATTRIBUTE_DATA a;
    bool have = used && GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a) &&
                CompareFileTime(&a.ftLastWriteTime, &g_launch_time) >= 0;
    if (!have) {
        if (g_avatar[k])
            DeleteObject(g_avatar[k]);
        g_avatar[k] = nullptr;
        return;
    }
    if (g_avatar[k] && CompareFileTime(&a.ftLastWriteTime, &g_avatar_time[k]) == 0)
        return;
    if (g_avatar[k])
        DeleteObject(g_avatar[k]);
    g_avatar[k] = HBITMAP(LoadImageW(nullptr, path.c_str(), IMAGE_BITMAP, 0, 0, LR_LOADFROMFILE | LR_CREATEDIBSECTION));
    g_avatar_time[k] = a.ftLastWriteTime;
}

bool g_demo_party = false;
void demo_party()
{
    const wchar_t *steam[3] = {L"Wakestone", L"Sunpaw", L"Grigori"}, *chars[3] = {L"Elric", L"Mira", L"Tomas"};
    const int levels[3] = {74, 61, 55}, pings[3] = {0, 48, 132};
    for (int k = 0; k < 4; ++k) {
        Seat r;
        if (k < 3) {
            r.used = true;
            r.me = k == 0;
            r.host = k == 0;
            r.role = k == 0 ? tr(L"HÔTE") : tr(L"INVITÉ ") + std::to_wstring(k);
            r.steam = steam[k];
            r.character = chars[k];
            r.level = levels[k];
            r.ping = pings[k];
            r.zone = k == 0 ? tr(L"Ta zone") : tr(L"Même zone");
            r.down = k == 2;
            r.state = k == 2 ? tr(L"⚠ À terre") : tr(L"Debout");
            r.link = tr(L"Bonne connexion");
            r.link_color = kOk;
        }
        g_seats[k] = r;
    }
    if (!g_avatar[1]) {
        BITMAPINFO bi = {};
        bi.bmiHeader = {sizeof(BITMAPINFOHEADER), 64, -64, 1, 32, BI_RGB};
        void *bits = nullptr;
        g_avatar[1] = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (bits)
            for (int y = 0; y < 64; ++y)
                for (int x = 0; x < 64; ++x)
                    static_cast<uint32_t *>(bits)[y * 64 + x] = RGB(0x30 + x, 0x50 + y, 0x90 + (x + y) / 2);
    }
    g_salon_info = std::wstring(tr(L"Monde partagé")) + L" · Steam · 3" + tr(L"/4 joueurs") + tr(L" · tu es l'hôte");
}

void update_players()
{
    if (!g_players)
        return;
    if (g_demo_party) {
        demo_party();
        InvalidateRect(g_players, nullptr, FALSE);
        if (g_party)
            InvalidateRect(g_party, nullptr, FALSE);
        return;
    }
    std::string text;
    WIN32_FILE_ATTRIBUTE_DATA a;
    std::wstring path = g_game_dir + L"\\coop_status.txt";
    bool fresh = g_watching && GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a) &&
                 CompareFileTime(&a.ftLastWriteTime, &g_launch_time) >= 0 && read_file(path, text);
    FILETIME now_ft;
    GetSystemTimeAsFileTime(&now_ft);
    ULARGE_INTEGER n, w;
    n.LowPart = now_ft.dwLowDateTime, n.HighPart = now_ft.dwHighDateTime;
    w.LowPart = fresh ? a.ftLastWriteTime.dwLowDateTime : 0, w.HighPart = fresh ? a.ftLastWriteTime.dwHighDateTime : 0;
    bool stale = !fresh || n.QuadPart - w.QuadPart > 50000000ULL;
    int me = -1, count = 0;
    bool host_role = false;
    char mode[16] = "", link[16] = "";
    int day = -1, day_ms = -1;
    Seat seats[4];
    size_t at = 0;
    while (!stale && at < text.size()) {
        size_t nl = text.find('\n', at);
        std::string line = text.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
        at = nl == std::string::npos ? text.size() : nl + 1;
        int slot, conn, present, down, stage, same;
        long ago;
        char name[160] = {};
        if (sscanf(line.c_str(), "me %d", &me) == 1)
            continue;
        if (line == "role host")
            host_role = true;
        if (sscanf(line.c_str(), "game %15s %15s %d %d", mode, link, &day, &day_ms) >= 2)
            continue;
        int v = 0;
        unsigned long long self_id = 0;
        if (sscanf(line.c_str(), "level %d %d", &slot, &v) == 2 && slot >= 0 && slot < 4) {
            seats[slot].level = v;
            continue;
        }
        if (sscanf(line.c_str(), "ping %d %d", &slot, &v) == 2 && slot >= 0 && slot < 4) {
            seats[slot].ping = v;
            continue;
        }
        if (sscanf(line.c_str(), "self %llu", &self_id) == 1) {
            g_self_steam = self_id;
            continue;
        }
        if (sscanf(line.c_str(), "steam %d %159[^\n]", &slot, name) >= 1 && slot >= 0 && slot < 4) {
            seats[slot].steam = widen_utf8(name);
            continue;
        }
        if (sscanf(line.c_str(), "player %d %d %d %d %d %ld %d %159[^\n]", &slot, &conn, &present, &down, &stage, &ago, &same,
                   name) < 7 || !conn || slot < 0 || slot >= 4)
            continue;
        Seat &r = seats[slot];
        r.used = true;
        ++count;
        r.me = slot == me;
        r.down = present && down;
        r.role = slot == 0 ? tr(L"HÔTE") : tr(L"INVITÉ ") + std::to_wstring(slot);
        r.host = slot == 0;
        r.character = widen_utf8(name);
        r.zone = r.me ? tr(L"Ta zone") : !present ? tr(L"Zone inconnue") : same ? tr(L"Même zone")
                                                                        : tr(L"Autre zone (n° ") + std::to_wstring(stage) + L")";
        r.state = !present ? L"" : down ? tr(L"\u26A0 À terre") : tr(L"Debout");
        r.link = r.me ? tr(L"Toi") : ago < 0 ? tr(L"Connexion inconnue") : ago < 1500 ? tr(L"Bonne connexion") : ago < 5000 ? tr(L"Connexion lente")
                                                                                                              : tr(L"Coupée (") + std::to_wstring(ago / 1000) + L" s)";
        r.link_color = r.me ? kInfo : ago < 0 ? kMuted : ago < 1500 ? kOk : ago < 5000 ? kWarn : kBad;
    }
    for (int k = 0; k < 4; ++k) {
        if (!seats[k].used) {
            seats[k].steam.clear();
            seats[k].level = seats[k].ping = 0;
        }
        g_seats[k] = seats[k];
        load_avatar(k, seats[k].used);
    }
    if (!g_watching) {
        g_salon_info = tr(L"Le jeu n'est pas lancé.");
    } else if (stale || !count) {
        g_salon_info = tr(L"Salon vide pour l'instant : il se remplit une fois en jeu.");
    } else {
        g_salon_info = std::wstring(strcmp(mode, "shared") == 0 ? tr(L"Monde partagé") : tr(L"Mode classique")) +
                       (strcmp(link, "steam") == 0 ? L" · Steam" : tr(L" · réseau local")) + L" · " + std::to_wstring(count) +
                       tr(L"/4 joueurs");
        if (day >= 0 && day_ms >= 0) {
            wchar_t t[64];
            _snwprintf(t, 64, tr(L" · jour %d, %02d:%02d"), day, day_ms / 3600000 % 24, day_ms / 60000 % 60);
            t[63] = 0;
            g_salon_info += t;
        }
        g_salon_info += host_role ? tr(L" · tu es l'hôte") : tr(L" · tu es invité");
    }
    std::wstring words = tr(L"Salon. ") + g_salon_info;
    for (const Seat &r : g_seats)
        if (r.used)
            words += L". " + r.role + L" " + (r.steam.empty() ? r.character : r.steam) +
                     (r.level ? tr(L", niveau ") + std::to_wstring(r.level) : L"") + L", " + r.zone + L", " + r.state + L", " +
                     (r.ping ? std::to_wstring(r.ping) + L" ms" : r.link);
    SetWindowTextW(g_players, words.c_str());
    InvalidateRect(g_players, nullptr, FALSE);
    if (g_party) {
        SetWindowTextW(g_party, words.c_str());
        InvalidateRect(g_party, nullptr, FALSE);
    }
    if (g_copy_code && bool(IsWindowEnabled(g_copy_code)) != (lan_mode() || g_self_steam != 0)) {
        EnableWindow(g_copy_code, lan_mode() || g_self_steam != 0);
        InvalidateRect(g_wnd, nullptr, FALSE);
    }
}

void paint_portrait(HDC dc, const RECT &pic, int k, const Seat &r)
{
    static const COLORREF seat_colors[4] = {RGB(0x5c, 0x46, 0x30), RGB(0x3a, 0x4a, 0x5c), RGB(0x3e, 0x58, 0x44), RGB(0x58, 0x3d, 0x4e)};
    int ring = px(2);
    RECT in = pic;
    InflateRect(&in, -ring - px(2), -ring - px(2));
    theme::circle(dc, pic, CLR_INVALID, r.host ? theme::kAccent : r.me ? theme::kText : theme::kBorder, float(ring));
    if (g_avatar[k]) {
        theme::circle_picture(dc, in, g_avatar[k]);
        return;
    }
    theme::circle(dc, in, seat_colors[k]);
    std::wstring who = r.steam.empty() ? r.character : r.steam;
    std::wstring letter = who.empty() ? std::to_wstring(k + 1) : who.substr(0, 1);
    SelectObject(dc, g_head);
    SetTextColor(dc, theme::kText);
    RECT t = in;
    DrawTextW(dc, letter.c_str(), -1, &t, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
}

void paint_empty_portrait(HDC dc, const RECT &pic, int k)
{
    theme::circle(dc, pic, CLR_INVALID, theme::kBorder, 1.5f, true);
    SelectObject(dc, g_head);
    SetTextColor(dc, theme::mix(kMuted, kBg, 0.45f));
    RECT t = pic;
    DrawTextW(dc, std::to_wstring(k + 1).c_str(), -1, &t, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
}

void paint_place(HDC dc, const Seat &r, RECT t, UINT align)
{
    SelectObject(dc, g_small);
    SetTextColor(dc, r.host ? theme::kAccent : kMuted);
    SetTextCharacterExtra(dc, px(1));
    DrawTextW(dc, r.role.c_str(), -1, &t, align | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    SetTextCharacterExtra(dc, 0);
}

std::wstring seat_title(const Seat &r)
{
    return (r.steam.empty() ? (r.character.empty() ? r.role : r.character) : r.steam) + (r.me ? tr(L" (toi)") : L"");
}

void paint_party_columns(HDC mem, const RECT &rc)
{
    SetTextColor(mem, kMuted);
    RECT info = {0, 0, rc.right, px(22)};
    DrawTextW(mem, g_salon_info.c_str(), -1, &info, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    int top = px(32), gap = px(theme::kGapSmall);
    int cw = (rc.right - 3 * gap) / 4, ch = rc.bottom - top;
    for (int k = 0; k < 4; ++k) {
        const Seat &r = g_seats[k];
        RECT c = {k * (cw + gap), top, k * (cw + gap) + cw, top + ch};
        int av = px(88);
        RECT pic = {c.left + (cw - av) / 2, c.top + px(20), c.left + (cw - av) / 2 + av, c.top + px(20) + av};
        if (!r.used) {
            theme::round_rect(mem, c, px(theme::kRadiusCard), CLR_INVALID, theme::kBorder, 1.f, true);
            paint_empty_portrait(mem, pic, k);
            SelectObject(mem, g_font);
            SetTextColor(mem, kMuted);
            RECT t = {c.left, pic.bottom + px(16), c.right, pic.bottom + px(40)};
            DrawTextW(mem, tr(L"Place libre"), -1, &t, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
            continue;
        }
        theme::round_rect(mem, c, px(theme::kRadiusCard), kCard, r.me ? theme::mix(theme::kBorder, theme::kAccent, 0.55f) : theme::kBorder);
        paint_portrait(mem, pic, k, r);
        int x0 = c.left + px(10), x1 = c.right - px(10), y = pic.bottom + px(12);
        paint_place(mem, r, {x0, y, x1, y + px(18)}, DT_CENTER);
        y += px(20);
        SelectObject(mem, g_bold);
        SetTextColor(mem, kText);
        std::wstring title = seat_title(r);
        RECT t = {x0, y, x1, y + px(22)};
        DrawTextW(mem, title.c_str(), -1, &t, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        y += px(24);
        SelectObject(mem, g_font);
        SetTextColor(mem, kMuted);
        std::wstring line2 = r.character;
        if (r.level) {
            std::wstring lv = tr(L" · niveau ") + std::to_wstring(r.level);
            line2 += line2.empty() && lv.size() > 3 ? lv.substr(3) : lv;
        }
        t = {x0, y, x1, y + px(20)};
        DrawTextW(mem, line2.c_str(), -1, &t, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        y += px(20);
        SetTextColor(mem, r.down ? kBad : kMuted);
        std::wstring line3 = r.zone + (r.state.empty() ? L"" : L" · " + r.state);
        t = {x0, y, x1, y + px(20)};
        DrawTextW(mem, line3.c_str(), -1, &t, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        std::wstring ping = r.me ? tr(L"toi") : r.ping ? std::to_wstring(r.ping) + L" ms" : r.link;
        COLORREF pc = r.me ? kInfo : r.ping ? (r.ping < 90 ? kOk : r.ping < 180 ? kWarn : kBad) : r.link_color;
        SelectObject(mem, g_small);
        SIZE ps = {};
        GetTextExtentPoint32W(mem, ping.c_str(), int(ping.size()), &ps);
        int pw = ps.cx + px(20);
        if (pw > cw - px(16))
            pw = cw - px(16);
        RECT pill = {c.left + (cw - pw) / 2, c.bottom - px(14) - px(22), c.left + (cw - pw) / 2 + pw, c.bottom - px(14)};
        theme::round_rect(mem, pill, px(11), theme::mix(kCard, pc, 0.18f));
        SetTextColor(mem, pc);
        DrawTextW(mem, ping.c_str(), -1, &pill, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
}

void paint_party_row(HDC mem, const RECT &rc)
{
    SelectObject(mem, g_small);
    SetTextColor(mem, kMuted);
    SetTextCharacterExtra(mem, px(1));
    std::wstring head = tr(L"Ton groupe");
    CharUpperW(&head[0]);
    RECT h = {0, 0, rc.right, px(20)};
    DrawTextW(mem, head.c_str(), -1, &h, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    SetTextCharacterExtra(mem, 0);
    int top = px(28), gap = px(theme::kGapSmall);
    int sw = (rc.right - 3 * gap) / 4, sh = rc.bottom - top - px(2);
    for (int k = 0; k < 4; ++k) {
        const Seat &r = g_seats[k];
        RECT c = {k * (sw + gap), top, k * (sw + gap) + sw, top + sh};
        int av = px(48);
        RECT pic = {c.left + px(12), c.top + (sh - av) / 2, c.left + px(12) + av, c.top + (sh - av) / 2 + av};
        int x0 = pic.right + px(12), x1 = c.right - px(10);
        if (!r.used) {
            theme::round_rect(mem, c, px(theme::kRadiusCard), CLR_INVALID, theme::kBorder, 1.f, true);
            paint_empty_portrait(mem, pic, k);
            SelectObject(mem, g_font);
            SetTextColor(mem, kMuted);
            RECT t = {x0, c.top, x1, c.bottom};
            DrawTextW(mem, tr(L"Place libre"), -1, &t, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
            continue;
        }
        theme::round_rect(mem, c, px(theme::kRadiusCard), kCard, r.me ? theme::mix(theme::kBorder, theme::kAccent, 0.55f) : theme::kBorder);
        paint_portrait(mem, pic, k, r);
        int mid = (c.top + c.bottom) / 2;
        SelectObject(mem, g_bold);
        SetTextColor(mem, kText);
        std::wstring title = seat_title(r);
        RECT t = {x0, mid - px(21), x1, mid + px(1)};
        DrawTextW(mem, title.c_str(), -1, &t, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        paint_place(mem, r, {x0, mid + px(2), x1, mid + px(20)}, DT_LEFT);
    }
}

void paint_salon(HWND h, HDC dc)
{
    RECT rc;
    GetClientRect(h, &rc);
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP buf = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
    HGDIOBJ old_buf = SelectObject(mem, buf);
    FillRect(mem, &rc, g_bg_brush);
    SetBkMode(mem, TRANSPARENT);
    HGDIOBJ old_font = SelectObject(mem, g_font);
    if (h == g_party)
        paint_party_row(mem, rc);
    else
        paint_party_columns(mem, rc);
    SelectObject(mem, old_font);
    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old_buf);
    DeleteObject(buf);
    DeleteDC(mem);
}

LRESULT CALLBACK salon_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        paint_salon(h, dc);
        EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_ERASEBKGND)
        return 1;
    return DefWindowProcW(h, m, w, l);
}

struct ButtonLook {
    HWND h;
    float hover;
    bool inside;
};
std::vector<ButtonLook> g_buttons;
const UINT_PTR kHoverTimer = 77;

ButtonLook *look_of(HWND h)
{
    for (ButtonLook &b : g_buttons)
        if (b.h == h)
            return &b;
    return nullptr;
}

LRESULT CALLBACK button_sub(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR)
{
    if (m == WM_MOUSEMOVE) {
        if (ButtonLook *b = look_of(h); b && !b->inside) {
            b->inside = true;
            TRACKMOUSEEVENT t = {sizeof t, TME_LEAVE, h, 0};
            TrackMouseEvent(&t);
            SetTimer(GetParent(h), kHoverTimer, 15, nullptr);
        }
    } else if (m == WM_MOUSELEAVE) {
        if (ButtonLook *b = look_of(h))
            b->inside = false;
        SetTimer(GetParent(h), kHoverTimer, 15, nullptr);
    } else if (m == WM_ERASEBKGND) {
        return 1;
    }
    return DefSubclassProc(h, m, w, l);
}

void hover_step(HWND parent)
{
    bool moving = false;
    float step = 15.f / float(theme::kHoverMs);
    for (ButtonLook &b : g_buttons) {
        float target = b.inside ? 1.f : 0.f;
        if (b.hover == target)
            continue;
        b.hover = b.hover < target ? (b.hover + step > target ? target : b.hover + step) : (b.hover - step < target ? target : b.hover - step);
        InvalidateRect(b.h, nullptr, FALSE);
        moving = true;
    }
    if (!moving)
        KillTimer(parent, kHoverTimer);
}

enum ButtonKind { BTN_PRIMARY, BTN_SECONDARY, BTN_GHOST, BTN_NAV, BTN_ROLE, BTN_SWITCH, BTN_DANGER, BTN_CHOICE };
struct RoleCard {
    const wchar_t *icon, *title, *text;
};
const RoleCard kRoles[3] = {
    {L"\uE80F", L"Hôte", L"Je charge ma partie ; mes amis me rejoignent."},
    {L"\uE8FA", L"Invité", L"Je rejoins un ami : ma partie n'est ni chargée ni sauvegardée."},
    {L"\uE77B", L"Seul", L"Je joue ma partie ; la progression gagnée en co-op y est ajoutée."},
};
ButtonKind kind_of(int id);

COLORREF under(HWND c)
{
    RECT rc;
    GetWindowRect(c, &rc);
    MapWindowPoints(nullptr, g_wnd, reinterpret_cast<POINT *>(&rc), 2);
    auto inside = [&](int top, int bottom) { return rc.top >= px(top) && rc.bottom <= px(bottom); };
    if (g_page == PAGE_SALON && inside(kConnY, kConnY + kConnH))
        return kCard;
    if (g_page == PAGE_SETTINGS && inside(kDangerY, kDangerY + 96))
        return theme::mix(theme::kSurface, theme::kBad, 0.05f);
    return kBg;
}

void fill(HDC dc, const RECT &r, COLORREF c)
{
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}

void draw_button(const DRAWITEMSTRUCT *d)
{
    ButtonLook *lk = look_of(d->hwndItem);
    float hv = lk ? lk->hover : 0.f;
    ButtonKind kind = kind_of(int(d->CtlID));
    bool pressed = d->itemState & ODS_SELECTED, disabled = d->itemState & ODS_DISABLED, focus = d->itemState & ODS_FOCUS;
    bool show_focus = focus && !(d->itemState & ODS_NOFOCUSRECT);
    UINT hide_prefix = (d->itemState & ODS_NOACCEL) ? DT_HIDEPREFIX : 0;
    RECT r = d->rcItem;
    HDC dc = d->hDC;
    SetBkMode(dc, TRANSPARENT);
    if (kind == BTN_ROLE) {
        int role = int(d->CtlID) == ID_GUEST ? ROLE_GUEST : int(d->CtlID) == ID_SOLO ? ROLE_SOLO : ROLE_HOST;
        bool chosen = role == g_role;
        fill(dc, r, theme::kBg);
        RECT card = r;
        InflateRect(&card, -px(1), -px(1));
        COLORREF face = theme::mix(theme::kSurface, theme::kSurfaceHover, pressed ? 1.f : hv);
        theme::round_rect(dc, card, px(theme::kRadiusCard), chosen ? theme::mix(face, theme::kAccent, 0.06f) : face,
                          chosen ? theme::kAccent : theme::kBorder, chosen ? 1.5f : 1.f);
        if (show_focus) {
            RECT f = card;
            InflateRect(&f, -px(4), -px(4));
            theme::round_rect(dc, f, px(theme::kRadiusCard - 4), CLR_INVALID, theme::kFocus, 1.f);
        }
        const RoleCard &rc = kRoles[role];
        int pad = px(theme::kPadCard);
        RECT ic = {card.left + pad, card.top + pad, card.left + pad + px(34), card.top + pad + px(34)};
        theme::circle(dc, ic, chosen ? theme::kAccent : theme::kSurfaceRaised);
        HGDIOBJ old = SelectObject(dc, g_icon);
        SetTextColor(dc, chosen ? theme::kOnAccent : theme::kText);
        DrawTextW(dc, rc.icon, -1, &ic, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        SelectObject(dc, g_head);
        SetTextColor(dc, chosen ? theme::kAccent : theme::kText);
        RECT tt = {ic.right + px(12), card.top + pad, card.right - pad, card.top + pad + px(34)};
        static const wchar_t *titles[3] = {tr(L"&Hôte"), tr(L"&Invité"), tr(L"&Seul")};
        DrawTextW(dc, titles[role], -1, &tt, DT_LEFT | DT_SINGLELINE | DT_VCENTER | hide_prefix);
        SelectObject(dc, g_font);
        SetTextColor(dc, theme::kMuted);
        RECT dt = {card.left + pad, ic.bottom + px(8), card.right - pad, card.bottom - px(6)};
        DrawTextW(dc, tr(rc.text), -1, &dt, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX | DT_END_ELLIPSIS);
        SelectObject(dc, old);
        return;
    }
    if (kind == BTN_SWITCH) {
        bool on = switch_on(d->hwndItem) || (d->hwndItem == g_lan && g_gog);
        std::wstring label;
        for (const Switch &w : g_switches)
            if (w.h == d->hwndItem)
                label = w.label;
        COLORREF bg = under(d->hwndItem);
        fill(dc, r, bg);
        RECT row = r;
        if (hv > 0 || focus)
            theme::round_rect(dc, row, px(theme::kRadiusButton), theme::mix(bg, theme::kSurface, focus ? 1.f : hv));
        if (show_focus)
            theme::round_rect(dc, row, px(theme::kRadiusButton), CLR_INVALID, theme::kFocus, 1.f);
        int sw = px(40), sh = px(22);
        RECT pill = {r.right - px(10) - sw, (r.top + r.bottom - sh) / 2, r.right - px(10), (r.top + r.bottom + sh) / 2};
        theme::round_rect(dc, pill, sh / 2, on ? theme::kAccent : theme::kSurfaceRaised, on ? CLR_INVALID : theme::kBorder);
        int k = sh - px(6);
        RECT knob = {on ? pill.right - px(3) - k : pill.left + px(3), pill.top + px(3), 0, 0};
        knob.right = knob.left + k;
        knob.bottom = knob.top + k;
        theme::circle(dc, knob, on ? theme::kOnAccent : theme::kMuted);
        SetTextColor(dc, disabled ? theme::kMuted : theme::kText);
        HGDIOBJ old = SelectObject(dc, g_font);
        RECT t = {r.left + px(10), r.top, pill.left - px(12), r.bottom};
        DrawTextW(dc, label.c_str(), -1, &t, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | hide_prefix);
        SelectObject(dc, old);
        return;
    }
    if (kind == BTN_NAV) {
        bool lang = d->CtlID == ID_LANG;
        int page = lang ? -1 : int(d->CtlID) - ID_NAV;
        bool current = page == g_page;
        fill(dc, r, theme::kTopbar);
        RECT pill = r;
        InflateRect(&pill, -px(4), -px(14));
        if (!current && (hv > 0 || pressed))
            theme::round_rect(dc, pill, px(theme::kRadiusButton), theme::mix(theme::kTopbar, theme::kSurfaceHover, pressed ? 1.f : hv));
        if (show_focus)
            theme::round_rect(dc, pill, px(theme::kRadiusButton), CLR_INVALID, theme::kFocus, 1.f);
        HGDIOBJ old = SelectObject(dc, g_tab);
        if (lang) {
            SetTextColor(dc, theme::mix(theme::kMuted, theme::kText, pressed ? 1.f : hv));
            SelectObject(dc, g_small_icon);
            RECT ic = {r.left + px(10), r.top, r.left + px(30), r.bottom};
            DrawTextW(dc, L"", -1, &ic, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
            SelectObject(dc, g_bold);
            RECT lb = {ic.right + px(4), r.top, r.right - px(4), r.bottom};
            DrawTextW(dc, kLangCodes[g_lang], -1, &lb, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
            SelectObject(dc, old);
            return;
        }
        const wchar_t *name = tr(kPageNames[page]);
        SetTextColor(dc, current ? theme::kAccent : theme::mix(theme::kMuted, theme::kText, pressed ? 1.f : hv));
        DrawTextW(dc, name, -1, &r, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
        SIZE ts = {};
        GetTextExtentPoint32W(dc, name, int(wcslen(name)), &ts);
        int w = ts.cx < r.right - r.left - px(8) ? ts.cx : r.right - r.left - px(8), cx = (r.left + r.right) / 2;
        if (current) {
            RECT bar = {cx - w / 2, r.bottom - px(5), cx + w / 2, r.bottom - px(3)};
            theme::round_rect(dc, bar, px(1), theme::kAccent);
        }
        if (page == PAGE_LOG && g_log_unseen) {
            RECT dot = {cx + w / 2 + px(4), (r.top + r.bottom) / 2 - px(9), cx + w / 2 + px(11), (r.top + r.bottom) / 2 - px(2)};
            theme::circle(dc, dot, theme::kWarn);
        }
        SelectObject(dc, old);
        return;
    }
    COLORREF bg = under(d->hwndItem);
    fill(dc, r, bg);
    wchar_t label[256] = L"";
    GetWindowTextW(d->hwndItem, label, 256);
    if (kind == BTN_PRIMARY) {
        COLORREF top = theme::mix(theme::kAccentHover, RGB(0xf0, 0xd2, 0x86), 0.5f * hv), foot = theme::mix(theme::kAccentDeep, theme::kAccent, 0.6f * hv);
        if (pressed)
            top = theme::kAccent, foot = theme::kAccentDeep;
        if (disabled)
            top = foot = theme::kSurfaceRaised;
        theme::round_rect_v(dc, r, px(theme::kRadiusButton), top, foot, disabled ? theme::kBorder : theme::mix(theme::kAccentDeep, theme::kOnAccent, 0.35f));
        if (show_focus) {
            RECT f = r;
            InflateRect(&f, -px(3), -px(3));
            theme::round_rect(dc, f, px(theme::kRadiusButton - 2), CLR_INVALID, theme::kOnAccent, 1.5f);
        }
        CharUpperW(label);
        SetTextColor(dc, disabled ? theme::kMuted : theme::kOnAccent);
        HGDIOBJ old = SelectObject(dc, g_head);
        SetTextCharacterExtra(dc, px(2));
        DrawTextW(dc, label, -1, &r, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | hide_prefix);
        SetTextCharacterExtra(dc, 0);
        SelectObject(dc, old);
        return;
    }
    COLORREF face, text = theme::kText, border = CLR_INVALID;
    if (kind == BTN_GHOST) {
        face = theme::mix(bg, theme::kSurfaceHover, pressed ? 1.f : hv);
    } else if (kind == BTN_DANGER) {
        face = theme::mix(theme::kSurface, theme::kBad, pressed ? 0.28f : 0.06f + 0.14f * hv);
        border = theme::mix(theme::kBorder, theme::kBad, 0.7f);
        text = theme::kBad;
    } else if (kind == BTN_CHOICE && (d->CtlID == ID_STORE_GOG) == (g_store == 1)) {
        face = theme::mix(theme::kSurface, theme::kAccent, pressed ? 0.30f : 0.14f + 0.08f * hv);
        border = theme::kAccent;
    } else {
        face = theme::mix(theme::kSurfaceRaised, theme::kSurfaceHover, pressed ? 1.f : hv);
        border = theme::mix(theme::kBorder, theme::kAccent, 0.25f * hv);
    }
    if (disabled) {
        face = kind == BTN_GHOST ? bg : theme::kSurface;
        text = theme::mix(theme::kMuted, theme::kBg, 0.35f);
        border = theme::kBorder;
    }
    theme::round_rect(dc, r, px(theme::kRadiusButton), face, border);
    if (show_focus) {
        RECT f = r;
        InflateRect(&f, -px(2), -px(2));
        theme::round_rect(dc, f, px(theme::kRadiusButton - 2), CLR_INVALID, theme::kFocus, 1.5f);
    }
    if (kind == BTN_CHOICE) {
        bool gog = d->CtlID == ID_STORE_GOG, picked = gog == (g_store == 1);
        swprintf(label, 256, L"%ls%ls", picked ? L"✔  " : L"", gog ? L"GOG" : L"Steam");
    }
    SetTextColor(dc, text);
    HGDIOBJ old = SelectObject(dc, HFONT(SendMessageW(d->hwndItem, WM_GETFONT, 0, 0)));
    RECT t = r;
    UINT align = (GetWindowLongW(d->hwndItem, GWL_STYLE) & BS_LEFT) == BS_LEFT ? DT_LEFT : DT_CENTER;
    if (align == DT_LEFT)
        t.left += px(12);
    DrawTextW(dc, label, -1, &t, align | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | hide_prefix);
    SelectObject(dc, old);
}

void draw_button_buffered(const DRAWITEMSTRUCT *d)
{
    RECT r = d->rcItem;
    int w = r.right - r.left, h = r.bottom - r.top;
    HDC mem = w > 0 && h > 0 ? CreateCompatibleDC(d->hDC) : nullptr;
    HBITMAP bmp = mem ? CreateCompatibleBitmap(d->hDC, w, h) : nullptr;
    if (!bmp) {
        if (mem)
            DeleteDC(mem);
        draw_button(d);
        return;
    }
    HGDIOBJ old = SelectObject(mem, bmp);
    DRAWITEMSTRUCT b = *d;
    b.hDC = mem;
    OffsetRect(&b.rcItem, -r.left, -r.top);
    draw_button(&b);
    BitBlt(d->hDC, r.left, r.top, w, h, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
}

HWND make(const wchar_t *cls, const wchar_t *text, DWORD style, int x, int y, int w, int h, int id, HFONT font = nullptr)
{
    bool push = wcscmp(cls, L"BUTTON") == 0 && ((style & 0xF) == BS_PUSHBUTTON || (style & 0xF) == BS_DEFPUSHBUTTON);
    if (push)
        style = (style & ~DWORD(0xF)) | BS_OWNERDRAW;
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, px(x), px(y), px(w), px(h), g_wnd,
                             reinterpret_cast<HMENU>(INT_PTR(id)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, WPARAM(font ? font : g_font), TRUE);
    if (push) {
        g_buttons.push_back({c, 0.f, false});
        SetWindowSubclass(c, button_sub, 1, 0);
    } else if (wcscmp(cls, L"BUTTON") == 0) {
        SetWindowTheme(c, L"", L"");
    }
    return c;
}

ButtonKind kind_of(int id)
{
    if ((id >= ID_NAV && id < ID_NAV + PAGE_COUNT) || id == ID_LANG)
        return BTN_NAV;
    if (id == ID_HOST || id == ID_GUEST || id == ID_SOLO)
        return BTN_ROLE;
    if (id == ID_OWN_CHAR || id == ID_KEEP || id == ID_DEBUG || id == ID_CHECK_UPDATES || id == ID_LAN)
        return BTN_SWITCH;
    if (id == ID_STORE_STEAM || id == ID_STORE_GOG)
        return BTN_CHOICE;
    return id == ID_LAUNCH ? BTN_PRIMARY : id == ID_REMOVE ? BTN_DANGER : id == ID_EVENTS_TOGGLE ? BTN_GHOST : BTN_SECONDARY;
}

int g_height = kH;
std::vector<HWND> g_pages[PAGE_COUNT];
int g_building = PAGE_PLAY;

HWND add(HWND h)
{
    g_pages[g_building].push_back(h);
    return h;
}

void update_controls();

void show_page(int page)
{
    g_page = page;
    if (page == PAGE_LOG)
        g_log_unseen = false;
    for (int k = 0; k < PAGE_COUNT; ++k)
        for (HWND h : g_pages[k])
            ShowWindow(h, k == page ? SW_SHOW : SW_HIDE);
    for (int k = 0; k < PAGE_COUNT; ++k)
        if (HWND nav = GetDlgItem(g_wnd, ID_NAV + k))
            InvalidateRect(nav, nullptr, FALSE);
    if (page == PAGE_LOG && SendMessageW(g_events, LB_GETCOUNT, 0, 0) == 0)
        ShowWindow(g_events, SW_HIDE);
    if (page == PAGE_SAVES && g_backup_dirs.empty())
        ShowWindow(g_backup_list, SW_HIDE);
    if (!kPublicRepo[0]) {
        ShowWindow(g_check_updates, SW_HIDE);
        ShowWindow(g_project, SW_HIDE);
    }
    update_controls();
    RedrawWindow(g_wnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
}

void refresh_names()
{
    HWND cards[3] = {g_host, g_guest, g_solo};
    const wchar_t *names[3] = {tr(L"&Hôte"), tr(L"&Invité"), tr(L"&Seul")};
    for (int k = 0; k < 3; ++k) {
        std::wstring t = std::wstring(names[k]) + (k == g_role ? tr(L", choisi") : L"") + L". " + tr(kRoles[k].text);
        SetWindowTextW(cards[k], t.c_str());
        InvalidateRect(cards[k], nullptr, FALSE);
    }
    for (const Switch &w : g_switches) {
        SetWindowTextW(w.h, (w.label + (w.on ? tr(L" : activé") : tr(L" : désactivé"))).c_str());
        InvalidateRect(w.h, nullptr, FALSE);
    }
    update_controls();
}

void build_window()
{
    for (int k = 0; k < PAGE_COUNT; ++k)
        make(L"BUTTON", tr(kPageNames[k]), BS_PUSHBUTTON | WS_TABSTOP | (k == 0 ? WS_GROUP : 0), kTabX + k * kTabW, 0, kTabW, kTopH - 1,
             ID_NAV + k);
    const int L = kX, W = kCW;

    make(L"BUTTON", (std::wstring(tr(L"Langue")) + L" : " + kLangNames[g_lang]).c_str(), BS_PUSHBUTTON | WS_TABSTOP,
         kW - 20 - kLangW, 0, kLangW, kTopH - 1, ID_LANG);

    g_building = PAGE_PLAY;
    int y = kTopH + 20;
    add(make(L"STATIC", tr(L"Ton rôle"), 0, L, y, 300, 26, 0, g_head));
    y = kRoleY;
    int cw = (W - 2 * theme::kGapSmall) / 3, ch = kRoleH;
    g_host = add(make(L"BUTTON", tr(L"&Hôte"), BS_PUSHBUTTON | WS_TABSTOP | WS_GROUP, L, y, cw, ch, ID_HOST));
    g_guest = add(make(L"BUTTON", tr(L"&Invité"), BS_PUSHBUTTON | WS_TABSTOP, L + cw + theme::kGapSmall, y, cw, ch, ID_GUEST));
    g_solo = add(make(L"BUTTON", tr(L"&Seul"), BS_PUSHBUTTON | WS_TABSTOP, L + 2 * (cw + theme::kGapSmall), y, cw, ch, ID_SOLO));
    y += ch + 10;
    g_own_char = add(make(L"BUTTON", L"", BS_PUSHBUTTON | WS_TABSTOP, L, y, W, 30, ID_OWN_CHAR));
    g_keep = add(make(L"BUTTON", L"", BS_PUSHBUTTON | WS_TABSTOP, L, y + 32, W, 30, ID_KEEP));
    g_switches.push_back({g_own_char, true, tr(L"Jouer avec mon &propre perso (sinon je joue le pion de l'hôte)")});
    g_switches.push_back({g_keep, true, tr(L"&Garder la progression de mon perso gagnée chez un ami (exp, discipline, objets)")});
    y += 76;
    g_launch = add(make(L"BUTTON", tr(L"&Lancer le jeu"), BS_DEFPUSHBUTTON | WS_TABSTOP, L, y, 320, 56, ID_LAUNCH, g_head));
    g_update = add(make(L"BUTTON", L"", BS_PUSHBUTTON | WS_TABSTOP, L + 340, y + 8, 280, 40, ID_UPDATE));
    add(make(L"BUTTON", tr(L"? A&ide"), BS_PUSHBUTTON | WS_TABSTOP, L + W - 110, y + 6, 110, 44, ID_HELP));
    y += 72;
    g_state_rect = {px(L), px(y), px(L + W), px(y + 56)};
    g_state = add(make(L"STATIC", L"", SS_LEFT | SS_NOPREFIX | SS_CENTERIMAGE, L + 28, y + 8, W - 40, 40, 0, g_bold));
    set_state(tr(L"Choisis ton rôle puis lance le jeu."), kMuted);
    y += 72;
    g_party = add(make(L"ArisenSalon", tr(L"Ton groupe"), 0, L, y, W, kH - 14 - y, ID_PARTY));

    g_building = PAGE_SALON;
    g_players = add(make(L"ArisenSalon", tr(L"Salon"), 0, L, kTopH + 20, W, kConnY - 12 - kTopH - 20, ID_PLAYERS));
    g_copy_code = add(make(L"BUTTON", tr(L"&Copier le code"), BS_PUSHBUTTON | WS_TABSTOP, L + W - 170 - 18, kConnY + 60, 170, 38, ID_COPY_CODE));
    g_code_edit = add(make(L"EDIT", setting(L"host_code", L"").c_str(), WS_BORDER | WS_TABSTOP | ES_UPPERCASE | ES_AUTOHSCROLL, L + 18,
                           kConnY + 60, 200, 36, ID_CODE_EDIT, g_code_font));
    SendMessageW(g_code_edit, EM_LIMITTEXT, 21, 0);
    SendMessageW(g_code_edit, EM_SETCUEBANNER, TRUE, LPARAM(L"ABC-DEFG"));
    g_code_clear = add(make(L"BUTTON", tr(L"&Effacer"), BS_PUSHBUTTON | WS_TABSTOP, L + 230, kConnY + 60, 110, 36, ID_CODE_CLEAR));

    g_building = PAGE_SAVES;
    const int S = kTopH + 22;
    add(make(L"STATIC",
             tr(L"Une copie de secours de tes sauvegardes de Dragon's Dogma (Steam ou GOG Galaxy) est faite à chaque lancement. Tu peux en faire une tout de suite."),
             0, L, S, W, 44, 0));
    add(make(L"BUTTON", tr(L"Copier mes s&auvegardes"), BS_PUSHBUTTON | WS_TABSTOP, L, S + 50, 230, 40, ID_BACKUP));
    add(make(L"BUTTON", tr(L"&Ouvrir le dossier"), BS_PUSHBUTTON | WS_TABSTOP, L + 242, S + 50, 180, 40, ID_OPEN_BACKUPS));
    g_backup_note = add(make(L"STATIC", L"", SS_LEFT | SS_NOPREFIX | SS_PATHELLIPSIS, L, S + 102, W, 22, 0));
    show_backup_note();
    add(make(L"STATIC", tr(L"Copies de secours (double-clic : ouvrir)"), 0, L, S + 138, W, 22, 0, g_bold));
    g_backup_list = add(make(L"LISTBOX", tr(L"Copies de secours"), WS_VSCROLL | WS_TABSTOP | LBS_NOINTEGRALHEIGHT | LBS_NOTIFY |
                                                                 LBS_OWNERDRAWFIXED | LBS_HASSTRINGS,
                             L + 1, kSavesListY, W - 2, kH - kSavesListY - 28, ID_BACKUP_LIST));
    SetWindowTheme(g_backup_list, L"DarkMode_Explorer", nullptr);
    refresh_backups();

    g_building = PAGE_SETTINGS;
    const int P = kTopH + 20;
    add(make(L"STATIC", tr(L"Version du jeu"), SS_CENTERIMAGE, L, P, 150, 36, 0, g_bold));
    g_store_steam = add(make(L"BUTTON", L"Steam", BS_PUSHBUTTON | WS_TABSTOP | WS_GROUP, L + 160, P, 130, 36, ID_STORE_STEAM));
    g_store_gog = add(make(L"BUTTON", L"GOG", BS_PUSHBUTTON | WS_TABSTOP, L + 300, P, 130, 36, ID_STORE_GOG));
    add(make(L"BUTTON", tr(L"Comment marche la version &GOG ?"), BS_PUSHBUTTON | WS_TABSTOP, L + W - 290, P, 290, 36, ID_GOG_HELP));
    const int G = P + 48;
    add(make(L"STATIC", tr(L"Dossier du jeu"), 0, L, G, W, 22, 0, g_bold));
    g_game_edit = add(make(L"EDIT", g_game_dir.c_str(), WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, L, G + 26, W - 132, 30, ID_GAME_EDIT));
    add(make(L"BUTTON", tr(L"&Changer…"), BS_PUSHBUTTON | WS_TABSTOP, L + W - 120, G + 26, 120, 30, ID_BROWSE));
    g_version = add(make(L"STATIC", L"", 0, L, G + 64, W, 22, 0));
    g_debug = add(make(L"BUTTON", L"", BS_PUSHBUTTON | WS_TABSTOP, L, G + 100, W, 32, ID_DEBUG));
    g_switches.push_back({g_debug, true, tr(L"&Journal détaillé (utile pour signaler un problème)")});
    g_check_updates = add(make(L"BUTTON", L"", BS_PUSHBUTTON | WS_TABSTOP, L, G + 134, W, 32, ID_CHECK_UPDATES));
    g_switches.push_back({g_check_updates, true, tr(L"&Mettre à jour automatiquement au démarrage (GitHub)")});
    g_lan = add(make(L"BUTTON", L"", BS_PUSHBUTTON | WS_TABSTOP, L, kPublicRepo[0] ? G + 168 : G + 134, W, 32, ID_LAN));
    g_switches.push_back({g_lan, false, tr(L"Jouer sans Steam : réseau &local ou IP directe (port UDP 27101)")});
    add(make(L"STATIC", tr(L"Zone sensible"), 0, L + 18, kDangerY + 14, W - 36, 22, 0, g_bold));
    add(make(L"STATIC",
             tr(L"Retire dinput8.dll et coop.ini du dossier du jeu : le jeu redémarre en solo normal. Tes sauvegardes ne sont pas touchées. Une confirmation est demandée."),
             0, L + 18, kDangerY + 40, W - 36 - 200, 44, 0));
    add(make(L"BUTTON", tr(L"&Retirer le mod"), BS_PUSHBUTTON | WS_TABSTOP, L + W - 18 - 180, kDangerY + 38, 180, 40, ID_REMOVE));
    int ay = kDangerY + 96 + 12;
    add(make(L"STATIC", tr(L"À propos"), 0, L, ay, W, 22, 0, g_bold));
    add(make(L"STATIC",
             (L"Arisen Together v" + widen(DDDA_COOP_VERSION) +
              tr(L", par NLRI.\nLe cœur du mod vient de DDDA Coop, de clepix21 (jusqu'à sa version 0.11.0, licence MIT).\nDéveloppé avec l'aide de Claude (Anthropic). MinHook (licence BSD) : voir LICENCES.txt.\nMod non officiel, sans lien avec Capcom ; Dragon's Dogma: Dark Arisen © Capcom."))
                 .c_str(),
             SS_NOPREFIX, L, ay + 26, W - 200, kH - ay - 26 - 6, 0));
    g_project = add(make(L"BUTTON", tr(L"Page du &projet"), BS_PUSHBUTTON | WS_TABSTOP, L + W - 180, ay + 28, 180, 40, ID_PROJECT));

    g_building = PAGE_LOG;
    g_events = add(make(L"LISTBOX", tr(L"Journal"), WS_VSCROLL | WS_TABSTOP | LBS_NOINTEGRALHEIGHT | LBS_NOSEL | LBS_OWNERDRAWFIXED |
                                                   LBS_HASSTRINGS,
                        L + 8, kLogCardY + 6, W - 16, kH - 74 - kLogCardY - 12, ID_EVENTS));
    SetWindowTheme(g_events, L"DarkMode_Explorer", nullptr);
    add(make(L"BUTTON", tr(L"E&xporter le journal"), BS_PUSHBUTTON | WS_TABSTOP, L, kH - 60, 200, 40, ID_EXPORT));

    std::wstring role = setting(L"role", L"host");
    g_role = role == L"guest" ? ROLE_GUEST : role == L"solo" ? ROLE_SOLO : ROLE_HOST;
    g_switches[0].on = setting(L"own_character", L"1") == L"1";
    g_switches[1].on = setting(L"keep_progress", L"1") == L"1";
    g_switches[2].on = setting(L"debug_log", L"1") == L"1";
    g_switches[3].on = setting(L"check_updates", L"1") == L"1";
    g_switches[4].on = setting(L"direct_ip", L"0") == L"1";
    refresh_names();
    update_players();
    show_page(PAGE_PLAY);
}

void layout_events() {}

void paint_topbar(HDC dc)
{
    RECT bar = {0, 0, px(kW), px(kTopH)};
    fill(dc, bar, theme::kTopbar);
    {
        Gdiplus::Graphics g(dc);
        Gdiplus::LinearGradientBrush b(Gdiplus::Point(0, 0), Gdiplus::Point(px(kW), 0), theme::gp(theme::kAccent, 0),
                                       theme::gp(theme::kAccent, 0));
        Gdiplus::Color cols[3] = {theme::gp(theme::kAccent, 0), theme::gp(theme::kAccent, 170), theme::gp(theme::kAccent, 0)};
        Gdiplus::REAL at[3] = {0.f, 0.45f, 1.f};
        b.SetInterpolationColors(cols, at, 3);
        g.FillRectangle(&b, 0, px(kTopH) - 1, px(kW), 1);
    }
    RECT stone = {px(22), px(15), px(22 + 24), px(15 + 34)};
    theme::wakestone(dc, stone);
    HGDIOBJ old = SelectObject(dc, g_wordmark);
    SetTextColor(dc, theme::kAccent);
    SetTextCharacterExtra(dc, px(2));
    RECT t = {px(54), 0, px(kTabX - 6), px(kTopH)};
    DrawTextW(dc, L"ARISEN TOGETHER", -1, &t, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    SetTextCharacterExtra(dc, 0);
    SelectObject(dc, old);
}

void paint(HWND h)
{
    PAINTSTRUCT ps;
    HDC win = BeginPaint(h, &ps);
    RECT rc;
    GetClientRect(h, &rc);
    HDC dc = CreateCompatibleDC(win);
    HBITMAP buf = CreateCompatibleBitmap(win, rc.right, rc.bottom);
    HGDIOBJ old_buf = SelectObject(dc, buf);
    FillRect(dc, &rc, g_bg_brush);
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ old = SelectObject(dc, g_font);
    paint_topbar(dc);
    if (g_page == PAGE_PLAY) {
        std::wstring mod = L"Mod v" + widen(DDDA_COOP_VERSION);
        std::wstring line = g_game_state == 2 ? std::wstring(tr(L"✔  DDDA détecté · ")) + (g_gog ? L"GOG · " : L"Steam · ") + mod
                            : g_game_state == 1 ? tr(L"✖  Version de DDDA.exe non prise en charge · ") + mod
                                                : tr(L"✖  DDDA.exe introuvable : choisis son dossier dans Paramètres · ") + mod;
        SetTextColor(dc, g_game_state == 2 ? theme::kOk : theme::kBad);
        RECT st = {px(kX + 310), px(kTopH + 20), px(kX + kCW), px(kTopH + 46)};
        DrawTextW(dc, line.c_str(), -1, &st, DT_RIGHT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
        theme::round_rect(dc, g_state_rect, px(theme::kRadiusCard), theme::mix(kCard, g_state_color, 0.08f),
                          theme::mix(theme::kBorder, g_state_color, 0.35f));
        RECT bar = {g_state_rect.left + px(12), g_state_rect.top + px(14), g_state_rect.left + px(16), g_state_rect.bottom - px(14)};
        theme::round_rect(dc, bar, px(2), g_state_color);
        if (g_role != ROLE_GUEST) {
            int y = px(kRoleY + kRoleH + 43), cx = px(kX + kCW / 2), d = px(4);
            Gdiplus::Graphics g(dc);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            Gdiplus::LinearGradientBrush b(Gdiplus::Point(px(kX + 120), 0), Gdiplus::Point(px(kX + kCW - 120), 0), theme::gp(theme::kAccent, 0),
                                           theme::gp(theme::kAccent, 0));
            Gdiplus::Color cols[3] = {theme::gp(theme::kAccent, 0), theme::gp(theme::kAccent, 110), theme::gp(theme::kAccent, 0)};
            Gdiplus::REAL at[3] = {0.f, 0.5f, 1.f};
            b.SetInterpolationColors(cols, at, 3);
            g.FillRectangle(&b, px(kX + 120), y, px(kCW - 240), 1);
            Gdiplus::PointF dia[4] = {Gdiplus::PointF(float(cx), float(y - d)), Gdiplus::PointF(float(cx + d), float(y)),
                                      Gdiplus::PointF(float(cx), float(y + d + 1)), Gdiplus::PointF(float(cx - d), float(y))};
            Gdiplus::SolidBrush gold(theme::gp(theme::kAccent, 200));
            g.FillPolygon(&gold, dia, 4);
        }
    }
    if (g_page == PAGE_SALON) {
        RECT card = {px(kX), px(kConnY), px(kX + kCW), px(kConnY + kConnH)};
        theme::round_rect(dc, card, px(theme::kRadiusCard), kCard, theme::kBorder);
        SelectObject(dc, g_head);
        SetTextColor(dc, kText);
        RECT t = {card.left + px(18), card.top + px(10), card.right - px(18), card.top + px(38)};
        DrawTextW(dc, tr(L"Connexion"), -1, &t, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        SelectObject(dc, g_font);
        SetTextColor(dc, kMuted);
        RECT hint = {card.left + px(18), card.top + px(38), card.right - px(18), card.top + px(56)};
        RECT code = {card.left + px(18), px(kConnY + 60), card.right - px(220), px(kConnY + 98)};
        RECT v = {px(kX + 356), px(kConnY + 60), card.right - px(18), px(kConnY + 96)};
        if (g_role == ROLE_HOST && lan_mode()) {
            DrawTextW(dc, tr(L"Donne cette adresse à tes amis (réseau local, ou ton IP publique avec le port UDP 27101 ouvert)."), -1,
                      &hint, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
            SelectObject(dc, g_code_font);
            SetTextColor(dc, kText);
            std::vector<std::wstring> ips = local_addresses();
            std::wstring c = ips.empty() ? L"–" : ips[0];
            for (size_t k = 1; k < ips.size() && k < 3; ++k)
                c += L"  " + ips[k];
            DrawTextW(dc, c.c_str(), -1, &code, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        } else if (g_role == ROLE_HOST) {
            DrawTextW(dc, tr(L"Donne ce code à tes amis : ils le collent dans leur salon pour te rejoindre toi précisément."), -1,
                      &hint, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
            SelectObject(dc, g_code_font);
            SetTextColor(dc, g_self_steam ? theme::kAccent : kMuted);
            std::wstring c = g_self_steam ? code_from_steam(g_self_steam) : L"–––-––––";
            DrawTextW(dc, c.c_str(), -1, &code, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
            if (!g_self_steam) {
                SelectObject(dc, g_small);
                SetTextColor(dc, kMuted);
                RECT why = {code.left + px(150), code.top, code.right, code.bottom};
                DrawTextW(dc, tr(L"lance le jeu en hôte (Steam) pour l'obtenir"), -1, &why, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
            }
        } else if (g_role == ROLE_GUEST && lan_mode()) {
            DrawTextW(dc, tr(L"L'adresse de ton hôte (par exemple 192.168.1.20, ou 192.168.1.20:27101)."), -1, &hint,
                      DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
            std::wstring txt = window_text(g_code_edit);
            std::string ip;
            int port = 0;
            bool ok = parse_address(txt, ip, port);
            SetTextColor(dc, ok ? kOk : kBad);
            DrawTextW(dc, ok ? tr(L"✔  adresse valide") : tr(L"✖  adresse manquante ou invalide"), -1, &v,
                      DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        } else if (g_role == ROLE_GUEST) {
            DrawTextW(dc, tr(L"Le code de ton hôte pour le rejoindre lui précisément ; vide : le premier ami Steam qui héberge."),
                      -1, &hint, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
            std::wstring txt = window_text(g_code_edit);
            uint64_t id = 0;
            bool ok = steam_from_code(txt, id);
            SetTextColor(dc, txt.empty() ? kMuted : ok ? kOk : kBad);
            DrawTextW(dc, txt.empty() ? tr(L"●  recherche automatique") : ok ? tr(L"✔  code valide") : tr(L"✖  code incomplet ou invalide"),
                      -1, &v, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        } else {
            DrawTextW(dc, tr(L"Seul : pas de connexion."), -1, &hint, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        }
    }
    if (g_page == PAGE_SETTINGS) {
        RECT card = {px(kX), px(kDangerY), px(kX + kCW), px(kDangerY + 96)};
        theme::round_rect(dc, card, px(theme::kRadiusCard), theme::mix(kCard, kBad, 0.05f), theme::mix(theme::kBorder, kBad, 0.55f));
    }
    if (g_page == PAGE_SAVES) {
        RECT card = {px(kX - 1), px(kSavesListY - 4), px(kX + kCW + 1), px(kH - 24)};
        theme::round_rect(dc, card, px(theme::kRadiusSmall + 2), kCard, theme::kBorder);
        if (g_backup_dirs.empty()) {
            SelectObject(dc, g_font);
            SetTextColor(dc, kMuted);
            RECT e = card;
            DrawTextW(dc, tr(L"Aucune copie pour l'instant."), -1, &e, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        }
    }
    if (g_page == PAGE_LOG) {
        int n = int(SendMessageW(g_events, LB_GETCOUNT, 0, 0)), warn = 0, bad = 0;
        for (int i = 0; i < n; ++i) {
            int kind = int(SendMessageW(g_events, LB_GETITEMDATA, i, 0)) & 3;
            warn += kind == 2;
            bad += kind == 3;
        }
        SelectObject(dc, g_font);
        std::wstring counts = std::to_wstring(n) + (plural(n) ? tr(L" événements") : tr(L" événement"));
        SetTextColor(dc, kMuted);
        RECT c = {px(kX), px(kTopH + 18), px(kX + kCW), px(kTopH + 42)};
        DrawTextW(dc, counts.c_str(), -1, &c, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        SIZE cs = {};
        GetTextExtentPoint32W(dc, counts.c_str(), int(counts.size()), &cs);
        c.left += cs.cx + px(16);
        std::wstring w = L"⚠ " + std::to_wstring(warn) + (plural(warn) ? tr(L" avertissements") : tr(L" avertissement"));
        SetTextColor(dc, warn ? kWarn : kMuted);
        DrawTextW(dc, w.c_str(), -1, &c, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        GetTextExtentPoint32W(dc, w.c_str(), int(w.size()), &cs);
        c.left += cs.cx + px(16);
        std::wstring e = L"✖ " + std::to_wstring(bad) + (plural(bad) ? tr(L" erreurs") : tr(L" erreur"));
        SetTextColor(dc, bad ? kBad : kMuted);
        DrawTextW(dc, e.c_str(), -1, &c, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        RECT card = {px(kX), px(kLogCardY), px(kX + kCW), px(kH - 74)};
        theme::round_rect(dc, card, px(theme::kRadiusCard), kCard, theme::kBorder);
        if (n == 0) {
            SetTextColor(dc, kMuted);
            RECT ec = card;
            DrawTextW(dc, tr(L"Rien pour l'instant : les événements arrivent au lancement du jeu."), -1, &ec,
                      DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        }
    }
    SelectObject(dc, old);
    BitBlt(win, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, old_buf);
    DeleteObject(buf);
    DeleteDC(dc);
    EndPaint(h, &ps);
}

void pick_language(HWND h)
{
    RECT r;
    GetWindowRect(GetDlgItem(h, ID_LANG), &r);
    HMENU menu = CreatePopupMenu();
    for (int k = 0; k < LANG_COUNT; ++k)
        AppendMenuW(menu, MF_STRING | (k == g_lang ? MF_CHECKED : 0), UINT_PTR(1 + k), kLangNames[k]);
    int pick = int(TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTALIGN | TPM_TOPALIGN | TPM_NONOTIFY, r.right, r.bottom, 0, h, nullptr));
    DestroyMenu(menu);
    if (pick < 1 || pick - 1 == g_lang)
        return;
    WritePrivateProfileStringW(L"launcher", L"lang", kLangCodes[pick - 1], settings_path().c_str());
    restart_launcher(h);
}

LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_CREATE:
        g_wnd = h;
        build_window();
        SetTimer(h, ID_TIMER, 1000, nullptr);
        ShowWindow(g_update, SW_HIDE);
        if (switch_on(g_check_updates))
            if (HANDLE t = CreateThread(nullptr, 0, check_updates, nullptr, 0, nullptr))
                CloseHandle(t);
        if (setting(L"help_seen", L"0") != L"1") {
            WritePrivateProfileStringW(L"launcher", L"help_seen", L"1", settings_path().c_str());
            PostMessageW(h, WM_COMMAND, ID_HELP, 0);
        }
        if (g_start_page > 0 && g_start_page < PAGE_COUNT)
            show_page(g_start_page);
        return 0;
    case WM_UPDATE_FOUND:
        SetWindowTextW(g_update, (tr(L"Installer la mise à jour ") + g_update_tag + L"  ›").c_str());
        if (g_page == PAGE_PLAY)
            ShowWindow(g_update, SW_SHOW);
        add_event(tr(L"Une nouvelle version du mod est disponible : ") + g_update_tag + tr(L" (bouton en haut de « Jouer »)."));
        if (!g_test_releases.empty()) {
            add_event(L"test: zip " + (g_update_url.empty() ? std::wstring(L"(none)") : g_update_url) + L" sha256 " + widen(g_update_sha));
            return 0;
        }
        if (!game_running())
            start_update();
        return 0;
    case WM_UPDATE_DONE:
        g_updating = false;
        if (w) {
            add_event(tr(L"Mise à jour installée : Arisen Together redémarre."));
            restart_launcher(h);
            return 0;
        }
        add_event(tr(L"Échec de la mise à jour (") + g_update_error + tr(L") : le bouton ouvre la page de la version sur GitHub."));
        g_update_url.clear();
        g_test_update_zip.clear();
        EnableWindow(g_update, TRUE);
        SetWindowTextW(g_update, (tr(L"Mise à jour ") + g_update_tag + tr(L" disponible  ›")).c_str());
        return 0;
    case WM_MEASUREITEM:
        if (w == ID_BACKUP_LIST) {
            reinterpret_cast<MEASUREITEMSTRUCT *>(l)->itemHeight = UINT(px(30));
            return TRUE;
        }
        if (w == ID_EVENTS) {
            reinterpret_cast<MEASUREITEMSTRUCT *>(l)->itemHeight = UINT(px(20));
            return TRUE;
        }
        break;
    case WM_DRAWITEM:
        if (w == ID_BACKUP_LIST) {
            draw_backup(reinterpret_cast<const DRAWITEMSTRUCT *>(l));
            return TRUE;
        }
        if (w == ID_EVENTS) {
            draw_event(reinterpret_cast<const DRAWITEMSTRUCT *>(l));
            return TRUE;
        }
        if (reinterpret_cast<const DRAWITEMSTRUCT *>(l)->CtlType == ODT_BUTTON) {
            draw_button_buffered(reinterpret_cast<const DRAWITEMSTRUCT *>(l));
            return TRUE;
        }
        break;
    case DM_GETDEFID:
        return MAKELRESULT(ID_LAUNCH, DC_HASDEFID);
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        SetTextColor(HDC(w), kText);
        SetBkColor(HDC(w), kCard);
        return LRESULT(g_card_brush);
    case WM_PAINT:
        paint(h);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_COMMAND:
        if (LOWORD(w) >= ID_NAV && LOWORD(w) < ID_NAV + PAGE_COUNT) {
            show_page(LOWORD(w) - ID_NAV);
            return 0;
        }
        switch (LOWORD(w)) {
        case ID_BROWSE: on_browse(); break;
        case ID_HOST:
        case ID_GUEST:
        case ID_SOLO:
            g_role = LOWORD(w) == ID_GUEST ? ROLE_GUEST : LOWORD(w) == ID_SOLO ? ROLE_SOLO : ROLE_HOST;
            refresh_names();
            save_settings();
            InvalidateRect(h, nullptr, FALSE);
            break;
        case ID_KEEP:
        case ID_OWN_CHAR:
        case ID_DEBUG:
        case ID_CHECK_UPDATES:
        case ID_LAN:
            for (Switch &sw : g_switches)
                if (sw.h == HWND(l))
                    sw.on = !sw.on;
            refresh_names();
            save_settings();
            break;
        case ID_LAUNCH: on_launch(); break;
        case ID_REMOVE: on_remove(); break;
        case ID_BACKUP: on_backup(); break;
        case ID_HELP: show_help(); break;
        case ID_GOG_HELP: show_gog_help(); break;
        case ID_UNSUPPORTED_HELP: show_unsupported_help(); break;
        case ID_STORE_STEAM: pick_store(0); break;
        case ID_STORE_GOG: pick_store(1); break;
        case ID_LANG: pick_language(h); break;
        case ID_UPDATE:
            if (!g_update_url.empty() || !g_test_update_zip.empty()) {
                if (game_running())
                    ask(tr(L"Ferme le jeu avant d'installer la mise à jour."), MB_ICONINFORMATION);
                else
                    start_update();
            } else
                ShellExecuteW(h, L"open", (std::wstring(kRepoUrl) + L"/releases/tag/" + g_update_tag).c_str(), nullptr, nullptr,
                              SW_SHOWNORMAL);
            break;
        case ID_PROJECT:
            if (kPublicRepo[0])
                ShellExecuteW(h, L"open", kRepoUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            break;
        case ID_COPY_CODE:
            if ((lan_mode() || g_self_steam) && OpenClipboard(h)) {
                std::vector<std::wstring> ips = local_addresses();
                std::wstring c = lan_mode() ? (ips.empty() ? std::wstring() : ips[0]) : code_from_steam(g_self_steam);
                EmptyClipboard();
                if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (c.size() + 1) * sizeof(wchar_t))) {
                    memcpy(GlobalLock(mem), c.c_str(), (c.size() + 1) * sizeof(wchar_t));
                    GlobalUnlock(mem);
                    SetClipboardData(CF_UNICODETEXT, mem);
                }
                CloseClipboard();
                add_event(tr(L"Code de connexion copié : ") + c);
            }
            break;
        case ID_CODE_CLEAR:
            SetWindowTextW(g_code_edit, L"");
            break;
        case ID_CODE_EDIT:
            if (HIWORD(w) == EN_CHANGE) {
                WritePrivateProfileStringW(L"launcher", L"host_code", window_text(g_code_edit).c_str(), settings_path().c_str());
                RECT card = {px(kX), px(384), px(kX + kCW), px(500)};
                InvalidateRect(h, &card, FALSE);
            }
            break;
        case ID_OPEN_BACKUPS: {
            int i = int(SendMessageW(g_backup_list, LB_GETCURSEL, 0, 0));
            std::wstring dir = i >= 0 && i < int(g_backup_dirs.size()) ? g_backup_dirs[size_t(i)] : documents_dir() + L"\\Arisen Together\\Backups";
            if (!is_dir(dir))
                dir = documents_dir() + L"\\Arisen Together";
            ShellExecuteW(g_wnd, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            break;
        }
        case ID_BACKUP_LIST:
            if (HIWORD(w) == LBN_DBLCLK)
                SendMessageW(h, WM_COMMAND, ID_OPEN_BACKUPS, 0);
            break;
        case ID_EXPORT: on_export(); break;
        case ID_GAME_EDIT:
            if (HIWORD(w) == EN_KILLFOCUS && window_text(g_game_edit) != g_game_dir) {
                check_game();
                save_settings();
            }
            break;
        }
        return 0;
    case WM_TIMER:
        if (w == kHoverTimer) {
            hover_step(h);
            return 0;
        }
        poll_log();
        check_watchers();
        update_players();
        return 0;
    case WM_CTLCOLORSTATIC:
        if (HWND(l) == g_state) {
            SetTextColor(HDC(w), g_state_color);
            SetBkColor(HDC(w), theme::mix(theme::kSurface, g_state_color, 0.08f));
            return LRESULT(g_state_brush);
        }
        if (g_page == PAGE_SETTINGS) {
            RECT rc;
            GetWindowRect(HWND(l), &rc);
            MapWindowPoints(nullptr, h, reinterpret_cast<POINT *>(&rc), 2);
            if (rc.top >= px(kDangerY) && rc.bottom <= px(kDangerY + 96)) {
                static HBRUSH danger = CreateSolidBrush(theme::mix(theme::kSurface, theme::kBad, 0.05f));
                SetTextColor(HDC(w), rc.top < px(kDangerY + 36) ? kText : kMuted);
                SetBkColor(HDC(w), theme::mix(theme::kSurface, theme::kBad, 0.05f));
                return LRESULT(danger);
            }
        }
        SetTextColor(HDC(w), HWND(l) == g_version || HWND(l) == g_backup_note ? kMuted : kText);
        SetBkColor(HDC(w), kBg);
        return LRESULT(g_bg_brush);
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

}

int CALLBACK font_found(const LOGFONTW *, const TEXTMETRICW *, DWORD, LPARAM l)
{
    *reinterpret_cast<bool *>(l) = true;
    return 0;
}

bool font_exists(const wchar_t *face)
{
    LOGFONTW want = {};
    want.lfCharSet = DEFAULT_CHARSET;
    wcsncpy(want.lfFaceName, face, LF_FACESIZE - 1);
    bool found = false;
    HDC dc = GetDC(nullptr);
    EnumFontFamiliesExW(dc, &want, font_found, LPARAM(&found), 0);
    ReleaseDC(nullptr, dc);
    return found;
}

const wchar_t *serif_family()
{
    for (const wchar_t *face : theme::kSerifFonts)
        if (font_exists(face))
            return face;
    return theme::kFontFallback;
}

const wchar_t *font_family()
{
    static const wchar_t *chosen = nullptr;
    if (chosen)
        return chosen;
    chosen = font_exists(theme::kFontFamily) ? theme::kFontFamily : theme::kFontFallback;
    return chosen;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show)
{
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Gdiplus::GdiplusStartupInput gsi;
    ULONG_PTR gdiplus = 0;
    Gdiplus::GdiplusStartup(&gdiplus, &gsi, nullptr);
    INITCOMMONCONTROLSEX icc = {sizeof icc, ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES};
    InitCommonControlsEx(&icc);

    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    g_exe_dir = exe;
    g_exe_dir.resize(g_exe_dir.rfind(L'\\'));
    for (int k = 0; k < 30 && exists(g_exe_dir + L"\\ArisenTogether.exe.old") && !DeleteFileW((g_exe_dir + L"\\ArisenTogether.exe.old").c_str()); ++k)
        Sleep(100);

    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring game_arg, steam_arg;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--game" && i + 1 < argc)
            game_arg = argv[++i];
        else if (a == L"--steam" && i + 1 < argc)
            steam_arg = argv[++i];
        else if (a == L"--gog" && i + 1 < argc)
            g_gog_arg = argv[++i];
        else if (a == L"--gog-storage" && i + 1 < argc)
            g_gog_storage_arg = argv[++i];
        else if (a == L"--docs" && i + 1 < argc)
            g_docs_dir = argv[++i];
        else if (a == L"--no-launch")
            g_no_launch = true;
        else if (a == L"--test-update" && i + 1 < argc)
            g_test_update = argv[++i];
        else if (a == L"--test-update-zip" && i + 2 < argc) {
            g_test_update_zip = argv[++i];
            std::wstring sha = argv[++i];
            g_test_update_sha.assign(sha.begin(), sha.end());
        }
        else if (a == L"--page" && i + 1 < argc)
            g_start_page = _wtoi(argv[++i]);
        else if (a == L"--demo-party")
            g_demo_party = true;
        else if (a == L"--test-releases" && i + 1 < argc)
            g_test_releases = argv[++i];
    }
    LocalFree(argv);
    std::wstring lang = setting(L"lang", L"");
    g_lang = -1;
    for (int k = 0; k < LANG_COUNT; ++k)
        if (_wcsicmp(lang.c_str(), kLangCodes[k]) == 0)
            g_lang = k;
    if (g_lang < 0)
        switch (PRIMARYLANGID(GetUserDefaultUILanguage())) {
        case LANG_FRENCH: g_lang = LANG_FR; break;
        case LANG_ITALIAN: g_lang = LANG_IT; break;
        case LANG_GERMAN: g_lang = LANG_DE; break;
        case LANG_SPANISH: g_lang = LANG_ES; break;
        default: g_lang = LANG_EN; break;
        }
    g_steam_dir = steam_arg.empty() ? steam_dir() : steam_arg;
    g_game_dir = !game_arg.empty() ? game_arg : setting(L"game", L"");
    g_steam_game = find_game(g_steam_dir);
    g_gog_game = find_gog_game();
    if (g_game_dir.empty())
        g_game_dir = !g_steam_game.empty() ? g_steam_game : g_gog_game;

    HDC screen = GetDC(nullptr);
    g_dpi = GetDeviceCaps(screen, LOGPIXELSY);
    ReleaseDC(nullptr, screen);
    auto font = [](const wchar_t *face, int pt, int weight) {
        LOGFONTW f = {};
        f.lfHeight = -MulDiv(pt, g_dpi, 72);
        f.lfWeight = weight;
        f.lfQuality = CLEARTYPE_QUALITY;
        wcsncpy(f.lfFaceName, face, LF_FACESIZE - 1);
        return CreateFontIndirectW(&f);
    };
    const wchar_t *sans = font_family(), *serif = serif_family();
    g_font = font(sans, theme::kSizeBody, FW_NORMAL);
    g_bold = font(sans, theme::kSizeBody, FW_SEMIBOLD);
    g_small = font(sans, theme::kSizeSmall, FW_NORMAL);
    g_head = font(serif, theme::kSizeTitle, FW_BOLD);
    g_tab = font(serif, theme::kSizeTab, FW_NORMAL);
    g_wordmark = font(serif, theme::kSizeWordmark, FW_BOLD);
    {
        LOGFONTW f = {};
        f.lfHeight = -MulDiv(14, g_dpi, 72);
        f.lfQuality = CLEARTYPE_QUALITY;
        wcscpy(f.lfFaceName, L"Segoe MDL2 Assets");
        g_icon = CreateFontIndirectW(&f);
        f.lfHeight = -MulDiv(10, g_dpi, 72);
        g_small_icon = CreateFontIndirectW(&f);
        LOGFONTW c = {};
        c.lfHeight = -MulDiv(16, g_dpi, 72);
        c.lfWeight = FW_SEMIBOLD;
        c.lfQuality = CLEARTYPE_QUALITY;
        wcscpy(c.lfFaceName, L"Consolas");
        g_code_font = CreateFontIndirectW(&c);
    }
    g_bg_brush = CreateSolidBrush(kBg);
    g_card_brush = CreateSolidBrush(kCard);

    WNDCLASSW sc = {};
    sc.lpfnWndProc = salon_proc;
    sc.hInstance = inst;
    sc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    sc.lpszClassName = L"ArisenSalon";
    RegisterClassW(&sc);
    WNDCLASSW wc = {};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"ArisenLauncher";
    RegisterClassW(&wc);
    RECT r = {0, 0, MulDiv(kW, g_dpi, 96), MulDiv(kH, g_dpi, 96)};
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    AdjustWindowRect(&r, style, FALSE);
    HWND h = CreateWindowW(wc.lpszClassName, kTitle, style, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left,
                           r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    RECT wr;
    GetWindowRect(h, &wr);
    RECT need = {0, 0, MulDiv(kW, g_dpi, 96), MulDiv(g_height, g_dpi, 96)};
    AdjustWindowRect(&need, style, FALSE);
    SetWindowPos(h, nullptr, 0, 0, need.right - need.left, need.bottom - need.top, SWP_NOMOVE | SWP_NOZORDER);
    if (HANDLE small = LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                  GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR))
        SendMessageW(h, WM_SETICON, ICON_SMALL, LPARAM(small));
    BOOL dark = TRUE;
    DwmSetWindowAttribute(h, 20, &dark, sizeof dark);
    COLORREF border = theme::kBorder, caption = theme::kTopbar, caption_text = theme::kText;
    DwmSetWindowAttribute(h, 34, &border, sizeof border);
    DwmSetWindowAttribute(h, 35, &caption, sizeof caption);
    DwmSetWindowAttribute(h, 36, &caption_text, sizeof caption_text);
    SendMessageW(h, WM_CHANGEUISTATE, MAKEWPARAM(UIS_SET, UISF_HIDEACCEL | UISF_HIDEFOCUS), 0);
    ShowWindow(h, show);
    check_game();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (msg.message == WM_KEYDOWN && (GetKeyState(VK_CONTROL) & 0x8000)) {
            int to = -1;
            if (msg.wParam >= '1' && msg.wParam < '1' + PAGE_COUNT)
                to = int(msg.wParam - '1');
            else if (msg.wParam == VK_TAB)
                to = (g_page + ((GetKeyState(VK_SHIFT) & 0x8000) ? PAGE_COUNT - 1 : 1)) % PAGE_COUNT;
            if (to >= 0) {
                show_page(to);
                SetFocus(GetDlgItem(h, ID_NAV + to));
                continue;
            }
        }
        if (IsDialogMessageW(h, &msg))
            continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
