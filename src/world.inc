#include "physics_ids.h"
#define WEB_MAX 1536
#define BODY_MAX 768
/* Highest placed instance ID in all rooms of Steam build 7679547. */
#define LAST_PLACED_ID 128740
typedef struct
{
    int id, type;
    float x, y, sx, sy;
} Endpoint;
typedef struct
{
    int id, colour;
    Endpoint o, a;
    float props[12];
} Strand;
typedef struct
{
    int id, type;
    float x, y, vx, vy, angle, spin;
} Body;
typedef struct
{
    unsigned version, room, numWebs, numBodies, complete;
    Strand web[WEB_MAX];
    Body bodies[BODY_MAX];
} World;
static World *worldData;
static HANDLE worldMapping;
static unsigned lastWorld, knownRoom = ~0u;
static const int webFields[] = {
    V_ox, V_oy, V_ax, V_ay, V_odir, V_odist, V_adir, V_adist, V_length, V_current_length, V_anchored, V_breakable};
typedef struct
{
    uint32_t implementation[12];
} Iterator;
static void begin(Iterator *i, void *self, int type)
{
    typedef void(__thiscall * Begin)(Iterator *, int, void *, void *, int);
    ((Begin)addr(0x1211ff0))(i, type, self, self, 0);
}
static void *next(Iterator *i)
{
    typedef void *(__thiscall * Get)(Iterator *);
    typedef void(__thiscall * Advance)(Iterator *);
    void *p = ((Get)addr(0x12121d0))(i);
    if (p)
        ((Advance)addr(0x1213300))(i);
    return p;
}
static int instance_id(void *p)
{
    return (int)coord(p, B_id);
}
static int object_type(void *p)
{
    return (int)coord(p, B_object_index);
}
static void *find_id(void *self, int id)
{
    if (id < 100000)
        return NULL;
    Iterator it;
    begin(&it, self, id);
    return next(&it);
}
static Endpoint endpoint(void *self, Val v)
{
    Endpoint e = {.id = -4, .type = -1};
    void *p = NULL;
    if ((v.kind & 0xffffff) == 6)
        p = (void *)(uintptr_t)v.i32;
    else if (isnum(v))
        p = find_id(self, (int)value(v));
    if (p)
    {
        e.id = instance_id(p);
        e.type = object_type(p);
        e.x = coord(p, B_x);
        e.y = coord(p, B_y);
        e.sx = coord(p, B_xstart);
        e.sy = coord(p, B_ystart);
    }
    return e;
}
static int mobile_type(int t)
{
    for (unsigned i = 0; i < sizeof(dynamic_types) / sizeof(*dynamic_types); i++)
        if (dynamic_types[i] == t)
            return 1;
    return 0;
}
static int is_tie_web(int id);
static void fill_strand(void *self, void *p, Strand *s);
static void publish_world(void *self)
{
    unsigned seq = (worldData->version & ~1u) + 2;
    worldData->version = seq - 1;
    MemoryBarrier();
    worldData->room = bus->room;
    worldData->numWebs = worldData->numBodies = 0;
    worldData->complete = 1;
    Iterator it;
    void *p;
    begin(&it, self, 107);
    while ((p = next(&it)))
    {
        if (worldData->numWebs == WEB_MAX)
        {
            worldData->complete = 0;
            break;
        }
        if (is_tie_web(instance_id(p)))
            continue;
        Strand *s = &worldData->web[worldData->numWebs++];
        s->id = instance_id(p);
        fill_strand(self, p, s);
    }
    begin(&it, self, 69);
    while ((p = next(&it)) && worldData->numBodies < BODY_MAX)
    {
        int id = instance_id(p), t = object_type(p);
        if (id > LAST_PLACED_ID || t == 101 || t == 105 || t == 106 || !mobile_type(t))
            continue;
        Body *b = &worldData->bodies[worldData->numBodies++];
        *b = (Body){id,
                    t,
                    coord(p, B_x),
                    coord(p, B_y),
                    coord(p, B_phy_speed_x),
                    coord(p, B_phy_speed_y),
                    coord(p, B_phy_rotation),
                    coord(p, B_phy_angular_velocity)};
    }
    MemoryBarrier();
    worldData->version = seq;
}
typedef struct
{
    int host, local;
} Binding;
static Binding nodeBindings[WEB_MAX * 2], webBindings[WEB_MAX];
static int nodeCount, webCount;
/* Small open-addressing table: instance id -> index, rebuilt per update (ids are > 100000). */
#define TABLE_SIZE 4096
typedef struct
{
    int key[TABLE_SIZE], val[TABLE_SIZE];
} Table;
static void table_clear(Table *t)
{
    memset(t->key, 0, sizeof(t->key));
}
static void table_put(Table *t, int key, int val)
{
    unsigned h = (unsigned)key * 2654435761u % TABLE_SIZE;
    for (unsigned i = 0; i < TABLE_SIZE; i++, h = (h + 1) % TABLE_SIZE)
        if (!t->key[h] || t->key[h] == key)
        {
            t->key[h] = key;
            t->val[h] = val;
            return;
        }
}
static int table_get(const Table *t, int key, int missing)
{
    unsigned h = (unsigned)key * 2654435761u % TABLE_SIZE;
    for (unsigned i = 0; i < TABLE_SIZE && t->key[h]; i++, h = (h + 1) % TABLE_SIZE)
        if (t->key[h] == key)
            return t->val[h];
    return missing;
}
static Table localById, bindingByHost, incomingIds;
static void bind_id(Binding *list, int *n, int limit, int host, int local)
{
    for (int i = 0; i < *n; i++)
        if (list[i].host == host)
        {
            list[i].local = local;
            return;
        }
    if (*n < limit)
        list[(*n)++] = (Binding){host, local};
}
static int bound(Binding *list, int n, int host)
{
    for (int i = 0; i < n; i++)
        if (list[i].host == host)
            return list[i].local;
    return -4;
}
static int resolve_anchor(void *self, Endpoint e)
{
    if (e.type < 0)
        return -4;
    if (e.type == 94)
        return remote ? instance_id(remote) : -4;
    if (e.type == 467)
        return player ? instance_id(player) : -4;
    if (e.type >= 467 || e.id < 100000)
        return -4;
    int cached = bound(nodeBindings, nodeCount, e.id);
    void *p = find_id(self, cached);
    if (p && object_type(p) == e.type)
        return cached;
    p = find_id(self, e.id);
    if (p && object_type(p) == e.type &&
        (e.id <= LAST_PLACED_ID || hypot(coord(p, B_xstart) - e.sx, coord(p, B_ystart) - e.sy) < 1.5))
        return e.id;
    Iterator it;
    begin(&it, self, e.type);
    int best = -4;
    double distance = 36;
    while ((p = next(&it)))
    {
        double dx = e.x - coord(p, B_x), dy = e.y - coord(p, B_y), dist = dx * dx + dy * dy;
        if (dist < distance)
        {
            best = instance_id(p);
            distance = dist;
        }
    }
    if (best == -4 && (e.type == 101 || e.type == 105))
    {
        GameString layer;
        Val v = FN(self, instance_create_layer, num(e.x), num(e.y), str(&layer, "Webs"), num(e.type));
        best = (int)value(v);
    }
    if (best >= 100000)
        bind_id(nodeBindings, &nodeCount, WEB_MAX * 2, e.id, best);
    return best;
}
static void fill_strand(void *self, void *p, Strand *s)
{
    s->id = instance_id(p);
    s->colour = (int)coord(p, B_image_blend);
    s->o = endpoint(self, *field(p, V_o));
    s->a = endpoint(self, *field(p, V_a));
    for (int j = 0; j < 12; j++)
        s->props[j] = (float)read_field(p, webFields[j]);
}
/* ---- Ownership. Each player's game is the authority for the webs that player makes, as for an
   NPC's own body: the host's world carries the host's webs (and the level's), and the guest sends
   the webs it spun ("ties"), which the host creates and keeps out of its world list. Whoever cuts a
   strand asks its owner to remove it, and the pieces left after the cut belong to the one who cut. */
