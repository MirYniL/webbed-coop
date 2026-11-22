/* "Co-op" lives in Webbed's own menus: an extra entry in the title and pause menu (oMainMenu),
   and a submenu that is a plain oMenu instance, so the game draws it with its own font, buttons,
   tooltips, cursor, sounds and slide animation, exactly like Settings. */
#define V_option_labels 0x18a68
#define V_option_tooltips 0x18a6a
#define V_option_scripts 0x18a69
#define V_option_types 0x18a6b
#define V_option_cursor 0x18a67
#define V_controls_active 0x187c6
#define V_ui_b 0x18ced
#define F_variable_instance_get 0x11ca130
#define F_variable_instance_set 0x11ca650
#define F_instance_exists 0x11ab540
#define F_array_create 0x117eb40
#define F_array_get 0x117f090
#define F_array_length 0x117f2b0
#define F_asset_get_index 0x1304840
#define F_draw_sprite_ext 0x11d4a00
#define F_gpu_set_fog 0x12e9130
#define F_is_array 0x1180ab0
#define S_get_translation 0x6f0180
#define S_draw_text_lang_shadow 0x6edd00
#define S_open_steam_page 0x515050
#define OBJ_oMenu 333
#define OBJ_oMainMenu 334
#define SPR_sMapMarker 596
#define SPR_sUIArrow 244

static Event createMainMenu;
static double mainMenuId = -1, mainMenuDepth, subId = -1, pendingFrom = -1;
static int pendingCursor, restoreControls, coopScript = -1, language = -1, coopIndex = -1;
static char subKey[512];

