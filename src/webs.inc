/* Webs between the two games, as the game itself treats webs on bugs.

   Webbed's webs do not hold what they stick to with joints or springs. A bug with webs on it gets
   heavier damping the more webs it has (oGrabbable: phy_linear_damping) and stops steering itself
   (npc_move), a web pulls only while the spider reels it in (pull_power), it snaps when stretched
   too far (snap_web), and a shot splits it in two (oBullet -> split_web). The hooks below make the
   friend's spider and the friend's webs take part in exactly those rules. */
#define S_split_web 0x4cb6a0
#define S_snap_web 0x4d6a90
static Script splitOriginal, snapOriginal;
static Val no_pieces(void *self);

static int spider_id(double id)
{
    return (player && id == coord(player, B_id)) || (remote && id == coord(remote, B_id));
}
static int on_spider(void *web)
{
    return spider_id(value(*field(web, V_o))) || spider_id(value(*field(web, V_a)));
}
/* A web on one of the two spiders does not snap by itself: the friend's position always lags a
   little, and the two of you would lose every strand. A shot still splits it. */
static Val *coop_snap(void *self, void *other, Val *ret, int argc, Val **argv)
{
    if (bus && bus->status == CONNECTED && argc >= 1)
    {
        void *web = find_id(self, (int)value(*argv[0]));
        /* A strand on a spider does not snap by itself, and one the friend owns snaps only in the
           friend's game (the pieces come with the sync), or it would snap twice. */
        if (web && object_type(web) == 107 && (on_spider(web) || (same_room() && !owned_here(instance_id(web)))))
        {
            *ret = no_pieces(self);
            return ret;
        }
    }
    return snapOriginal(self, other, ret, argc, argv);
}
static int owned_here(int id);
static void request_split(int id, double x, double y);
/* A shot that hits a strand the friend owns asks the friend's game to split it there; the halves
   come back through the usual sync, so both games always hold the same strands. */
static int splitting;
/* Shots fired by our copy of the friend's spider (its grapple shot is an oBullet): they fly and
   land as on the friend's screen, but only the friend's own shot cuts, in the friend's game. */
#define OBJ_oBullet 99
#define OBJ_oBullet_collides_platforms 100
static int puppetBullets[16], puppetBulletNext;
static int newest_bullet(void *self)
{
    int newest = 0;
    for (int type = OBJ_oBullet; type <= OBJ_oBullet_collides_platforms; type++)
    {
        Iterator it;
        void *p;
        begin(&it, self, type);
        while ((p = next(&it)))
            if (instance_id(p) > newest)
                newest = instance_id(p);
    }
    return newest;
}
static void mark_puppet_bullets(void *self, int before)
{
    for (int type = OBJ_oBullet; type <= OBJ_oBullet_collides_platforms; type++)
    {
        Iterator it;
        void *p;
        begin(&it, self, type);
        while ((p = next(&it)))
            if (instance_id(p) > before)
                puppetBullets[puppetBulletNext++ % 16] = instance_id(p);
    }
}
static int puppet_bullet(void *p)
{
    if (!p)
        return 0;
    int id = instance_id(p);
    for (int i = 0; i < 16; i++)
        if (puppetBullets[i] == id)
            return 1;
    return 0;
}
/* What a split returns to the bullet: its two new pieces. None here (the bullet pushes them). */
static Val no_pieces(void *self)
{
    return FN(self, array_create, num(2), num(-4));
}
static Val *coop_split(void *self, void *other, Val *ret, int argc, Val **argv)
{
#ifdef NET_QA
    {
        char line[400], part[120];
        int w = snprintf(line, sizeof(line), "split_web self=%d(%d) argc=%d:", instance_id(self), object_type(self), argc);
        for (int i = 0; i < argc && i < 4; i++)
        {
            describe(part, sizeof(part), self, *argv[i]);
            w += snprintf(line + w, sizeof(line) - w, " %s", part);
        }
        log_line(line);
    }
#endif
    if (!splitting && bus && bus->status == CONNECTED && same_room() && puppet_bullet(other))
    {
        *ret = no_pieces(self);
        return ret;
    }
    if (!splitting && bus && bus->status == CONNECTED && same_room() && argc >= 2)
    {
        void *web = find_id(self, (int)value(*argv[0]));
        if (web && object_type(web) == 107 && !owned_here(instance_id(web)) && value(FN(self, is_array, *argv[1])))
        {
            Val px = FN(self, array_get, *argv[1], num(0)), py = FN(self, array_get, *argv[1], num(1));
            request_split(instance_id(web), value(px), value(py));
            /* Cut our copy right away, as the owner will: the pieces here stand in until the owner's
               own pieces arrive and take their place. */
            int before = newest_web(self);
            mark_provisional(instance_id(web), 1);
            splitting = 1;
            Val *r = splitOriginal(self, other, ret, argc, argv);
            splitting = 0;
            mark_new_provisional(self, before);
            return r;
        }
    }
    return splitOriginal(self, other, ret, argc, argv);
}
/* The owner's side of a split request: split our strand at that point with the game's own code. */
static void split_here(void *self, int id, double x, double y)
{
    void *web = find_id(self, id);
    /* A strand already gone this frame (the game removes snapped halves) has no variables left. */
    if (!web || object_type(web) != 107 || !value(FN(self, instance_exists, num(id))))
        return;
    Val point = FN(self, array_create, num(2));
    FN(self, array_set, point, num(0), num(x));
    FN(self, array_set, point, num(1), num(y));
    Val a0 = num(id), r = undef();
    Val *args[] = {&a0, &point};
    splitting = 1;
    splitOriginal(web, web, &r, 2, args);
    splitting = 0;
    drop(&r);
    drop(&point);
}