#define TIE_MAX 96
#define OWN_MAX 128
typedef struct
{
    uint32_t version, count;
    Strand s[TIE_MAX];
} Ties;
static Ties *tiesData;
static Ties incomingTies;
static HANDLE tiesMapping;
static int tieIds[TIE_MAX], tieIdCount; /* guest: the webs it sent; host: webs made from them */
static int tieSource[TIE_MAX];          /* host: the guest's strand id for each */
static unsigned lastTies, tieRoom = ~0u;
static int ownIds[OWN_MAX], ownCount; /* guest: webs its own spider spun */
static int is_own(int id)
{
    for (int i = 0; i < ownCount; i++)
        if (ownIds[i] == id)
            return 1;
    return 0;
}
static int is_tie_web(int id)
{
    for (int i = 0; i < tieIdCount; i++)
        if (tieIds[i] == id)
            return 1;
    return bus->role == GUEST && is_own(id);
}
static int bound_local(int id)
{
    for (int i = 0; i < webCount; i++)
        if (webBindings[i].local == id)
            return 1;
    return 0;
}
/* Instance ids only grow, so a web newer than the newest one before the spider's step was spun
   (or split off by a cut) during that step. */
static int newest_web(void *self)
{
    if (bus->role != GUEST || bus->status != CONNECTED)
        return -1;
    int newest = 0;
    Iterator it;
    void *p;
    begin(&it, self, 107);
    while ((p = next(&it)))
    {
        int id = instance_id(p);
        if (id > newest)
            newest = id;
    }
    return newest;
}
static void forget_dead_own(void *self)
{
    int n = 0;
    for (int i = 0; i < ownCount; i++)
    {
        void *p = find_id(self, ownIds[i]);
        if (p && object_type(p) == 107)
            ownIds[n++] = ownIds[i];
    }
    ownCount = n;
}
static void claim_new_webs(void *self, int newest)
{
    if (newest < 0)
        return;
    Iterator it;
    void *p;
    begin(&it, self, 107);
    while ((p = next(&it)))
    {
        int id = instance_id(p);
        if (id <= newest || is_own(id))
            continue;
        if (ownCount == OWN_MAX)
            forget_dead_own(self);
        if (ownCount < OWN_MAX)
            ownIds[ownCount++] = id;
    }
}
/* ---- Cuts: strand ids for the other side to remove. The game appends to "out", the network
   helper to "in"; each side only ever moves its own counter forward. ---- */
