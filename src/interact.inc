/* Spider-to-spider interaction. Webbed's movement, webs and grapple are hand-written on top of
   raycasts and collision checks, not GameMaker physics, so the friend's spider takes part by
   being visible to those checks. */
#define S_raycast 0x430e60
static Script raycastOriginal;

static void describe(char *out, size_t n, void *self, Val v)
{
    int k = v.kind & 0xffffff;
    if (k == 1 && v.str && v.str->text)
        snprintf(out, n, "s'%s'", v.str->text);
    else if (k == 2)
    {
        int len = (int)value(FN(self, array_length, v));
        int w = snprintf(out, n, "arr%d[", len);
        for (int i = 0; i < len && i < 6 && w < (int)n - 24; i++)
        {
            Val e = FN(self, array_get, v, num(i));
            w += snprintf(out + w, n - w, "%s%d:%.1f", i ? "," : "", e.kind & 0xffffff, value(e));
            drop(&e);
        }
        snprintf(out + w, n - w, "]");
    }
    else
        snprintf(out, n, "k%d:%.2f", k, value(v));
}

/* The other spider as seen by `self`: on each machine the local player sees the remote copy of
   the friend, and the remote copy (replaying the friend's buttons) sees the local player. */
static void *other_spider(void *self)
{
    if (!bus || bus->status != CONNECTED || !player || !remote || !same_room())
        return NULL;
    /* Only the real owner of a spider acts on the friend: the remote copy (replaying the friend's
       buttons) never touches the local spider. What the friend does to us arrives as their own
       result instead (their webs via the tie channel and world sync, their pushes via positions),
       so nothing is solved twice and the pair is not flung around. */
    if (self == player)
        return remote;
    return NULL;
}
#define SPIDER_RADIUS 7.0
#define OBJ_oSolid 71
/* Only queries for solid ground count the friend in: the same checks with other objects (a fly
   to eat, a pickup) must never get a spider back. */
static int solid_query(void *self, Val objects)
{
    if (isnum(objects))
        return (int)value(objects) == OBJ_oSolid;
    if (!value(FN(self, is_array, objects)))
        return 0;
    int n = (int)value(FN(self, array_length, objects)), found = 0;
    for (int i = 0; i < n && !found; i++)
    {
        Val e = FN(self, array_get, objects, num(i));
        found = (int)value(e) == OBJ_oSolid;
        drop(&e);
    }
    return found;
}
/* Where the segment a->b first enters the circle, as a fraction of its length (or -1). */
static double segment_circle(double ax, double ay, double bx, double by, double cx, double cy, double r)
{
    double dx = bx - ax, dy = by - ay, fx = ax - cx, fy = ay - cy;
    double a = dx * dx + dy * dy, b = 2 * (fx * dx + fy * dy), c = fx * fx + fy * fy - r * r;
    if (a < 1e-9)
        return c <= 0 ? 0 : -1;
    double disc = b * b - 4 * a * c;
    if (disc < 0)
        return -1;
    double sq = sqrt(disc), t1 = (-b - sq) / (2 * a), t2 = (-b + sq) / (2 * a);
    if (t1 >= 0 && t1 <= 1)
        return t1;
    if (t1 < 0 && t2 >= 0)
        return 0; /* starts inside */
    return -1;
}
/* The same for a square of half-size r: a flat back, so a spider can stand on the friend the way
   it stands on a box instead of sliding off a ball. */
static double segment_box(double ax, double ay, double bx, double by, double cx, double cy, double r)
{
    double lo = 0, hi = 1, d[2] = {bx - ax, by - ay}, s[2] = {ax - cx, ay - cy};
    for (int k = 0; k < 2; k++)
    {
        if (fabs(d[k]) < 1e-9)
        {
            if (fabs(s[k]) > r)
                return -1;
            continue;
        }
        double t1 = (-r - s[k]) / d[k], t2 = (r - s[k]) / d[k];
        if (t1 > t2)
        {
            double t = t1;
            t1 = t2, t2 = t;
        }
        lo = fmax(lo, t1), hi = fmin(hi, t2);
        if (lo > hi)
            return -1;
    }
    return lo;
}
static int webs_on(void *self, void *spider)
{
    double id = coord(spider, B_id);
    int n = 0;
    Iterator it;
    void *web;
    begin(&it, self, 107);
    while ((web = next(&it)))
        n += value(*field(web, V_o)) == id || value(*field(web, V_a)) == id;
    return n;
}
/* The friend is something to aim at, never something to bump into: only while the grapple or the
   weave is held, and only for long (aiming) rays. */