static int russian(void *self)
{
    if (language < 0)
    {
        if (!self)
            return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_RUSSIAN;
        Val l = global_get(self, "language");
        char line[120];
        if ((l.kind & 0xffffff) == 1 && l.str && l.str->text)
        {
            snprintf(line, sizeof(line), "coop: game language '%s'", l.str->text);
            language = !_strnicmp(l.str->text, "ru", 2) || strstr(l.str->text, "усск") != NULL;
        }
        else
        {
            snprintf(line, sizeof(line), "coop: game language kind %d value %.1f", l.kind, value(l));
            language = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_RUSSIAN;
        }
        log_line(line);
        drop(&l);
    }
    return language;
}
static int is_string(Val v)
{
    return (v.kind & 0xffffff) == 1 && v.str && v.str->text;
}
static Val iget(void *p, double id, const char *name)
{
    GameString k;
    return FN(p, variable_instance_get, num(id), str(&k, name));
}
static void iset(void *p, double id, const char *name, Val v)
{
    GameString k;
    FN(p, variable_instance_set, num(id), str(&k, name), v);
}
static int exists(void *p, double id)
{
    return id >= 0 && value(FN(p, instance_exists, num(id))) != 0;
}
/* Tab 0 of a menu option array, rebuilt with `extra` inserted at `at` (at < 0: no insert). */
static Val tab_with(void *p, Val outer, int at, Val extra)
{
    Val inner = FN(p, array_get, outer, num(0));
    int n = (int)value(FN(p, array_length, inner));
    Val fresh = FN(p, array_create, num(n + (at >= 0)));
    for (int i = 0, j = 0; i < n; i++, j++)
    {
        if (i == at)
            FN(p, array_set, fresh, num(j++), extra);
        Val v = FN(p, array_get, inner, num(i));
        FN(p, array_set, fresh, num(j), v);
        drop(&v);
    }
    drop(&inner);
    Val wrap = FN(p, array_create, num(1));
    FN(p, array_set, wrap, num(0), fresh);
    drop(&fresh);
    return wrap;
}
static void set_tab(void *p, double id, const char *name, int at, Val extra)
{
    Val outer = iget(p, id, name);
    if (!value(FN(p, is_array, outer)))
    {
        drop(&outer);
        return;
    }
    Val wrap = tab_with(p, outer, at, extra);
    drop(&outer);
    iset(p, id, name, wrap);
    drop(&wrap);
}
static void coop_script_id(void *self);
static void on_main_menu_create(void *self, void *other)
{
    createMainMenu(self, other);
    log_line("coop: menu created");
    language = -1;
    russian(self);
    coop_script_id(self);
    {
        char line[80];
        snprintf(line, sizeof(line), "coop: script %d, russian %d", coopScript, language);
        log_line(line);
    }
    if (coopScript < 0)
        return;
    mainMenuId = coord(self, B_id);
    mainMenuDepth = coord(self, B_depth);
    /* Insert "Co-op" right before "Settings" in the title and pause menus. */
    Val labels = *field(self, V_option_labels);
    Val inner = FN(self, array_get, labels, num(0));
    int n = (int)value(FN(self, array_length, inner)), at = n > 1 ? n - 1 : 0;
    for (int i = 0; i < n; i++)
    {
        Val v = FN(self, array_get, inner, num(i));
        if (is_string(v) && !strcmp(v.str->text, "Settings"))
            at = i;
        drop(&v);
    }
    drop(&inner);
    GameString a, b;
    coopIndex = at;
    set_tab(self, mainMenuId, "option_labels", at, str(&a, T("Кооператив", "Co-op")));
    set_tab(self, mainMenuId, "option_tooltips", at,
            str(&b, T("Играть вдвоём через Steam: у каждого свой паук, сюжет общий",
                      "Play together over Steam: a spider each, one shared story")));
    set_tab(self, mainMenuId, "option_scripts", at, num(coopScript));
    set_tab(self, mainMenuId, "option_types", at, num(1)); /* 1 = button */
    clear_texts();
}
/* Stand-in for open_steam_page (only the demo build uses it): every co-op button runs this. */
static Val *coop_script(void *self, void *other, Val *ret, int argc, Val **argv)
{
    (void)other, (void)argc, (void)argv;
    pendingFrom = coord(self, B_id);
    pendingCursor = (int)read_field(self, V_option_cursor);
    return ret;
}
enum
{
    ITEM_HOST,
    ITEM_PASTE,
    ITEM_FIND,
    ITEM_COPY,
    ITEM_LEAVE,
    ITEM_BACK
};
static int items[6], itemCount;
/* A short message about the button just pressed; it replaces the hint line for a while. */
static char notice[192];
static DWORD noticeAt;
static char copiedFor[192];
static void say(const char *text)
{
    snprintf(notice, sizeof(notice), "%s", text);
    noticeAt = GetTickCount();
}
static int invite_ready(void)
{
    return bus->role == HOST && bus->status != CONNECTING && valid_invite(bus->invite);
}
static void build_items(void)
{
    itemCount = 0;
    if (!bus->role)
    {
        if (player && !guestSlot)
            items[itemCount++] = ITEM_HOST;
        items[itemCount++] = ITEM_PASTE;
        items[itemCount++] = ITEM_FIND;
    }
    else if (bus->role == HOST)
    {
        items[itemCount++] = ITEM_COPY;
        items[itemCount++] = ITEM_LEAVE;
    }
    else
        items[itemCount++] = ITEM_LEAVE;
    items[itemCount++] = ITEM_BACK;
}
static const char *item_label(int it)
{
    switch (it)
    {
    case ITEM_HOST:
        return T("Открыть мир для друга", "Open world to a friend");
    case ITEM_PASTE:
        return T("Вставить приглашение", "Paste invite");
    case ITEM_FIND:
        return T("Найти друга в Steam", "Find a friend on Steam");
    case ITEM_COPY:
        return T("Скопировать приглашение", "Copy invite");
    case ITEM_LEAVE:
        if (bus->role == HOST)
            return T("Закрыть мир", "Close world");
        return bus->status == CONNECTED ? T("Покинуть мир друга", "Leave friend's world") : T("Отменить", "Cancel");
    default:
        return T("Назад", "Back");
    }
}
/* What is going on right now (line 1) and what to do next (line 2). */
static void status_lines(const char **head, const char **hint)
{
    int connected = bus->status == CONNECTED;
    if (bus->role == HOST)
    {
        *head = connected      ? T("Друг в игре", "Your friend is in")
                : invite_ready() ? T("Мир открыт · ждём друга", "World open · waiting for your friend")
                                 : T("Запускаю Porthole... (5-20 секунд)", "Starting Porthole... (5-20 seconds)");
        *hint = connected      ? ""
                : invite_ready() ? T("Приглашение уже в буфере обмена: вставьте его другу в чат (Ctrl+V)",
                                     "The invite is on the clipboard: paste it to your friend (Ctrl+V)")
                                 : "";
    }
    else if (bus->role == GUEST)
    {
        *head = connected             ? T("Вы в мире друга", "You are in your friend's world")
                : bus->status_text[0] ? bus->status_text
                                      : T("Подключаюсь...", "Connecting...");
        *hint = connected ? "" : T("Обычно до 30 секунд", "Usually up to 30 seconds");
    }
    else
    {
        *head = bus->status_text[0] ? bus->status_text : T("Кооператив выключен", "Co-op is off");
        *hint = player && !guestSlot
                    ? T("Хост: «Открыть мир». Гость: скопируйте приглашение и нажмите «Вставить»",
                        "Host: «Open world». Guest: copy the invite, then «Paste invite»")
                    : T("Скопируйте приглашение друга (Ctrl+C) и нажмите «Вставить приглашение»",
                        "Copy your friend's invite (Ctrl+C), then «Paste invite»");
    }
    if (notice[0] && GetTickCount() - noticeAt < 10000)
        *hint = notice;
}
static void refresh_sub(void *self, int force)
{
    build_items();
    char key[64];
    snprintf(key, sizeof(key), "%d|%d|%d|%d|%d", bus->role, bus->status, itemCount, items[0], language);
    if (!force && !strcmp(key, subKey))
        return;
    snprintf(subKey, sizeof(subKey), "%s", key);
    Val labels = FN(self, array_create, num(itemCount)), tips = FN(self, array_create, num(itemCount));
    Val scripts = FN(self, array_create, num(itemCount)), types = FN(self, array_create, num(itemCount));
    for (int i = 0; i < itemCount; i++)
    {
        GameString a, b;
        FN(self, array_set, labels, num(i), str(&a, item_label(items[i])));
        FN(self, array_set, tips, num(i), str(&b, ""));
        FN(self, array_set, scripts, num(i), num(coopScript));
        FN(self, array_set, types, num(i), num(1));
    }
    const char *names[] = {"option_labels", "option_tooltips", "option_scripts", "option_types"};
    Val *arrays[] = {&labels, &tips, &scripts, &types};
    for (int i = 0; i < 4; i++)
    {
        Val wrap = FN(self, array_create, num(1));
        FN(self, array_set, wrap, num(0), *arrays[i]);
        drop(arrays[i]);
        iset(self, subId, names[i], wrap);
        drop(&wrap);
    }
    Val cursor = iget(self, subId, "option_cursor");
    if (value(cursor) >= itemCount)
        iset(self, subId, "option_cursor", num(itemCount - 1));
    drop(&cursor);
}
static void open_sub(void *self)
{
    if (exists(self, subId) || !exists(self, mainMenuId))
        return;
    subId = value(FN(self, instance_create_depth, num(0), num(0), num(mainMenuDepth - 1), num(OBJ_oMenu)));
    subKey[0] = 0;
    notice[0] = 0;
    refresh_sub(self, 1);
    iset(self, subId, "option_cursor", num(0));
    iset(self, mainMenuId, "active", num(0));
    iset(self, mainMenuId, "controls_active", num(0));
}
static void close_sub(void *self)
{
    if (exists(self, subId))
        FN(self, instance_destroy, num(subId));
    subId = -1;
    if (exists(self, mainMenuId))
    {
        iset(self, mainMenuId, "active", num(1));
        restoreControls = 1; /* after Esc is released, or the pause menu would read it too */
    }
}
static DWORD lastAction;
static void sub_action(void *self, int cursor)
{
    build_items();
    /* The buttons change under the cursor (Paste becomes Cancel): a double click or a held
       Enter must not hit the new button straight away. */
    if (GetTickCount() - lastAction < 700)
        return;
    lastAction = GetTickCount();
    {
        char line[80];
        snprintf(line, sizeof(line), "coop: menu item %d of %d", cursor, itemCount);
        log_line(line);
    }
    if (cursor < 0 || cursor >= itemCount)
        return;
    switch (items[cursor])
    {
    case ITEM_HOST:
        copiedFor[0] = 0;
        bus->status_text[0] = 0;
        start_link(HOST);
        break;
    case ITEM_PASTE:
        if (clipboard_invite())
        {
            bus->status_text[0] = 0;
            say(T("Приглашение найдено", "Invite found"));
            start_link(GUEST);
        }
        else
            say(T("В буфере обмена нет приглашения: скопируйте его из сообщения друга (Ctrl+C)",
                  "No invite on the clipboard: copy it from your friend's message (Ctrl+C)"));
        break;
    case ITEM_FIND:
        bus->status_text[0] = 0;
        coop_find();
        break;
    case ITEM_COPY:
        if (invite_ready())
        {
            clipboard_copy(bus->invite);
            snprintf(copiedFor, sizeof(copiedFor), "%s", bus->invite);
            say(T("Скопировано! Вставьте другу в чат (Ctrl+V)", "Copied! Paste it to your friend (Ctrl+V)"));
        }
        else
            say(T("Porthole ещё запускается, подождите пару секунд", "Porthole is still starting, wait a moment"));
        break;
    case ITEM_LEAVE:
        bus->disconnect = 1;
        say(bus->role == HOST ? T("Мир закрыт", "World closed") : T("Выход...", "Leaving..."));
        break;
    default:
        close_sub(self);
    }
}
static void coop_menu_step(void *self)
{
    /* The moment Porthole hands out the invite, it goes to the clipboard by itself. */
    if (invite_ready() && strcmp(copiedFor, bus->invite))
    {
        clipboard_copy(bus->invite);
        snprintf(copiedFor, sizeof(copiedFor), "%s", bus->invite);
        say(T("Приглашение скопировано: вставьте его другу в чат (Ctrl+V)",
              "Invite copied: paste it to your friend (Ctrl+V)"));
    }
    if (pendingFrom >= 0)
    {
        double from = pendingFrom;
        pendingFrom = -1;
        if (from == mainMenuId)
            open_sub(self);
        else if (from == subId)
            sub_action(self, pendingCursor);
    }
    if (subId >= 0)
    {
        if (!exists(self, subId) || !exists(self, mainMenuId))
            close_sub(self);
        else if (read_field(self, V_ui_b) || read_field(self, V_toggle_pause))
            close_sub(self);
        else
            refresh_sub(self, 0);
    }
    if (restoreControls && !read_field(self, V_ui_b) && !read_field(self, V_toggle_pause))
    {
        restoreControls = 0;
        if (exists(self, mainMenuId))
            iset(self, mainMenuId, "controls_active", num(1));
    }
}

