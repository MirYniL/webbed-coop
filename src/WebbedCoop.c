#include "build.h"
#define F_instance_exists 0x11ab540
#define F_ds_map_create 0x11b3910
#define F_array_length 0x117f2b0
#define F_array_get 0x117f090
#define F_array_create 0x117eb40
#define F_is_array 0x1180ab0
#include "session.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <wchar.h>
#include <windows.h>

typedef struct
{
    char *text;
    int refs, size;
} GameString;
typedef struct
{
    union {
        double real;
        int i32;
        int64_t i64;
        GameString *str;
    };
    int flags, kind;
} Val;
typedef void (*Event)(void *, void *);
typedef Val *(__thiscall *Member)(void *, int);
typedef void (*Builtin)(Val *, void *, void *, int, Val *);
typedef Val *(*Script)(void *, void *, Val *, int, Val **);
static uintptr_t base;
static Bus *bus;
static void *player, *remote, *io, *cam;
static DWORD remotePress, remoteRelease;
static Spider incoming, lastRemote;
static int adding, lobby, guestSlot, menuTick, guestSlotPending, localPaused;
/* Spider.active: 0 none, 1 playing, 2 that player has the game paused (their world is frozen). */
enum
{
    SPIDER_NONE,
    SPIDER_PLAYING,
    SPIDER_PAUSED
};
static uint32_t appliedSequence;
static HANDLE helper, sessionMap;
static Val originalSlot;
static Event createPlayer, stepPlayer, drawPlayer, drawEndPlayer, endPlayer, cleanPlayer, stepInput, stepCamera, drawGui;
static wchar_t runtimeDir[MAX_PATH], patchFile[MAX_PATH];
static const unsigned objectRemote = 467;
static void *addr(uintptr_t va)
{
    return (void *)(base + va - 0x400000);
}
static Val num(double x)
{
    Val v = {0};
    v.real = x;
    return v;
}
static Val undef(void)
{
    Val v = {.kind = 5};
    return v;
}
static double value(Val v)
{
    switch (v.kind & 0xffffff)
    {
    case 0:
    case 13:
        return v.real;
    case 7:
        return v.i32;
    case 10:
        return (double)v.i64;
    default:
        return 0;
    }
}
static void drop(Val *);
typedef struct TempText
{
    Val v;
    struct TempText *next;
} TempText;
static TempText *tempTexts;
static Val str(GameString *unused, const char *s)
{
    (void)unused;
    TempText *t = malloc(sizeof(*t));
    if (!t)
        return undef();
    t->v = undef();
    ((void (*)(Val *, const char *))addr(0x119b4d0))(&t->v, s);
    t->next = tempTexts;
    tempTexts = t;
    return t->v;
}
static void clear_texts(void)
{
    while (tempTexts)
    {
        TempText *t = tempTexts;
        tempTexts = t->next;
        drop(&t->v);
        free(t);
    }
}
static Val *field(void *p, int id)
{
    return (*(Member **)p)[2](p, id);
}
static double read_field(void *p, int id)
{
    return value(*field(p, id));
}
static double coord(void *p, uintptr_t idx)
{
    Val r = undef();
    typedef void (*Get)(void *, int, int, Val *, int, int);
    ((Get)addr(0x119d2f0))(p, *(int *)addr(idx), INT32_MIN, &r, 0, 0);
    return value(r);
}
static void set_coord(void *p, uintptr_t idx, double n)
{
    Val v = num(n);
    typedef void (*Set)(void *, int, int, Val *);
    ((Set)addr(0x119e130))(p, *(int *)addr(idx), INT32_MIN, &v);
}
static Val builtin(void *p, uintptr_t f, int n, Val *a)
{
    Val r = undef();
    ((Builtin)addr(f))(&r, p, p, n, a);
    return r;
}
#define FN(p, f, ...) builtin(p, F_##f, (int)(sizeof((Val[]){__VA_ARGS__}) / sizeof(Val)), (Val[]){__VA_ARGS__})
#define FN0(p, f) builtin(p, F_##f, 0, NULL)
static Val script(void *p, uintptr_t f, int n, Val *args)
{
    Val r = undef();
    Val *argv[4] = {NULL};
    for (int i = 0; i < n; i++)
        argv[i] = args + i;
    ((Script)addr(f))(p, p, &r, n, argv);
    return r;
}
static void drop(Val *r)
{
    Val u = undef();
    typedef void(__thiscall * Assign)(Val *, Val *);
    ((Assign)addr(0x401ff0))(r, &u);
}
static Val global_get(void *p, const char *key)
{
    GameString k;
    return FN(p, variable_global_get, str(&k, key));
}
static void global_set(void *p, const char *key, Val v)
{
    GameString k;
    FN(p, variable_global_set, str(&k, key), v);
}
static void log_line(const char *s)
{
    OutputDebugStringA(s);
#ifdef NET_QA
    FILE *f = fopen("native-qa.log", "a");
    if (f)
    {
        fprintf(f, "%s\n", s);
        fclose(f);
    }
#endif
}
static int isnum(Val v)
{
    int k = v.kind & 0xffffff;
    return k == 0 || k == 7 || k == 10 || k == 13;
}
static int activeWindow(void)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}
static int anchor(void *p)
{
    return p && isfinite(coord(p, B_x)) && isfinite(coord(p, B_y));
}
static int bit(unsigned bits, int n)
{
    return !!(bits & (1u << n));
}
static double dir(double x, double y)
{
    double d = atan2(-y, x) * 57.2957795131;
    return d < 0 ? d + 360 : d;
}
static void on_create(void *self, void *other)
{
    createPlayer(self, other);
    if (adding)
    {
        remote = self;
        log_line("Remote spawned");
    }
    else
        player = self;
}
static void on_clean(void *self, void *other)
{
    if (self == player)
        player = NULL;
    if (self == remote)
        remote = NULL;
    cleanPlayer(self, other);
}
static int same_room(void)
{
    return bus->status == CONNECTED && incoming.active && GetTickCount() - incoming.heartbeat < 1600 &&
           incoming.room == bus->room && !guestSlotPending;
}
static void on_step(void *self, void *other);
static void remote_event(Event orig, void *self, void *other)
{
    double dx = incoming.hm, dy = incoming.vm, px = coord(self, B_x), py = coord(self, B_y);
    unsigned b = incoming.buttons, pr = remotePress, rel = remoteRelease;
    const int ids[] = {V_hm,
                       V_vm,
                       V_mvmag,
                       V_mvdir,
                       V_jmp,
                       V_jmp_held,
                       V_grapple,
                       V_grapple_pressed,
                       V_grapple_released,
                       V_weave,
                       V_weave_pressed,
                       V_weave_released,
                       V_laser,
                       V_dance,
                       V_interact,
                       V_interact_held,
                       V_mx,
                       V_my,
                       V_aimdir,
                       V_aimmag,
                       V_toggle_pause,
                       V_bugopedia,
                       V_bugopedia_pressed,
                       V_show_cursor};
    double vars[] = {dx,
                     dy,
                     fmin(1, hypot(dx, dy)),
                     dir(dx, dy),
                     bit(pr, 0),
                     bit(b, 0),
                     bit(b, 1),
                     bit(pr, 1),
                     bit(rel, 1),
                     bit(b, 2),
                     bit(pr, 2),
                     bit(rel, 2),
                     bit(b, 3),
                     bit(b, 4),
                     bit(pr, 5),
                     bit(b, 5),
                     incoming.mx,
                     incoming.my,
                     dir(incoming.mx - px, incoming.my - py),
                     1,
                     0,
                     0,
                     0,
                     1};
    _Static_assert(sizeof(ids) / sizeof(*ids) == sizeof(vars) / sizeof(*vars), "pad fields");
    Val old[sizeof(ids) / sizeof(*ids)];
    for (unsigned i = 0; i < sizeof(ids) / sizeof(*ids); i++)
    {
        Val *v = field(io, ids[i]);
        old[i] = *v;
        *v = num(vars[i]);
    }
    orig(self, other);
    for (unsigned i = 0; i < sizeof(ids) / sizeof(*ids); i++)
        *field(io, ids[i]) = old[i];
}
static void on_step(void *self, void *other)
{
    if (self == player && same_room() && !remote && !adding)
    {
        adding = 1;
        FN(self, instance_create_depth, num(incoming.x), num(incoming.y), num(coord(self, B_depth)), num(objectRemote));
        adding = 0;
    }
    if (self != remote)
    {
        stepPlayer(self, other);
        return;
    }
    if (!same_room())
    {
        FN0(self, instance_destroy);
        return;
    }
    if (incoming.active == SPIDER_PAUSED)
    {
        /* The friend paused: their spider stands still exactly where they left it. */
        set_coord(self, B_phy_position_x, incoming.x);
        set_coord(self, B_phy_position_y, incoming.y);
        set_coord(self, B_phy_speed_x, 0);
        set_coord(self, B_phy_speed_y, 0);
        appliedSequence = incoming.sequence;
        return;
    }
    if (io)
        remote_event(stepPlayer, self, other);
    /* The friend's spider runs on their buttons here; each network frame says where it really is.
       Small differences are eased in over a few frames instead of teleporting the spider, which
       read as stutter; only a big jump (a door, a respawn) snaps. */
    static double corrX, corrY;
    if (incoming.sequence != appliedSequence)
    {
        appliedSequence = incoming.sequence;
        double dx = incoming.x - coord(self, B_x), dy = incoming.y - coord(self, B_y);
        if (fabs(dx) + fabs(dy) > 64)
        {
            set_coord(self, B_phy_position_x, incoming.x);
            set_coord(self, B_phy_position_y, incoming.y);
            set_coord(self, B_phy_speed_x, incoming.vx);
            set_coord(self, B_phy_speed_y, incoming.vy);
            corrX = corrY = 0;
        }
        else
        {
            corrX = dx;
            corrY = dy;
            set_coord(self, B_phy_speed_x, .5 * coord(self, B_phy_speed_x) + .5 * incoming.vx);
            set_coord(self, B_phy_speed_y, .5 * coord(self, B_phy_speed_y) + .5 * incoming.vy);
        }
    }
    if (fabs(corrX) + fabs(corrY) > .05)
    {
        double kx = corrX * .3, ky = corrY * .3;
        set_coord(self, B_phy_position_x, coord(self, B_x) + kx);
        set_coord(self, B_phy_position_y, coord(self, B_y) + ky);
        corrX -= kx;
        corrY -= ky;
    }
}
static void arrow(void *,double,double,double,double,double,int);
static void shadow_text(void *, double, double, const char *, int, double);
static int russian(void *);
static int peer_colour(void);
static int soft_colour(int,double);
static void on_draw(void *self, void *other)
{
    if (self != remote || !io)
    {
        drawPlayer(self, other);
        return;
    }
    /* Tint the friend's spider towards its marker colour so the two read apart at a glance. */
    int tint = soft_colour(peer_colour(), .6);
    double blend = coord(self, B_image_blend);
    set_coord(self, B_image_blend, tint);
    remote_event(drawPlayer, self, other);
    set_coord(self, B_image_blend, blend);
    double x = coord(self, B_x), y = coord(self, B_y) - 17;
    Val alpha=FN0(self,draw_get_alpha);
    FN(self,draw_set_alpha,num(1));
    arrow(self,x,y-6,0,1,16,peer_colour());
    if (incoming.active == SPIDER_PAUSED)
        shadow_text(self, x, y - 30, russian(io) ? "Пауза" : "Paused", 0xffffff, 1);
    FN(self,draw_set_alpha,alpha);
}
/* Draw End paints the grapple/weave aim line towards (gx, gy): for the friend's spider it must
   read the friend's buttons, not this player's. */