/* ---- Pushes. Whatever the game does to our copy of the friend's spider with physics (our
   grapple or reeled web pulling it, a shot hitting it) is a push meant for the friend's real
   spider: it is added up here and the friend's game applies it, through the same physics calls,
   to the spider that game owns. Forces count for one physics step (1/60 s). pushX/pushY (declared
   early, network_frame sends them) hold all pushes given to the friend's copy so far. ---- */
#define F_physics_apply_force_impl 0x131b9f0
#define F_physics_apply_impulse_impl 0x131bad0
static Builtin forceOriginal, impulseOriginal;
static double gotX, gotY; /* the friend's totals already applied here */
static int gotValid;
/* The friend's copy pulling our own spider (its grapple on us): the friend's game sends that pull
   itself, so it must not count twice. */
static int puppet_on_player(void *self)
{
    return puppetEvent && self && self == player;
}
static void coop_force(Val *ret, void *self, void *other, int argc, Val *args)
{
    if (puppet_on_player(self))
    {
        *ret = undef();
        return;
    }
    if (self && self == remote && argc >= 4)
    {
        if (!puppetEvent)
            pushX += value(args[2]) / 60, pushY += value(args[3]) / 60;
        *ret = undef();
        return;
    }
    forceOriginal(ret, self, other, argc, args);
}
static void coop_impulse(Val *ret, void *self, void *other, int argc, Val *args)
{
    if (puppet_on_player(self))
    {
        *ret = undef();
        return;
    }
    if (self && self == remote && argc >= 4)
    {
        if (!puppetEvent)
            pushX += value(args[2]), pushY += value(args[3]);
        *ret = undef();
        return;
    }
    impulseOriginal(ret, self, other, argc, args);
}
static void receive_pushes(void)
{
    if (!player || !remote || !same_room() || incoming.active != SPIDER_PLAYING)
    {
        gotValid = 0;
        return;
    }
    if (!gotValid || !isfinite(incoming.ix) || !isfinite(incoming.iy))
    {
        gotX = incoming.ix, gotY = incoming.iy, gotValid = 1;
        return;
    }
    double dx = incoming.ix - gotX, dy = incoming.iy - gotY;
    gotX = incoming.ix, gotY = incoming.iy;
    if (fabs(dx) + fabs(dy) < 1e-9 || fabs(dx) + fabs(dy) > 1e5)
        return;
    /* A single huge push (both pulling at once, a late burst of frames) would fling the spider. */
    double size = hypot(dx, dy);
    if (size > 2)
        dx *= 2 / size, dy *= 2 / size;
    Val r = undef(), args[] = {num(coord(player, B_x)), num(coord(player, B_y)), num(dx), num(dy)};
    impulseOriginal(&r, player, player, 4, args);
}
static int install_push_hooks(void)
{
    static const unsigned char body[] = {0x8b, 0x44, 0x24, 0x08, 0x83, 0xec, 0x0c};
    int ok = hook_code_n(F_physics_apply_force_impl, body, 7, coop_force, (void **)&forceOriginal);
    ok &= hook_code_n(F_physics_apply_impulse_impl, body, 7, coop_impulse, (void **)&impulseOriginal);
    return ok;
}

/* ---- Webs the friend tied to this player's spider. A bug with webs on it is damped harder per
   web and cannot run off; a spider tied by the friend gets the same: heavier going per strand,
   and a strand that is already taut does not stretch further (it pulls back softly, through the
   speed the physics uses, never by moving the spider). The friend reeling it in arrives as pushes. */
static void webs_on_me(void *self)
{
    if (!player || !bus || bus->status != CONNECTED || !remote || !same_room())
        return;
    double me = coord(player, B_id);
    double px = coord(player, B_x), py = coord(player, B_y);
    double vx = coord(player, B_phy_speed_x), vy = coord(player, B_phy_speed_y);
    int n = 0;
    Iterator it;
    void *web;
    begin(&it, self, 107);
    while ((web = next(&it)))
    {
        double o = value(*field(web, V_o)), a = value(*field(web, V_a));
        int isO = o == me, isA = a == me;
        if ((!isO && !isA) || owned_here(instance_id(web)))
            continue;
        n++;
        double tx = read_field(web, isO ? V_ax : V_ox), ty = read_field(web, isO ? V_ay : V_oy);
        double length = read_field(web, V_length), dist = hypot(tx - px, ty - py);
        if (length <= 0 || dist <= length || dist < 1e-6)
            continue;
        double nx = (tx - px) / dist, ny = (ty - py) / dist, outward = -(vx * nx + vy * ny);
        if (outward > 0)
            vx += nx * outward, vy += ny * outward;
        double back = fmin((dist - length) * .05, 1.5);
        vx += nx * back, vy += ny * back;
    }
    if (!n)
        return;
    double keep = 1 / (1 + .03 * fmin(n, 6));
    set_coord(player, B_phy_speed_x, vx * keep);
    set_coord(player, B_phy_speed_y, vy * keep);
}

static void webs_step(void *self)
{
    receive_pushes();
    webs_on_me(self);
}
static int install_web_hooks(void)
{
    int ok = hook_script(S_snap_web, coop_snap, &snapOriginal);
    ok &= hook_script(S_split_web, coop_split, &splitOriginal);
    ok &= install_push_hooks();
    return ok;
}