/* Text the way the game prints its own HUD lines: pixel font, white with a dark shadow. */
static void shadow_text(void *self, double x, double y, const char *text, int colour, double alpha)
{
    static int font = -1;
    if (font < 0)
    {
        GameString family;
        /* The game's own pixel font, with the Cyrillic range the stock menu glyphs use. */
        font = (int)value(FN(self, font_add, str(&family, "CutePixel.ttf"), num(12), num(0), num(0), num(32), num(1105)));
    }
    Val oldFont = FN0(self, draw_get_font), oldColour = FN0(self, draw_get_color), oldAlpha = FN0(self, draw_get_alpha);
    Val ha = FN0(self, draw_get_halign), va = FN0(self, draw_get_valign);
    FN(self, draw_set_font, num(font));
    FN(self, draw_set_halign, num(1));
    FN(self, draw_set_valign, num(0));
    FN(self, draw_set_alpha, num(alpha));
    GameString t;
    Val v = str(&t, text);
    FN(self, draw_set_color, num(0x1a1a24));
    FN(self, draw_text, num(x + 1), num(y + 1), v);
    FN(self, draw_set_color, num(colour));
    FN(self, draw_text, num(x), num(y), v);
    FN(self, draw_set_font, oldFont);
    FN(self, draw_set_color, oldColour);
    FN(self, draw_set_alpha, oldAlpha);
    FN(self, draw_set_halign, ha);
    FN(self, draw_set_valign, va);
}
/* In the co-op menu: a status block above the buttons, always visible. In play: one line at the
   bottom that stays while something is pending (Porthole starting, waiting, connecting) and
   fades a few seconds after anything else changes. */
