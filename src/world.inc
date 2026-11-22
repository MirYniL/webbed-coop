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
        Strand *s = &worldData->web[worldData->numWebs++];
        s->id = instance_id(p);
        s->colour = (int)coord(p, B_image_blend);
        s->o = endpoint(self, *field(p, V_o));
        s->a = endpoint(self, *field(p, V_a));
        for (int j = 0; j < 12; j++)
            s->props[j] = (float)read_field(p, webFields[j]);
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
    if (entering)
    {
        nodeCount = webCount = 0;
        knownRoom = w->room;
    }
    Iterator it;
    begin(&it, self, 107);
    void *p;
    int n = 0;
    while (n < WEB_MAX && (p = next(&it)))
    {
        localWebs[n++] = (LocalWeb){
            instance_id(p), 0, read_field(p, V_ox), read_field(p, V_oy), read_field(p, V_ax), read_field(p, V_ay)};
    }
    Binding fresh[WEB_MAX];
    int count = 0;
    for (unsigned i = 0; i < w->numWebs; i++)
    {
        Strand *s = &w->web[i];
        int match = -1, target = bound(webBindings, webCount, s->id);
        double minCost = 100;
        for (int j = 0; j < n; j++)
        {
            LocalWeb *l = &localWebs[j];
            if (l->used)
                continue;
            if (l->id == target)
            {
                match = j;
                break;
            }
            double cost = fabs(l->ox - s->props[0]) + fabs(l->oy - s->props[1]) + fabs(l->ax - s->props[2]) +
                          fabs(l->ay - s->props[3]);
            if (cost < minCost)
            {
                match = j;
                minCost = cost;
            }
        }
        if (match < 0 && (entering || target == -1))
        {
            fresh[count++] = (Binding){s->id, -1};
            continue;
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
        for (int i = 0; i < webCount; i++)
        {
            Binding old = webBindings[i];
            if (old.local < 100000)
                continue;
            int present = 0;
            for (unsigned j = 0; j < w->numWebs; j++)
                if (w->web[j].id == old.host)
                {
                    present = 1;
                    break;
                }
            if (!present)
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
    if (!worldData || !same_room() || !remote)
    {
        knownRoom = ~0u;
        return;
    }
    if (bus->role == HOST && menuTick % 8 == 0)
        publish_world(self);
    if (bus->role == GUEST)
        apply_world(self);
}
static void open_world_map(void)
{
    wchar_t name[80];
    swprintf(name, 80, L"Local\\WebbedWorld_%lu", GetCurrentProcessId());
    worldMapping = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(World), name);
    if (worldMapping)
        worldData = MapViewOfFile(worldMapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(World));
}