static int aiming(void)
{
    return io && (read_field(io, V_grapple) != 0 || read_field(io, V_weave) != 0);
}
static Val *coop_raycast(void *self, void *other, Val *ret, int argc, Val **argv)
{
    Val *r = raycastOriginal(self, other, ret, argc, argv);
    void *friendSpider = aiming() ? other_spider(self) : NULL;
    if (friendSpider && argc >= 4 &&
        hypot(value(*argv[2]) - value(*argv[0]), value(*argv[3]) - value(*argv[1])) < 96)
        friendSpider = NULL;
    if (friendSpider && argc >= 5 && solid_query(self, *argv[4]))
    {
        double ax = value(*argv[0]), ay = value(*argv[1]), bx = value(*argv[2]), by = value(*argv[3]);
        /* Short rays are the spider's own feet and sides. Each spider's body is solved on its
           owner's machine only: the remote copy (replaying the friend's buttons) walks through the
           local spider, or both machines would push the pair against each other and fling them.
           Long rays are aiming: those see the other spider everywhere, so a grapple or web on a
           friend lands the same on both screens. */
        /* A friend moves a little all the time, so a held weave would tie a strand to them on
           every frame: three strands on one spider is plenty. */

    }
    if (friendSpider && argc >= 5 && solid_query(self, *argv[4]))
    {
        double ax = value(*argv[0]), ay = value(*argv[1]), bx = value(*argv[2]), by = value(*argv[3]);
        double cx = coord(friendSpider, B_x), cy = coord(friendSpider, B_y);
        /* Feet and sides meet a flat box; aiming (long rays) the round body. */
        /* Spiders pass through each other (feet and sides never meet the friend); aiming does. */
        double t = hypot(bx - ax, by - ay) < 48 ? -1 : segment_circle(ax, ay, bx, by, cx, cy, SPIDER_RADIUS);
        if (t >= 0)
        {
            double hx = ax + (bx - ax) * t, hy = ay + (by - ay) * t, hitDist = hypot(hx - ax, hy - ay);
            double oldDist = 1e30;
            if (value(FN(self, is_array, *r)) && value(FN(self, array_length, *r)) >= 3)
            {
                Val e = FN(self, array_get, *r, num(2));
                if (value(e) >= 100000)
                {
                    Val x = FN(self, array_get, *r, num(0)), y = FN(self, array_get, *r, num(1));
                    oldDist = hypot(value(x) - ax, value(y) - ay);
                }
                drop(&e);
            }
            if (hitDist < oldDist)
            {
                Val hit = FN(self, array_create, num(3));
                FN(self, array_set, hit, num(0), num(hx));
                FN(self, array_set, hit, num(1), num(hy));
                FN(self, array_set, hit, num(2), num(coord(friendSpider, B_id)));
                drop(r);
                *r = hit;
            }
        }
    }
#ifdef NET_QA
    static int logged;
    if (logged < 40 && self && (self == player || self == remote))
    {
        logged++;
        char line[600], part[160];
        int w = snprintf(line, sizeof(line), "raycast by %s argc=%d:", self == player ? "player" : "remote", argc);
        for (int i = 0; i < argc && i < 8; i++)
        {
            describe(part, sizeof(part), self, *argv[i]);
            w += snprintf(line + w, sizeof(line) - w, " %s", part);
        }
        describe(part, sizeof(part), self, *r);
        snprintf(line + w, sizeof(line) - w, " => %s", part);
        log_line(line);
    }
#endif
    return r;
}
#define F_collision_circle_impl 0x11a9f40
static Builtin collisionCircleOriginal;
/* collision_circle(x, y, r, obj, prec, notme): weaving anchors a web's end on the solid found
   around the spider, so the friend's spider counts as solid here too. */