static char toastText[192];
static DWORD toastAt;
static void coop_toast(void *self)
{
    const char *head, *hint;
    status_lines(&head, &hint);
    double w = value(FN0(self, display_get_gui_width)), h = value(FN0(self, display_get_gui_height));
    if (exists(self, subId))
    {
        shadow_text(self, w * .5, h * .16, head, 0xffffff, 1);
        if (hint[0])
            shadow_text(self, w * .5, h * .16 + 16, hint, 0xe8d8b8, 1);
        return;
    }
    if (strcmp(toastText, head))
    {
        snprintf(toastText, sizeof(toastText), "%s", head);
        toastAt = GetTickCount();
    }
    int pending = bus->role && bus->status != CONNECTED;
    DWORD age = GetTickCount() - toastAt;
    if (!bus->role && !bus->status_text[0])
        return;
    if (!pending && age > 6000)
        return;
    shadow_text(self, w * .5, h - 22, toastText, 0xffffff, pending || age < 5000 ? 1 : (6000 - age) / 1000.0);
}
static int install_menu_hook(void)
{
    unsigned char *code = addr(S_open_steam_page);
    static const unsigned char prologue[] = {0x55, 0x89, 0xe5, 0x53, 0x57};
    if (memcmp(code, prologue, sizeof(prologue)))
        return 0;
    DWORD old;
    if (!VirtualProtect(code, 5, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    code[0] = 0xe9;
    *(int32_t *)(code + 1) = (int32_t)((uintptr_t)coop_script - (uintptr_t)(code + 5));
    VirtualProtect(code, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), code, 5);
    return 1;
}
static void coop_script_id(void *self)
{
    if (coopScript >= 0)
        return;
    GameString t;
    double id = value(FN(self, asset_get_index, str(&t, "open_steam_page")));
    if (id >= 0)
        coopScript = (int)id;
}
