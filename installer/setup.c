/* Webbed Co-op installer: a plain native program, no packer and no temporary unpacking.
   The payload (mod DLL, data.win delta, Python runtime for the network helper) is appended
   to this exe by tools/pack.py. Run it again to remove the mod.

   Usage: Webbed-Coop-Setup.exe [--install|--uninstall] [--quiet] [--game "<Webbed folder>"] */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <shlobj.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define WEBBED_APP L"1390350"
#define PORTHOLE_APP L"4963920"
static const char *EXE_SHA = "c63cecb63ed471bc68b2e5a0f90c4a0cf15a57266b7af1e2d80a0b04d0116dde";
static const char *DATA_SHA = "6caf485d6027afa3270ef0ab19595b95e137188168d1244a4a5c05bf9e85398c";
static const char *PATCHED_SHA = "305e6fe920715e78668502c2440846abc0a604749edcaef70421be8243c8d701";
static const wchar_t *TITLE = L"Webbed • Кооператив";

static int quiet;
static wchar_t failure[600];

static void fail(const wchar_t *text)
{
    if (!failure[0])
        wcsncpy(failure, text, 599);
}
static int box(const wchar_t *text, UINT style)
{
    return quiet ? IDOK : MessageBoxW(NULL, text, TITLE, style | MB_SETFOREGROUND);
}

/* ---------- files and hashes ---------- */
static uint8_t *read_all(const wchar_t *path, DWORD *size)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return NULL;
    LARGE_INTEGER n;
    uint8_t *data = NULL;
    if (GetFileSizeEx(f, &n) && n.QuadPart < 0x40000000 && (data = malloc((size_t)n.QuadPart + 1)))
    {
        DWORD got = 0, total = 0;
        while (total < n.QuadPart && ReadFile(f, data + total, (DWORD)n.QuadPart - total, &got, NULL) && got)
            total += got;
        if (total != n.QuadPart)
        {
            free(data);
            data = NULL;
        }
        else
        {
            data[total] = 0;
            *size = total;
        }
    }
    CloseHandle(f);
    return data;
}
static int write_all(const wchar_t *path, const void *data, DWORD size)
{
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return 0;
    DWORD put = 0;
    int ok = WriteFile(f, data, size, &put, NULL) && put == size;
    ok = FlushFileBuffers(f) && ok;
    CloseHandle(f);
    return ok;
}
static void sha256_hex(const void *data, DWORD size, char out[65])
{
    uint8_t digest[32] = {0};
    BCRYPT_ALG_HANDLE alg;
    BCRYPT_HASH_HANDLE hash;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) == 0)
    {
        if (BCryptCreateHash(alg, &hash, NULL, 0, NULL, 0, 0) == 0)
        {
            BCryptHashData(hash, (PUCHAR)data, size, 0);
            BCryptFinishHash(hash, digest, 32, 0);
            BCryptDestroyHash(hash);
        }
        BCryptCloseAlgorithmProvider(alg, 0);
    }
    for (int i = 0; i < 32; i++)
        sprintf(out + 2 * i, "%02x", digest[i]);
}
static int file_sha(const wchar_t *path, char out[65])
{
    DWORD n;
    uint8_t *d = read_all(path, &n);
    if (!d)
        return 0;
    sha256_hex(d, n, out);
    free(d);
    return 1;
}
static int exists(const wchar_t *path)
{
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}
static void make_dirs(const wchar_t *file)
{
    wchar_t dir[MAX_PATH];
    wcsncpy(dir, file, MAX_PATH - 1);
    dir[MAX_PATH - 1] = 0;
    for (wchar_t *p = dir + 3; *p; p++)
        if (*p == L'\\')
        {
            *p = 0;
            CreateDirectoryW(dir, NULL);
            *p = L'\\';
        }
}