static void on_draw_end(void *self, void *other)
{
    if (self == remote && io)
        remote_event(drawEndPlayer, self, other);
    else
        drawEndPlayer(self, other);
}
static void on_end(void *self, void *other)
{
    if (self != remote || !cam)
    {
        endPlayer(self, other);
        return;
    }
    Val tx = *field(cam, V_target_x), ty = *field(cam, V_target_y);
    endPlayer(self, other);
    *field(cam, V_target_x) = tx;
    *field(cam, V_target_y) = ty;
}
static void on_camera(void *self, void *other)
{
    cam = self;
    stepCamera(self, other);
}
static void network_frame(void *self)
{
    Spider s = bus->local;
    /* The pause menu deactivates every instance, so an inactive spider means a paused game
       (the global "paused" flag can stay set after the menu closes). */
    localPaused = player && !value(FN(self, instance_exists, num(coord(player, B_id))));
    s.active = (player != NULL && !guestSlotPending) ? (localPaused ? SPIDER_PAUSED : SPIDER_PLAYING) : SPIDER_NONE;
    s.room = bus->room = (unsigned)coord(self, B_room);
    s.heartbeat = GetTickCount();
    bus->playing = s.active;
    if (s.active)
    {
        s.x = coord(player, B_x);
        s.y = coord(player, B_y);
        s.vx = coord(player, B_phy_speed_x);
        s.vy = coord(player, B_phy_speed_y);
        s.hm = read_field(self, V_hm);
        s.vm = read_field(self, V_vm);
        s.mx = read_field(self, V_mx);
        s.my = read_field(self, V_my);
        const int ids[] = {V_jmp_held, V_grapple, V_weave, V_laser, V_dance, V_interact_held};
        unsigned old = s.buttons;
        s.buttons = 0;
        for (unsigned i = 0; i < 6; i++)
        {
            int held = read_field(self, ids[i]) != 0;
            if (held)
                s.buttons |= 1u << i;
            if (held != bit(old, i))
            {
                if (held)
                    s.down[i]++;
                else
                    s.up[i]++;
            }
        }
    }
    uint32_t seq = (bus->local.sequence & ~1u) + 2;
    s.sequence = seq - 1;
    bus->local.sequence = seq - 1;
    MemoryBarrier();
    memcpy((char *)&bus->local + 4, (char *)&s + 4, sizeof(s) - 4);
    MemoryBarrier();
    bus->local.sequence = seq;
    uint32_t version = bus->remote.sequence;
    MemoryBarrier();
    Spider peer = bus->remote;
    MemoryBarrier();
    if (!(version & 1) && version == bus->remote.sequence)
        incoming = peer;
    remotePress = remoteRelease = 0;
    for (unsigned i = 0; i < 6; i++)
    {
        if (incoming.down[i] != lastRemote.down[i])
            remotePress |= 1u << i;
        if (incoming.up[i] != lastRemote.up[i])
            remoteRelease |= 1u << i;
    }
    lastRemote = incoming;
}
#include "world.inc"

