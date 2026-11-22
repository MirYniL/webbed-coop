#include "steam_friends.inc"
static void clipboard_copy(const char *s)
{
    if (!s[0] || !OpenClipboard(NULL))
        return;
    size_t n = strlen(s) + 1;
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, n);
    if (h)
    {
        char *d = GlobalLock(h);
        if (d)
        {
            memcpy(d, s, n);
            GlobalUnlock(h);
            EmptyClipboard();
            if (!SetClipboardData(CF_TEXT, h))
                GlobalFree(h);
        }
        else
            GlobalFree(h);
    }
    CloseClipboard();
}
static int clipboard_invite(void)
{
    if (!OpenClipboard(NULL))
        return 0;
    HANDLE h = GetClipboardData(CF_TEXT);
    char *s = h ? GlobalLock(h) : NULL;
    int good = 0;
    if (s)
    {
        size_t n = strnlen(s, 190);
        if (n < 190 && valid_invite(s))
        {
            memcpy(bus->invite, s, n);
            bus->invite[n] = 0;
            good = 1;
        }
        GlobalUnlock(h);
    }
    CloseClipboard();
    return good;
}
static int russian(void *self);
#define T(ru, en) (russian(io) ? (ru) : (en))
static void start_link(int role)
{
    if (helper || bus->role)
        return;
    if (role == GUEST && !valid_invite(bus->invite))
    {
        snprintf(bus->status_text, 192, "%s", T("Скопируйте приглашение друга", "Copy your friend's invite first"));
        return;
    }
    if (role == HOST && !player)
    {
        snprintf(bus->status_text, 192, "%s", T("Сначала загрузите сохранение", "Load a save first"));
        return;
    }
    wchar_t exe[MAX_PATH], cmd[2 * MAX_PATH];
    swprintf(exe, MAX_PATH, L"%ls\\WebbedCoopLink.exe", runtimeDir);
    swprintf(cmd, 2 * MAX_PATH, L"\"%ls\" --link %lu", exe, GetCurrentProcessId());
    STARTUPINFOW si = {.cb = sizeof(si)};
    PROCESS_INFORMATION pi = {0};
    if (role == HOST)
        bus->invite[0] = 0;
    snprintf(bus->status_text, sizeof(bus->status_text), "%s", T("Запускается Porthole…", "Starting Porthole…"));
    bus->role = role;
    bus->status = CONNECTING;
    bus->disconnect = 0;
    if (!CreateProcessW(exe, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, runtimeDir, &si, &pi))
    {
        bus->role = 0;
        bus->status = FAILED;
        snprintf(bus->status_text, 192, "%s", T("Не удалось открыть сетевой модуль", "Could not start the network helper"));
        return;
    }
    helper = pi.hProcess;
    CloseHandle(pi.hThread);
    if (role == GUEST)
        guestSlotPending = 1;
}
/* Join: an invite on the clipboard wins, otherwise look for a friend with an open world in Steam. */
static void coop_join(void)
{
    if (bus->role || discoveryStarted)
        return;
    if (clipboard_invite())
    {
        start_link(GUEST);
        return;
    }
    char found[192];
    find_friend_world(found, 1);
    discoveryStarted = GetTickCount();
    snprintf(bus->status_text, 192, "%s", T("Ищу мир друга в Steam…", "Looking for your friend's world on Steam…"));
}
static void menu_input(void *self)
{
    if (helper && WaitForSingleObject(helper, 0) == WAIT_OBJECT_0)
    {
        CloseHandle(helper);
        helper = NULL;
        bus->role = 0;
        bus->status = IDLE;
        guestSlotPending = 0;
        if (guestSlot)
        {
            /* Restart into the player's own menu before the guest world can save to the solo slot. */
            guestSlot = 0;
            drop(&originalSlot);
            FN0(self, game_restart);
        }
    }
    steam_setup();
    announce_world();
    if (!bus->role && pendingSteamInvite[0])
    {
        snprintf(bus->invite, sizeof(bus->invite), "%s", pendingSteamInvite);
        pendingSteamInvite[0] = 0;
        discoveryStarted = 0;
        start_link(GUEST);
    }
    if (discoveryStarted && GetTickCount() - discoveryStarted > 1600 && !bus->role)
    {
        char found[192] = {0};
        int count = find_friend_world(found, 0);
        if (count == 1)
        {
            snprintf(bus->invite, sizeof(bus->invite), "%s", found);
            start_link(GUEST);
        }
        else
            snprintf(bus->status_text, 192, "%s",
                     count > 1 ? T("Мир открыли несколько друзей · скопируйте приглашение нужного",
                                   "Several friends are hosting · copy the invite you want")
                               : T("Друг ещё не открыл мир", "Your friend has not opened a world yet"));
        discoveryStarted = 0;
    }
}