/* ---------- finding Steam games ---------- */
static wchar_t libraries[16][MAX_PATH];
static int libraryCount;
static void add_library(const wchar_t *path)
{
    if (libraryCount >= 16 || !path[0])
        return;
    for (int i = 0; i < libraryCount; i++)
        if (!_wcsicmp(libraries[i], path))
            return;
    wcsncpy(libraries[libraryCount++], path, MAX_PATH - 1);
}
static void find_libraries(void)
{
    libraryCount = 0;
    wchar_t steam[MAX_PATH] = L"";
    DWORD size = sizeof(steam);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, NULL, steam, &size) ==
        ERROR_SUCCESS)
    {
        for (wchar_t *p = steam; *p; p++)
            if (*p == L'/')
                *p = L'\\';
        add_library(steam);
    }
    wchar_t pf[MAX_PATH];
    if (GetEnvironmentVariableW(L"ProgramFiles(x86)", pf, MAX_PATH))
    {
        wcscat(pf, L"\\Steam");
        add_library(pf);
    }
    for (int i = 0, roots = libraryCount; i < roots; i++)
    {
        wchar_t vdf[MAX_PATH];
        swprintf(vdf, MAX_PATH, L"%ls\\steamapps\\libraryfolders.vdf", libraries[i]);
        DWORD n;
        char *text = (char *)read_all(vdf, &n);
        if (!text)
            continue;
        for (char *p = text; (p = strstr(p, "\"path\"")); p += 6)
        {
            char *a = strchr(p + 6, '"'), value[MAX_PATH * 2];
            if (!a)
                break;
            int j = 0;
            for (char *c = a + 1; *c && *c != '"' && j < (int)sizeof(value) - 1; c++)
            {
                if (*c == '\\' && c[1] == '\\')
                    c++;
                value[j++] = *c;
            }
            value[j] = 0;
            wchar_t wide[MAX_PATH];
            if (MultiByteToWideChar(CP_UTF8, 0, value, -1, wide, MAX_PATH))
                add_library(wide);
        }
        free(text);
    }
}
static int find_app(const wchar_t *folder, const wchar_t *exe, wchar_t *out)
{
    find_libraries();
    for (int i = 0; i < libraryCount; i++)
    {
        wchar_t path[MAX_PATH];
        swprintf(path, MAX_PATH, L"%ls\\steamapps\\common\\%ls\\%ls", libraries[i], folder, exe);
        if (exists(path))
        {
            swprintf(out, MAX_PATH, L"%ls\\steamapps\\common\\%ls", libraries[i], folder);
            return 1;
        }
    }
    return 0;
}
static void open_steam(const wchar_t *what, const wchar_t *app)
{
    wchar_t url[80];
    swprintf(url, 80, L"steam://%ls/%ls", what, app);
    ShellExecuteW(NULL, L"open", url, NULL, NULL, SW_SHOWNORMAL);
}

/* ---------- payload appended by tools/pack.py ---------- */
typedef struct
{
    char name[260];
    const uint8_t *data;
    uint32_t size;
} Entry;
static uint8_t *payload;
static Entry entries[400];
static int entryCount;
static int load_payload(void)
{
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(NULL, self, MAX_PATH);
    DWORD n;
    uint8_t *exe = read_all(self, &n);
    if (!exe || n < 16 || memcmp(exe + n - 8, "WBCOOP02", 8))
        return 0;
    uint64_t start;
    memcpy(&start, exe + n - 16, 8);
    const uint8_t *p = exe + start, *end = exe + n - 16;
    while (p + 8 <= end && entryCount < 400)
    {
        uint32_t nameLen, size;
        memcpy(&nameLen, p, 4);
        p += 4;
        if (nameLen >= 260 || p + nameLen + 4 > end)
            return 0;
        Entry *e = &entries[entryCount++];
        memcpy(e->name, p, nameLen);
        e->name[nameLen] = 0;
        p += nameLen;
        memcpy(&size, p, 4);
        p += 4;
        if (p + size > end)
            return 0;
        e->data = p;
        e->size = size;
        p += size;
    }
    payload = exe;
    return entryCount > 0;
}
static const Entry *entry(const char *name)
{
    for (int i = 0; i < entryCount; i++)
        if (!strcmp(entries[i].name, name))
            return &entries[i];
    return NULL;
}

