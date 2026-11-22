static GameString exportRef = {"WebbedCoopSnapshot", 10000, 18}, guestRef = {"WebbedCoopGuest", 10000, 15};
static Val persistent_string(GameString *r)
{
    Val v = {.str = r, .kind = 1};
    return v;
}
static void export_world(void *self)
{
    Val previous = global_get(self, "save_name");
    if ((previous.kind & 0xffffff) != 1 || !player)
    {
        drop(&previous);
        return;
    }
    global_set(self, "save_name", persistent_string(&exportRef));
    script(self, S_save_game, 0, NULL);
    global_set(self, "save_name", previous);
    drop(&previous);
}
static void guest_load(void *self)
{
    if (!guestSlot)
    {
        originalSlot = global_get(self, "save_name");
        guestSlot = 1;
    }
    global_set(self, "save_name", persistent_string(&guestRef));
    script(self, S_load_game, 0, NULL);
    guestSlotPending = 0;
    lobby = 0;
}
static void apply_pickup(void *self, Val rooms, const char *roomName, char *payload)
{
    static const int objects[] = {427, 93, 436, 434};
    char *key = strchr(payload, '\t');
    if (!key)
        return;
    *key++ = 0;
    int kind = atoi(payload);
    if (kind < 0 || kind >= 4)
        return;
    GameString text;
    Val roomMap = FN(self, ds_map_find_value, rooms, str(&text, roomName));
    if (!isnum(roomMap))
        return;
    Val items = FN(self, ds_map_find_value, roomMap, str(&text, "collectable_map"));
    Val removed = FN(self, ds_map_find_value, roomMap, str(&text, "removed_map"));
    if (isnum(items))
        FN(self, ds_map_set, items, str(&text, key), num(kind));
    if (isnum(removed))
        FN(self, ds_map_set, removed, str(&text, key), num(1));
    Val name = FN(self, room_get_name, num(coord(self, B_room)));
    int active = ((name.kind & 255) == 1 && name.str && strcmp(name.str->text, roomName) == 0);
    drop(&name);
    if (!active)
        return;
    Iterator it;
    begin(&it, self, objects[kind]);
    void *item;
    while ((item = next(&it)))
    {
        Val id = num(instance_id(item));
        Val placed = script(item, 0x549980, 1, &id);
        int match = ((placed.kind & 255) == 1 && placed.str && !strcmp(placed.str->text, key));
        drop(&placed);
        if (match)
        {
            FN0(item, instance_destroy);
            break;
        }
    }
}

static void apply_patch(void *self)
{
    FILE *f = _wfopen(patchFile, L"rb");
    if (!f)
        return;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    if (n < 2 || n > 24 * 1024 * 1024)
    {
        fclose(f);
        return;
    }
    char *data = malloc(n + 1);
    if (!data)
    {
        fclose(f);
        return;
    }
    size_t size = fread(data, 1, n, f);
    fclose(f);
    data[size] = 0;
    Val save = global_get(self, "save_map");
    GameString a, b, c;
    Val rooms = FN(self, ds_map_find_value, save, str(&a, "room_maps"));
    Val flags = FN(self, ds_map_find_value, save, str(&a, "save_variables"));
    char *line = data;
    while (line && *line)
    {
        char *next = strchr(line, '\n');
        if (next)
            *next++ = 0;
        char *key = line + 1, *payload = strchr(key, '\t');
        if (!payload)
            break;
        *payload++ = 0;
        if (line[0] == 'R')
        {
            Val map = FN(self, json_decode, str(&a, payload));
            if (isnum(map))
                FN(self, ds_map_replace_map, rooms, str(&b, key), map);
        }
        else if (line[0] == 'O')
            apply_pickup(self, rooms, key, payload);
        else if (line[0] == 'H' && !strcmp(key, "prelaunch_hub"))
        {
            /* The hub as the story left it before the launch: a room map kept at the save root. */
            Val map = FN(self, json_decode, str(&a, payload));
            if (isnum(map))
                FN(self, ds_map_replace_map, save, str(&b, key), map);
        }
        else if (line[0] == 'V')
        {
            Val args[] = {str(&a, key), num(strtod(payload, NULL))};
            script(self, 0x5635b0, 2, args);
        }
        else if (line[0] == 'A' || line[0] == 'T')
        {
            Val root = FN(self, json_decode, str(&a, payload));
            if (isnum(root))
            {
                Val list = FN(self, ds_map_find_value, root, str(&c, "v"));
                if (isnum(list) && line[0] == 'A')
                {
                    Val arr = script(self, S_list_to_array, 1, &list);
                    global_set(self, key, arr);
                    drop(&arr);
                }
                else if (isnum(list))
                {
                    /* Match Webbed's task restore: the save map owns the deserialized list. */
                    Val tasks = global_get(self, "tasks");
                    FN(self, ds_list_copy, tasks, list);
                    GameString owner;
                    FN(self, ds_map_replace_map, save, str(&owner, "WebbedCoopTaskOwner"), root);
                    root = undef();
                }
                if (isnum(root))
                    FN(self, ds_map_destroy, root);
            }
        }
        line = next;
    }
    free(data);
}
/* Co-op slots (the host's snapshot, the guest's copy of the host world) are scratch files, but
   they sit in the folder Steam Cloud uploads (*.sav). Clear leftovers whenever no session runs.
   GameMaker keeps saves in %LOCALAPPDATA%\<game>\default; the WebbedCoop prefix is the mod's own. */
static void clean_coop_saves(void)
{
    wchar_t local[MAX_PATH], pattern[MAX_PATH], file[MAX_PATH];
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH))
        return;
    WIN32_FIND_DATAW game;
    swprintf(pattern, MAX_PATH, L"%ls\\*", local);
    HANDLE games = FindFirstFileW(pattern, &game);
    if (games == INVALID_HANDLE_VALUE)
        return;
    do
    {
        if (!(game.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || game.cFileName[0] == L'.')
            continue;
        const wchar_t *masks[] = {L"WebbedCoopSnapshot*", L"WebbedCoopGuest*"};
        for (unsigned i = 0; i < 2; i++)
        {
            swprintf(pattern, MAX_PATH, L"%ls\\%ls\\default\\%ls", local, game.cFileName, masks[i]);
            WIN32_FIND_DATAW found;
            HANDLE h = FindFirstFileW(pattern, &found);
            if (h == INVALID_HANDLE_VALUE)
                continue;
            do
            {
                const wchar_t *ext = wcsrchr(found.cFileName, L'.');
                if (!ext || (_wcsicmp(ext, L".sav") && _wcsicmp(ext, L".png")))
                    continue;
                swprintf(file, MAX_PATH, L"%ls\\%ls\\default\\%ls", local, game.cFileName, found.cFileName);
                DeleteFileW(file);
            } while (FindNextFileW(h, &found));
            FindClose(h);
        }
    } while (FindNextFileW(games, &game));
    FindClose(games);
}
static void campaign_commands(void *self)
{
    static int cleaned;
    if (!cleaned && !bus->role && !guestSlot && !guestSlotPending)
    {
        cleaned = 1;
        clean_coop_saves();
    }
    if (bus->request == bus->ack)
        return;
    unsigned req = bus->request;
    switch (req & 255)
    {
    case EXPORT_SAVE:
        if (!player)
            return;
        export_world(self);
        break;
    case LOAD_GUEST:
        guest_load(self);
        break;
    case APPLY_PATCH:
        if (!player)
            return;
        apply_patch(self);
        break;
    case 4:
        if (!player)
            return;
        script(self, S_save_game, 0, NULL);
        break;
    }
    MemoryBarrier();
    bus->ack = req;
}