static void coop_collision_circle(Val *ret, void *self, void *other, int argc, Val *args)
{
    collisionCircleOriginal(ret, self, other, argc, args);
    void *friendSpider = io && read_field(io, V_weave) != 0 ? other_spider(self) : NULL;
    if (!friendSpider || argc < 4 || value(*ret) >= 100000 || (int)value(args[3]) != OBJ_oSolid)
        return;

    double d = hypot(coord(friendSpider, B_x) - value(args[0]), coord(friendSpider, B_y) - value(args[1]));
    if (d <= value(args[2]) + SPIDER_RADIUS)
        *ret = num(coord(friendSpider, B_id));
}
static int install_interaction_hooks(void)
{
    static const unsigned char circleBody[] = {0x83, 0xec, 0x14, 0x56, 0x57};
    int ok = hook_script(S_raycast, coop_raycast, &raycastOriginal);
    if (!hook_code(F_collision_circle_impl, circleBody, coop_collision_circle, (void **)&collisionCircleOriginal))
        log_line("coop: collision_circle hook unavailable");
    return ok;
}

/* ---- Hugs and dancing together: small pixel hearts in the game's palette. ---- */
typedef struct
{
    double x, y, vx, vy;
    int life;
} Heart;
static Heart hearts[48];
static void spawn_hearts(double x, double y, int n)
{
    for (int i = 0; i < 48 && n > 0; i++)
        if (hearts[i].life <= 0)
        {
            hearts[i] = (Heart){x + (rand() % 13 - 6), y - 10, (rand() % 9 - 4) * .08, -.45 - (rand() % 5) * .08, 70};
            n--;
        }
}
static unsigned hugCooldown, danceTimer;
static void hearts_step(void *self)
{
    for (int i = 0; i < 48; i++)
        if (hearts[i].life > 0)
        {
            hearts[i].x += hearts[i].vx;
            hearts[i].y += hearts[i].vy;
            hearts[i].life--;
        }
    if (!player || !remote || !same_room() || incoming.active != SPIDER_PLAYING)
        return;
    double px = coord(player, B_x), py = coord(player, B_y), fx = coord(remote, B_x), fy = coord(remote, B_y);
    double d = hypot(px - fx, py - fy);
    if (hugCooldown)
        hugCooldown--;
    /* F next to the friend (either of you): a hug. Both games see both presses. */
    int localHug = read_field(self, V_interact) != 0, friendHug = bit(remotePress, 5);
    if (!hugCooldown && d < 30 && (localHug || friendHug))
    {
        spawn_hearts(px, py, 4);
        spawn_hearts(fx, fy, 4);
        hugCooldown = 50;
    }
    /* Both dancing close together. */
    if (d < 80 && read_field(self, V_dance) != 0 && bit(incoming.buttons, 4))
    {
        if (++danceTimer % 24 == 0)
        {
            spawn_hearts(px, py, 1);
            spawn_hearts(fx, fy, 1);
        }
    }
}
static void draw_hearts(void *self)
{
    static const char *shape[] = {".X.X.", "XXXXX", "XXXXX", ".XXX.", "..X.."};
    Val camera = FN(self, view_get_camera, num(0));
    double cw = value(FN(self, camera_get_view_width, camera)), ch = value(FN(self, camera_get_view_height, camera));
    double w = value(FN0(self, display_get_gui_width)), h = value(FN0(self, display_get_gui_height));
    if (cw <= 0 || ch <= 0)
        return;
    double cx = value(FN(self, camera_get_view_x, camera)), cy = value(FN(self, camera_get_view_y, camera));
    double px = w / cw; /* one game pixel in GUI units */
    Val alpha = FN0(self, draw_get_alpha);
    for (int i = 0; i < 48; i++)
    {
        if (hearts[i].life <= 0)
            continue;
        double gx = (hearts[i].x - cx) * w / cw, gy = (hearts[i].y - cy) * h / ch;
        FN(self, draw_set_alpha, num(fmin(1, hearts[i].life / 20.0)));
        for (int r = 0; r < 5; r++)
            for (int c = 0; c < 5; c++)
                if (shape[r][c] == 'X')
                {
                    int edge = (r == 0) || (c == 0 || c == 4) || (r == 4) || (r == 3 && (c == 1 || c == 3));
                    int colour = edge ? 0x4a2fd0 : 0x8f6cff; /* BGR: deep pink rim, pink fill */
                    double x0 = gx + (c - 2.5) * px, y0 = gy + (r - 2.5) * px;
                    FN(self, draw_rectangle_color, num(x0), num(y0), num(x0 + px - .01), num(y0 + px - .01), num(colour),
                       num(colour), num(colour), num(colour), num(0));
                }
    }
    FN(self, draw_set_alpha, alpha);
}
static void webs_step(void *self);
static void interaction_step(void *self)
{
    webs_step(self);
    hearts_step(self);
}