#define CUT_RING 64
typedef struct
{
    uint32_t outCount;
    int32_t out[CUT_RING];
    uint32_t inCount;
    int32_t in[CUT_RING];
} Cuts;
static Cuts *cutsData;
static HANDLE cutsMapping;
static uint32_t cutsSeen;
typedef struct
{
    int id;
    DWORD until;
} Hold;
static Hold holds[64];
static void send_cut(int id)
{
    if (!cutsData)
        return;
    cutsData->out[cutsData->outCount % CUT_RING] = id;
    MemoryBarrier();
    cutsData->outCount++;
}
/* A strand just cut here stays cut while the owner hears about it (a round trip or two). */
static void hold(int id)
{
    int slot = 0;
    DWORD now = GetTickCount();
    for (int i = 0; i < 64; i++)
        if ((int)(holds[i].until - now) < (int)(holds[slot].until - now))
            slot = i;
    holds[slot] = (Hold){id, now + 1500};
}
static int held(int id)
{
    DWORD now = GetTickCount();
    for (int i = 0; i < 64; i++)
        if (holds[i].id == id && (int)(holds[i].until - now) > 0)
            return 1;
    return 0;
}
static void receive_cuts(void *self)
{
    if (!cutsData)
        return;
    uint32_t count = cutsData->inCount;
    MemoryBarrier();
    if (count - cutsSeen > CUT_RING)
        cutsSeen = count - CUT_RING;
    for (; cutsSeen != count; cutsSeen++)
    {
        int id = cutsData->in[cutsSeen % CUT_RING];
        void *p = find_id(self, id);
        if (!p || object_type(p) != 107)
            continue;
        /* Only the owner's copy goes: the guest removes what it spun, the host what its world has. */
        if (bus->role == GUEST ? !is_tie_web(id) : is_tie_web(id))
            continue;
        hold(id);
        FN0(p, instance_destroy);
    }
}
static void publish_ties(void *self)
{
    if (!tiesData)
        return;
    double rid = remote ? coord(remote, B_id) : -1e9;
    forget_dead_own(self);
    int fresh[TIE_MAX], freshCount = 0;
    unsigned seq = (tiesData->version & ~1u) + 2;
    tiesData->version = seq - 1;
    MemoryBarrier();
    tiesData->count = 0;
    Iterator it;
    void *p;
    begin(&it, self, 107);
    while ((p = next(&it)) && tiesData->count < TIE_MAX)
    {
        int id = instance_id(p);
        /* Ours: what our spider spun, and webs on the friend's spider that are not from the host. */
        if (!is_own(id) &&
            ((value(*field(p, V_o)) != rid && value(*field(p, V_a)) != rid) || (!is_tie_web(id) && bound_local(id))))
            continue;
        fresh[freshCount++] = id;
        fill_strand(self, p, &tiesData->s[tiesData->count++]);
    }
    memcpy(tieIds, fresh, freshCount * sizeof(int));
    tieIdCount = freshCount;
    MemoryBarrier();
    tiesData->version = seq;
}
static void apply_ties(void *self)
{
    if (!tiesData)
        return;
    if (tieRoom != bus->room)
    {
        tieIdCount = 0; /* new room: the old ones went with it */
        tieRoom = bus->room;
    }
    /* A tie that vanished here was cut by the host's spider: tell the guest, who owns it. */
    for (int j = 0; j < tieIdCount; j++)
    {
        void *p = find_id(self, tieIds[j]);
        if ((!p || object_type(p) != 107) && !held(tieSource[j]))
        {
            send_cut(tieSource[j]);
            hold(tieSource[j]);
        }
    }
    unsigned seq = tiesData->version;
    if (seq & 1 || seq == lastTies)
        return;
    MemoryBarrier();
    memcpy(&incomingTies, tiesData, sizeof(Ties));
    MemoryBarrier();
    if (seq != tiesData->version || incomingTies.count > TIE_MAX)
        return;
    lastTies = seq;
    int keepIds[TIE_MAX], keepSource[TIE_MAX], keep = 0;
    for (unsigned i = 0; i < incomingTies.count; i++)
    {
        Strand *s = &incomingTies.s[i];
        if (held(s->id))
            continue;
        void *p = NULL;
        for (int j = 0; j < tieIdCount; j++)
            if (tieSource[j] == s->id)
            {
                p = find_id(self, tieIds[j]);
                if (p && object_type(p) != 107)
                    p = NULL;
                break;
            }
        if (!p)
        {
            int o = resolve_anchor(self, s->o), a = resolve_anchor(self, s->a);
            if ((s->o.type >= 0 && o < 0) || (s->a.type >= 0 && a < 0))
                continue;
            GameString layer;
            Val id = FN(self, instance_create_layer, num(s->props[0]), num(s->props[1]), str(&layer, "Webs"), num(107));
            p = find_id(self, (int)value(id));
            if (!p)
                continue;
            *field(p, V_o) = num(o);
            *field(p, V_a) = num(a);
            for (int j = 0; j < 12; j++)
                *field(p, webFields[j]) = num(s->props[j]);
            set_coord(p, B_image_blend, s->colour);
            Val created = num(instance_id(p));
            script(p, 0x4df040, 1, &created);
        }
        else
            *field(p, V_length) = num(s->props[8]);
        if (keep < TIE_MAX)
        {
            keepIds[keep] = instance_id(p);
            keepSource[keep++] = s->id;
        }
    }
    /* Ties the guest no longer has (cut on its side) go here too. */
    for (int j = 0; j < tieIdCount; j++)
    {
        int still = 0;
        for (int k = 0; k < keep; k++)
            still |= keepIds[k] == tieIds[j];
        void *p = still ? NULL : find_id(self, tieIds[j]);
        if (p && object_type(p) == 107)
            FN0(p, instance_destroy);
    }
    memcpy(tieIds, keepIds, keep * sizeof(int));
    memcpy(tieSource, keepSource, keep * sizeof(int));
    tieIdCount = keep;
}
typedef struct
{
    int id, used;
    double ox, oy, ax, ay;
} LocalWeb;
static LocalWeb localWebs[WEB_MAX];