/* data.win = original chunks, each XORed with stored runs of difference (see tools/pack.py). */
static uint8_t *patched_data(const uint8_t *src, DWORD srcSize, DWORD *outSize)
{
    const Entry *delta = entry("data.delta");
    if (!delta || srcSize < 8 || memcmp(src, "FORM", 4))
        return NULL;
    const uint8_t *dend = delta->data + delta->size;
    uint32_t count, total = 0;
    memcpy(&count, delta->data, 4);
    /* First pass: output size, and check the delta is well formed. */
    const uint8_t *d = delta->data + 4;
    for (uint32_t i = 0; i < count; i++)
    {
        uint32_t newSize, runs;
        if (d + 16 > dend)
            return NULL;
        memcpy(&newSize, d + 8, 4);
        memcpy(&runs, d + 12, 4);
        d += 16;
        for (uint32_t r = 0; r < runs; r++)
        {
            uint32_t at, len;
            if (d + 8 > dend)
                return NULL;
            memcpy(&at, d, 4);
            memcpy(&len, d + 4, 4);
            if ((uint64_t)at + len > newSize || d + 8 + len > dend)
                return NULL;
            d += 8 + len;
        }
        total += 8 + newSize;
    }
    uint8_t *out = calloc(1, (size_t)total + 8);
    if (!out)
        return NULL;
    memcpy(out, "FORM", 4);
    memcpy(out + 4, &total, 4);
    const uint8_t *s = src + 8;
    uint8_t *o = out + 8;
    d = delta->data + 4;
    for (uint32_t i = 0; i < count; i++)
    {
        uint32_t oldSize, newSize, runs;
        memcpy(&oldSize, d + 4, 4);
        memcpy(&newSize, d + 8, 4);
        memcpy(&runs, d + 12, 4);
        if (s + 8 + oldSize > src + srcSize || memcmp(s, d, 4) || memcmp(s + 4, &oldSize, 4))
        {
            free(out);
            return NULL;
        }
        memcpy(o, d, 4);
        memcpy(o + 4, &newSize, 4);
        memcpy(o + 8, s + 8, oldSize < newSize ? oldSize : newSize);
        d += 16;
        for (uint32_t r = 0; r < runs; r++)
        {
            uint32_t at, len;
            memcpy(&at, d, 4);
            memcpy(&len, d + 4, 4);
            for (uint32_t k = 0; k < len; k++)
                o[8 + at + k] ^= d[8 + k];
            d += 8 + len;
        }
        s += 8 + oldSize;
        o += 8 + newSize;
    }
    *outSize = total + 8;
    return out;
}