#include "campaign.inc"

#include "lobby.inc"

#include "native_menu.inc"

#include "markers.inc"
/* Webbed pauses itself when its window loses focus (os_is_paused in oIO). During a session that
   would freeze one player whenever they look at a chat, and on one PC it pauses the second
   window outright; Esc still pauses. The call goes through a function slot we can wrap. */
#define SLOT_os_is_paused 0x15cadc8
#define F_os_is_paused_impl 0x12d0560
static Builtin osIsPaused;
static void coop_os_is_paused(Val *ret, void *self, void *other, int argc, Val *args)
{
    osIsPaused(ret, self, other, argc, args);
    if (bus && bus->role)
        *ret = num(0);
}
static void wrap_focus_pause(void)
{
    Builtin *slot = addr(SLOT_os_is_paused);
    if (!osIsPaused && *slot == (Builtin)addr(F_os_is_paused_impl))
    {
        osIsPaused = *slot;
        *slot = coop_os_is_paused;
    }
}
/* A window without focus gets no input in a session: on one PC a gamepad would otherwise drive
   both spiders. */
static void drop_unfocused_input(void *self)
{
    if (!bus->role || activeWindow())
        return;
    const int ids[] = {V_hm,      V_vm,         V_mvmag,          V_jmp,           V_jmp_held, V_grapple,
                       V_grapple_pressed, V_weave, V_weave_pressed, V_laser, V_dance, V_interact,
                       V_interact_held, V_toggle_pause};
    for (unsigned i = 0; i < sizeof(ids) / sizeof(*ids); i++)
        *field(self, ids[i]) = num(0);
}
static void on_input(void *self, void *other)
{
    io = self;
    wrap_focus_pause();
    stepInput(self, other);
    drop_unfocused_input(self);
    menuTick++;
    coop_script_id(self);
    menu_input(self);
    coop_menu_step(self);
    network_frame(self);
    sync_world(self);
    campaign_commands(self);
#ifdef NET_QA
    qa_step(self);
#endif
    clear_texts();
}
static void on_gui(void *self, void *other)
{
    drawGui(self, other);
    edge_marker(self);
    coop_toast(self);
    clear_texts();
}
__declspec(dllexport) double webbed_coop_init(void)
{
    base = (uintptr_t)GetModuleHandleW(NULL);
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS32 *pe = (IMAGE_NT_HEADERS32 *)(base + dos->e_lfanew);
    if (pe->FileHeader.TimeDateStamp != 0x6189c082 || pe->OptionalHeader.SizeOfImage != 0x154e000)
        return 0;
    struct Bind
    {
        uintptr_t slot, original;
        Event newEvent, *old;
    } binds[] = {{0x15dbd14,0xe20e80,on_main_menu_create,&createMainMenu},{0x15d9ba8,0x9baa10,on_book_draw,&drawBook},{0x15d9e0c, 0xa609e0, on_create, &createPlayer}, {0x15d9e54, 0xa68e10, on_step, &stepPlayer},
                 {0x15d9e90, 0xb070a0, on_draw, &drawPlayer},
                 {0x15d9e9c, 0xb07de0, on_draw_end, &drawEndPlayer},     {0x15d9ea8, 0xb141c0, on_clean, &cleanPlayer},
                 {0x15d98b4, 0x9532e0, on_input, &stepInput},     {0x15d9818, 0x913b20, on_camera, &stepCamera},
                 {0x15d9e60, 0xaf1800, on_end, &endPlayer},       {0x15d986c, 0x94a460, on_gui, &drawGui}};
    for (unsigned i = 0; i < sizeof(binds) / sizeof(*binds); i++)
        if (*(Event *)addr(binds[i].slot) != (Event)addr(binds[i].original))
            return 0;
    wchar_t name[80];
    swprintf(name, 80, L"Local\\WebbedPeer_%lu", GetCurrentProcessId());
    HANDLE map = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(Bus), name);
    if (!map)
        return 0;
    bus = MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Bus));
    sessionMap = map;
    if (!bus)
    {
        CloseHandle(map);
        return 0;
    }
    if (!install_menu_hook())
    {
        UnmapViewOfFile(bus);
        CloseHandle(map);
        bus = NULL;
        return 0;
    }
    if (!install_enpause_hook())
        log_line("coop: soft pause unavailable, the game pauses as usual");
    open_world_map();
    bus->magic = 0x57425032;
    GetModuleFileNameW(NULL, runtimeDir, MAX_PATH);
    wchar_t *s = wcsrchr(runtimeDir, L'\\');
    if (s)
        *s = 0;
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    swprintf(patchFile, MAX_PATH, L"%lsWebbedCoop-%lu.tmp", tmp, GetCurrentProcessId());
    for (unsigned i = 0; i < sizeof(binds) / sizeof(*binds); i++)
    {
        Event *slot = addr(binds[i].slot);
        *binds[i].old = *slot;
        *slot = binds[i].newEvent;
    }
    return 1;
}