static World incomingWorld;
static void apply_world(void *self)
{
    unsigned seq = worldData->version;
    if (seq & 1 || seq == lastWorld)
        return;
    MemoryBarrier();
    memcpy(&incomingWorld, worldData, sizeof(World));
    MemoryBarrier();
    if (seq != worldData->version)
        return;
    lastWorld = seq;
    World *w = &incomingWorld;
    if (w->room != bus->room || w->numWebs > WEB_MAX || w->numBodies > BODY_MAX)
        return;
    int entering = knownRoom != w->room;
    (void)entering;
    if (entering)
    {
        nodeCount = webCount = 0;
        knownRoom = w->room;
    }
    for (int i = 0; i < webCount; i++)
    {
        Binding old = webBindings[i];
        void *gone = old.local >= 100000 ? find_id(self, old.local) : (void *)1;
        if ((!gone || object_type(gone) != 107) && !held(old.host))
        {
            send_cut(old.host);
            hold(old.host);
        }
    }
    Iterator it;
    begin(&it, self, 107);
    void *p;
    int n = 0;
    while (n < WEB_MAX && (p = next(&it)))
    {
        localWebs[n++] = (LocalWeb){instance_id(p),   is_tie_web(instance_id(p)), read_field(p, V_ox),
                                    read_field(p, V_oy), read_field(p, V_ax),   read_field(p, V_ay)};
    }
    static Binding fresh[WEB_MAX];
    int count = 0;
    table_clear(&localById);
    table_clear(&bindingByHost);
    for (int j = 0; j < n; j++)
        table_put(&localById, localWebs[j].id, j);
    for (int j = 0; j < webCount; j++)
        table_put(&bindingByHost, webBindings[j].host, webBindings[j].local);
    for (unsigned i = 0; i < w->numWebs; i++)
    {
        Strand *s = &w->web[i];
        if (held(s->id))
            continue;
        int target = table_get(&bindingByHost, s->id, -4);
        int match = target >= 100000 ? table_get(&localById, target, -1) : -1;
        if (match >= 0 && localWebs[match].used)
            match = -1;
        if (match < 0)
        {
            /* A strand seen for the first time: pair it with a local web at the same place, if any. */
            double minCost = 100;
            for (int j = 0; j < n; j++)
            {
                LocalWeb *l = &localWebs[j];
                if (l->used)
                    continue;
                double cost = fabs(l->ox - s->props[0]) + fabs(l->oy - s->props[1]) + fabs(l->ax - s->props[2]) +
                              fabs(l->ay - s->props[3]);
                if (cost < minCost)
                {
                    match = j;
                    minCost = cost;
                }
            }
        }
        if (match >= 0)
        {
            localWebs[match].used = 1;
            p = find_id(self, localWebs[match].id);
        }
        else
        {
            /* Real web endpoints can be scenery, movable physics bodies, or native web nodes. */
            int o = resolve_anchor(self, s->o), a = resolve_anchor(self, s->a);
            if ((s->o.type >= 0 && o < 0) || (s->a.type >= 0 && a < 0))
                continue;
            GameString layer;
            Val id = FN(self, instance_create_layer, num(s->props[0]), num(s->props[1]), str(&layer, "Webs"), num(107));
            p = find_id(self, (int)value(id));
            if (!p)
                continue;
            *field(p, V_o) = num(o);
            *field(p, V_a) = num(a);
        }
        if (!p)
            continue;
        if (match < 0)
        {
            for (int j = 0; j < 12; j++)
                *field(p, webFields[j]) = num(s->props[j]);
        }
        else
        {
            *field(p, V_length) = num(s->props[8]);
        }
        set_coord(p, B_image_blend, s->colour);
        Val id = num(instance_id(p));
        if (match < 0)
            script(p, 0x4df040, 1, &id);
        fresh[count++] = (Binding){s->id, (int)value(id)};
    }
    if (w->complete)
    {
        table_clear(&incomingIds);
        for (unsigned j = 0; j < w->numWebs; j++)
            table_put(&incomingIds, w->web[j].id, 1);
    }
    if (w->complete)
        for (int i = 0; i < webCount; i++)
        {
            Binding old = webBindings[i];
            if (old.local < 100000)
                continue;
            if (!table_get(&incomingIds, old.host, 0))
            {
                p = find_id(self, old.local);
                if (p && object_type(p) == 107)
                    FN0(p, instance_destroy);
            }
        }
    memcpy(webBindings, fresh, count * sizeof(Binding));
    webCount = count;
    for (unsigned i = 0; i < w->numBodies; i++)
    {
        if (w->bodies[i].type == 101 || w->bodies[i].type == 105 || w->bodies[i].type == 106 ||
            w->bodies[i].id > LAST_PLACED_ID)
            continue;
        Body *b = &w->bodies[i];
        p = find_id(self, b->id);
        if (!p || object_type(p) != b->type || !mobile_type(b->type))
            continue;
        if (hypot(coord(p, B_x) - b->x, coord(p, B_y) - b->y) > .75 || fabs(coord(p, B_phy_rotation) - b->angle) > 2)
        {
            set_coord(p, B_phy_position_x, b->x);
            set_coord(p, B_phy_position_y, b->y);
            set_coord(p, B_phy_rotation, b->angle);
            set_coord(p, B_phy_speed_x, b->vx);
            set_coord(p, B_phy_speed_y, b->vy);
            set_coord(p, B_phy_angular_velocity, b->spin);
        }
    }
}
static void sync_world(void *self)
{
    if (bus->role)
        receive_cuts(self);
    if (!worldData || !same_room() || !remote)
    {
        knownRoom = ~0u;
        if (bus->role == HOST)
            tieIdCount = 0; /* the guest left: what it spun here stays as part of the room */
        return;
    }
    /* Pausing deactivates every instance: a world read now would have no webs, and one applied now
       would duplicate webs that are only switched off. Wait for both games to be running. */
    if (localPaused || incoming.active == SPIDER_PAUSED)
        return;
    if (bus->role == HOST && menuTick % 4 == 0)
        publish_world(self);
    if (bus->role == GUEST)
    {
        apply_world(self);
        if (menuTick % 4 == 2)
            publish_ties(self);
    }
    else
        apply_ties(self);
}
static void open_world_map(void)
{
    wchar_t name[80];
    swprintf(name, 80, L"Local\\WebbedWorld_%lu", GetCurrentProcessId());
    worldMapping = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(World), name);
    if (worldMapping)
        worldData = MapViewOfFile(worldMapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(World));
    swprintf(name, 80, L"Local\\WebbedTies_%lu", GetCurrentProcessId());
    tiesMapping = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(Ties), name);
    if (tiesMapping)
        tiesData = MapViewOfFile(tiesMapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Ties));
    swprintf(name, 80, L"Local\\WebbedCuts_%lu", GetCurrentProcessId());
    cutsMapping = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(Cuts), name);
    if (cutsMapping)
        cutsData = MapViewOfFile(cutsMapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Cuts));
}