/* ---------- install / remove ---------- */
static wchar_t game[MAX_PATH];
static void game_path(wchar_t *out, const wchar_t *rel)
{
    swprintf(out, MAX_PATH, L"%ls\\%ls", game, rel);
}
static int replace_file(const wchar_t *target, const void *data, DWORD size)
{
    wchar_t temp[MAX_PATH];
    swprintf(temp, MAX_PATH, L"%ls.coop-new", target);
    make_dirs(target);
    if (!write_all(temp, data, size))
        return 0;
    if (!MoveFileExW(temp, target, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        DeleteFileW(temp);
        return 0;
    }
    return 1;
}
/* Removes the runtime folder the mod owns, then the files it put next to the game. */
static void remove_tree(const wchar_t *dir)
{
    wchar_t pattern[MAX_PATH];
    swprintf(pattern, MAX_PATH, L"%ls\\*", dir);
    WIN32_FIND_DATAW f;
    HANDLE h = FindFirstFileW(pattern, &f);
    if (h != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (!wcscmp(f.cFileName, L".") || !wcscmp(f.cFileName, L".."))
                continue;
            wchar_t child[MAX_PATH];
            swprintf(child, MAX_PATH, L"%ls\\%ls", dir, f.cFileName);
            if (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                remove_tree(child);
            else
            {
                SetFileAttributesW(child, FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(child);
            }
        } while (FindNextFileW(h, &f));
        FindClose(h);
    }
    RemoveDirectoryW(dir);
}
static int writable(void)
{
    wchar_t probe[MAX_PATH];
    game_path(probe, L"WebbedCoop.write-test");
    HANDLE f = CreateFileW(probe, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return GetLastError() != ERROR_ACCESS_DENIED;
    CloseHandle(f);
    return 1;
}
static int game_running(void)
{
    wchar_t data[MAX_PATH];
    game_path(data, L"data.win");
    HANDLE f = CreateFileW(data, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return GetLastError() == ERROR_SHARING_VIOLATION;
    CloseHandle(f);
    return 0;
}
static int installed(void)
{
    wchar_t dll[MAX_PATH];
    game_path(dll, L"WebbedCoop.dll");
    char sha[65];
    wchar_t data[MAX_PATH];
    game_path(data, L"data.win");
    return exists(dll) || (file_sha(data, sha) && !strcmp(sha, PATCHED_SHA));
}
static int uninstall(void)
{
    wchar_t data[MAX_PATH], original[MAX_PATH], path[MAX_PATH];
    game_path(data, L"data.win");
    game_path(original, L"WebbedCoop.backup\\data.win");
    char sha[65];
    if (file_sha(data, sha) && !strcmp(sha, PATCHED_SHA))
    {
        DWORD n;
        uint8_t *orig = read_all(original, &n);
        char osha[65] = "";
        if (orig)
            sha256_hex(orig, n, osha);
        if (!orig || strcmp(osha, DATA_SHA))
        {
            free(orig);
            fail(L"Не найдена резервная копия оригинального data.win. Проверьте файлы игры в Steam "
                 L"(Свойства → Установленные файлы → Проверить целостность).");
            return 0;
        }
        int ok = replace_file(data, orig, n);
        free(orig);
        if (!ok)
        {
            fail(L"Не удалось вернуть оригинальный data.win.");
            return 0;
        }
    }
    const wchar_t *files[] = {L"WebbedCoop.dll", L"WebbedCoopLink.exe", L"WebbedCoop.backup\\files.json"};
    for (int i = 0; i < 3; i++)
    {
        game_path(path, files[i]);
        DeleteFileW(path);
    }
    game_path(path, L"WebbedCoop");
    remove_tree(path);
    return 1;
}
static int install(void)
{
    wchar_t exe[MAX_PATH], data[MAX_PATH], original[MAX_PATH], path[MAX_PATH];
    char sha[65];
    game_path(exe, L"webbed.exe");
    if (!file_sha(exe, sha) || strcmp(sha, EXE_SHA))
    {
        fail(L"Эта версия Webbed не поддерживается. Нужна Windows-версия из Steam (сборка 7679547).");
        return 0;
    }
    game_path(data, L"data.win");
    DWORD n;
    uint8_t *current = read_all(data, &n);
    if (!current)
    {
        fail(L"Не удалось прочитать data.win.");
        return 0;
    }
    sha256_hex(current, n, sha);
    game_path(original, L"WebbedCoop.backup\\data.win");
    if (!strcmp(sha, DATA_SHA))
    {
        char osha[65];
        if (!file_sha(original, osha) || strcmp(osha, DATA_SHA))
            if (!replace_file(original, current, n))
            {
                free(current);
                fail(L"Не удалось сохранить резервную копию data.win.");
                return 0;
            }
        DWORD outSize;
        uint8_t *patched = patched_data(current, n, &outSize);
        char psha[65] = "";
        if (patched)
            sha256_hex(patched, outSize, psha);
        if (!patched || strcmp(psha, PATCHED_SHA) || !replace_file(data, patched, outSize))
        {
            free(patched);
            free(current);
            fail(L"Не удалось подготовить data.win для мода.");
            return 0;
        }
        free(patched);
    }
    else if (strcmp(sha, PATCHED_SHA))
    {
        free(current);
        fail(L"Файл data.win отличается от поддерживаемой версии Webbed. Проверьте версию игры и другие моды.");
        return 0;
    }
    free(current);
    /* Files the 1.0.0 installer left (a packed helper exe), then this version's files. */
    game_path(path, L"WebbedCoopLink.exe");
    DeleteFileW(path);
    game_path(path, L"WebbedCoop");
    remove_tree(path);
    for (int i = 0; i < entryCount; i++)
    {
        const Entry *e = &entries[i];
        if (!strcmp(e->name, "data.delta"))
            continue;
        wchar_t rel[MAX_PATH];
        MultiByteToWideChar(CP_UTF8, 0, e->name, -1, rel, MAX_PATH);
        for (wchar_t *p = rel; *p; p++)
            if (*p == L'/')
                *p = L'\\';
        game_path(path, rel);
        if (!replace_file(path, e->data, e->size))
        {
            fail(L"Не удалось записать файлы мода в папку игры.");
            return 0;
        }
    }
    return 1;
}

static int run_elevated(int remove)
{
    wchar_t self[MAX_PATH], args[MAX_PATH + 64];
    GetModuleFileNameW(NULL, self, MAX_PATH);
    swprintf(args, MAX_PATH + 64, L"%ls --game \"%ls\"", remove ? L"--uninstall" : L"--install", game);
    SHELLEXECUTEINFOW info = {.cbSize = sizeof(info), .fMask = SEE_MASK_NOCLOSEPROCESS, .lpVerb = L"runas",
                              .lpFile = self, .lpParameters = args, .nShow = SW_SHOWNORMAL};
    if (!ShellExecuteExW(&info) || !info.hProcess)
        return 0;
    WaitForSingleObject(info.hProcess, INFINITE);
    CloseHandle(info.hProcess);
    return 1;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
    (void)inst, (void)prev, (void)cmd, (void)show;
    int argc, mode = 0; /* 1 install, 2 uninstall */
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; i++)
    {
        if (!wcscmp(argv[i], L"--install"))
            mode = 1;
        else if (!wcscmp(argv[i], L"--uninstall"))
            mode = 2;
        else if (!wcscmp(argv[i], L"--quiet"))
            quiet = 1;
        else if (!wcscmp(argv[i], L"--game") && i + 1 < argc)
            wcsncpy(game, argv[++i], MAX_PATH - 1);
    }
    HANDLE lock = CreateMutexW(NULL, FALSE, L"Local\\WebbedCoopSetup-7679547");
    if (!lock || GetLastError() == ERROR_ALREADY_EXISTS)
        return 2;
    if (!load_payload())
    {
        box(L"Установщик повреждён. Скачайте его заново.", MB_ICONERROR);
        return 1;
    }
    /* 1. The game itself: open it in Steam and wait until Steam has installed it. */
    while (!game[0] && !find_app(L"Webbed", L"webbed.exe", game))
    {
        if (quiet)
        {
            fail(L"Webbed не найдена.");
            break;
        }
        static int opened;
        if (!opened && box(L"Webbed не найдена на этом компьютере.\n\nОткрыть её в Steam? Купите или установите игру, "
                           L"а когда Steam закончит, нажмите «Повтор».",
                           MB_YESNO | MB_ICONQUESTION) != IDYES)
            return 1;
        if (!opened)
            open_steam(L"install", WEBBED_APP);
        opened = 1;
        if (box(L"Жду, пока Steam установит Webbed.\n\nКогда установка закончится, нажмите «Повтор».",
                MB_RETRYCANCEL | MB_ICONINFORMATION) != IDRETRY)
            return 1;
    }
    if (!failure[0])
    {
        if (!mode)
        {
            if (installed())
            {
                if (box(L"Кооператив уже установлен. Удалить мод и вернуть исходную версию игры?",
                        MB_YESNO | MB_ICONQUESTION) != IDYES)
                    return 0;
                mode = 2;
            }
            else
                mode = 1;
        }
        while (game_running())
        {
            if (quiet || box(L"Закройте Webbed и нажмите «Повтор».", MB_RETRYCANCEL | MB_ICONWARNING) != IDRETRY)
            {
                fail(L"Webbed запущена: закройте игру и запустите установщик ещё раз.");
                break;
            }
        }
    }
    if (!failure[0] && !writable())
    {
        if (!quiet && !IsUserAnAdmin())
        {
            CloseHandle(lock);
            return run_elevated(mode == 2) ? 0 : 1;
        }
        fail(L"Нет доступа к папке игры.");
    }
    if (!failure[0] && !(mode == 2 ? uninstall() : install()))
        fail(L"Не получилось.");
    if (failure[0])
    {
        if (!quiet)
            box(failure, MB_ICONERROR);
        else
            fwprintf(stderr, L"%ls\n", failure);
        return 1;
    }
    if (mode == 2)
        box(L"Мод удалён, оригинальная версия Webbed восстановлена.", MB_ICONINFORMATION);
    else
    {
        wchar_t porthole[MAX_PATH];
        /* 2. Porthole carries the connection over Steam. */
        if (find_app(L"porthole", L"porthole.exe", porthole))
            box(L"Кооператив установлен.\n\nЗапустите Webbed из Steam: пункт «Кооператив» есть в главном меню и в паузе.",
                MB_ICONINFORMATION);
        else if (box(L"Кооператив установлен.\n\nДля игры по сети нужен бесплатный Porthole из Steam. Открыть его "
                     L"установку?",
                     MB_YESNO | MB_ICONQUESTION) == IDYES)
            open_steam(L"install", PORTHOLE_APP);
    }
    free(payload);
    CloseHandle(lock);
    return 0;
}
