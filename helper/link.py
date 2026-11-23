"""Webbed's two-peer transport over Porthole (Steam networking) and room/checkpoint exchange."""

import asyncio
import base64
import copy
import ctypes
import hmac
import ipaddress
import json
import math
import mmap
import os
import re
import secrets
import socket
import struct
import tempfile
import time
import winreg
import zlib
from contextlib import suppress
from pathlib import Path

PORT = 37942
PORTHOLE_APP = 4963920
INVITE = re.compile(r"WB3/([A-Za-z0-9]{4,32})/([A-Za-z0-9_-]{20,24})\Z")
SPIDER = struct.Struct("<5I8f12I2fI2d3f")
HEAD = struct.Struct("<9I192s192s64s")
LIMIT = 24 * 1024 * 1024
ARRAYS = (
    "collectables_found",
    "collectables_redeemed",
    "stickers_found",
    "available_pages",
)
NAME = re.compile(r"[A-Za-z0-9_]{1,100}\Z")


def compact(obj):
    return json.dumps(
        obj, ensure_ascii=True, separators=(",", ":"), allow_nan=False
    ).encode()


def unpack_json(b):
    dec = zlib.decompressobj()
    data = dec.decompress(b, LIMIT + 1)
    if not dec.eof or dec.unused_data or len(data) > LIMIT:
        raise ValueError("Размер данных мира")
    return json.loads(data.rstrip(b"\x00"))


def now_ms():
    return ctypes.windll.kernel32.GetTickCount() & 0xFFFFFFFF


def atomic(path, data):
    with tempfile.NamedTemporaryFile(dir=path.parent, delete=False) as stream:
        staging = Path(stream.name)
        stream.write(data)
    try:
        os.replace(staging, path)
    finally:
        staging.unlink(missing_ok=True)


def setstr(mem, offset, size, s):
    data = s.encode("utf8")[: size - 1]
    mem[offset : offset + size] = data.ljust(size, b"\0")


def getstr(mem, offset, size):
    return bytes(mem[offset : offset + size]).split(b"\0", 1)[0].decode("utf8")


WORLD_HEADER = struct.Struct("<5I")
STRAND = struct.Struct("<4i4f2i4f12f")
BODY = struct.Struct("<2i6f")
BODY_START = WORLD_HEADER.size + 1536 * STRAND.size
WORLD_SIZE = BODY_START + 768 * BODY.size
TIES_HEADER = struct.Struct("<2I")
TIE_MAX = 96
CUT_RING = 64
CUT = struct.Struct("<2i2f")  # id, kind (0 remove, 1 split), x, y
CUTS_IN = 4 + CUT_RING * CUT.size
CUTS_SIZE = CUTS_IN + 4 + CUT_RING * CUT.size
TIES_SIZE = TIES_HEADER.size + TIE_MAX * STRAND.size


class Game:
    def __init__(self, pid, profile=None):
        self.pid = pid
        self.mem = mmap.mmap(
            -1, HEAD.size + SPIDER.size * 2, tagname=f"Local\\WebbedPeer_{pid}"
        )
        if self.word(0) != 0x57425032:
            raise RuntimeError("Webbed не запущена")
        self.worldmem = mmap.mmap(-1, WORLD_SIZE, tagname=f"Local\\WebbedWorld_{pid}")
        # Webs the guest tied to the host's spider (world.inc, "Ties").
        self.tiesmem = mmap.mmap(-1, TIES_SIZE, tagname=f"Local\\WebbedTies_{pid}")
        # Strands cut here that the other side owns, and the other way round (world.inc, "Cuts").
        self.cutsmem = mmap.mmap(-1, CUTS_SIZE, tagname=f"Local\\WebbedCuts_{pid}")
        self.cuts_sent = struct.unpack_from("<I", self.cutsmem, 0)[0]
        self.lock = asyncio.Lock()
        self.serial = self.word(3) >> 8
        override = os.environ.get("WEBBED_COOP_PROFILE")  # QA copies with their own save folder
        self.profile = (
            profile
            or (Path(override) if override else None)
            or Path(os.environ["LOCALAPPDATA"]) / "webbed" / "default"
        )
        self.patch = Path(tempfile.gettempdir()) / f"WebbedCoop-{pid}.tmp"
        self.previous_snapshot = None

    def word(self, n):
        return struct.unpack_from("<I", self.mem, n * 4)[0]

    def set(self, n, v):
        struct.pack_into("<I", self.mem, n * 4, v)

    def status(self, v, s):
        setstr(self.mem, 228, 192, s)
        self.set(2, v)

    async def cmd(self, kind):
        async with self.lock:
            self.serial = (self.serial + 1) & 0xFFFFFF
            req = (self.serial << 8) | kind
            self.set(3, req)
            async with asyncio.timeout(12):
                while self.word(4) != req:
                    await asyncio.sleep(0.025)

    async def snapshot(self):
        file = self.profile / "WebbedCoopSnapshot.sav"
        old = file.stat().st_mtime_ns if file.exists() else None
        await self.cmd(1)
        async with asyncio.timeout(10):
            while True:
                try:
                    if file.stat().st_mtime_ns != old:
                        data = await asyncio.to_thread(file.read_bytes)
                        obj = json.loads(data.rstrip(b"\0"))
                        if "room_maps" in obj:
                            # Transient: keep it out of the folder Steam Cloud syncs (*.sav).
                            for extra in (".sav", "_backup.sav", ".png"):
                                with suppress(OSError):
                                    (self.profile / f"WebbedCoopSnapshot{extra}").unlink()
                            return obj
                except (OSError, ValueError):
                    pass
                await asyncio.sleep(0.05)

    async def load(self, world):
        self.profile.mkdir(parents=True, exist_ok=True)
        atomic(self.profile / "WebbedCoopGuest.sav", compact(world) + b"\0")
        await self.cmd(2)

    async def apply(self, patch):
        lines = []
        for key, room in patch.get("rooms", {}).items():
            if NAME.fullmatch(key):
                lines.append(b"R" + key.encode() + b"\t" + compact(room))
        for room, key, kind in patch.get("pickups", []):
            lines.append(f"O{room}\t{kind}\t{key}".encode("ascii"))
        for k, v in patch.get("flags", {}).items():
            if NAME.fullmatch(k) and type(v) in (int, float) and math.isfinite(v):
                lines.append(b"V" + k.encode() + b"\t" + compact(v))
        for k, v in patch.get("arrays", {}).items():
            if k in ARRAYS:
                lines.append(b"A" + k.encode() + b"\t" + compact({"v": v}))
        if "tasks" in patch:
            lines.append(b"Ttasks\t" + compact({"v": patch["tasks"]}))
        if "hub" in patch:
            lines.append(b"Hprelaunch_hub\t" + compact(patch["hub"]))
        if lines:
            atomic(self.patch, b"\n".join(lines))
            await self.cmd(3)

    def frame(self):
        a = HEAD.size
        seq = struct.unpack_from("<I", self.mem, a)[0]
        buf = self.mem[a : a + SPIDER.size]
        return (
            buf
            if not seq & 1 and seq == struct.unpack_from("<I", self.mem, a)[0]
            else None
        )

    def world_frame(self, last):
        seq, room, n, m, complete = WORLD_HEADER.unpack_from(self.worldmem)
        if not seq or seq == last or seq & 1 or n > 1536 or m > 768:
            return None
        data = (
            self.worldmem[: WORLD_HEADER.size + n * STRAND.size]
            + self.worldmem[BODY_START : BODY_START + m * BODY.size]
        )
        if struct.unpack_from("<I", self.worldmem)[0] == seq:
            return seq, data

    def install_world(self, data):
        if not WORLD_HEADER.size <= len(data) <= WORLD_SIZE:
            raise ValueError("Размер физического мира")
        _, room, n, m, full = WORLD_HEADER.unpack_from(data)
        offset = WORLD_HEADER.size + n * STRAND.size
        if (
            room > 1024
            or n > 1536
            or m > 768
            or full > 1
            or len(data) != offset + m * BODY.size
        ):
            raise ValueError("Пакет физического мира")
        for v in STRAND.iter_unpack(data[WORLD_HEADER.size : offset]):
            if (
                v[0] < 100000
                or not -1 <= v[3] <= 467
                or not -1 <= v[9] <= 467
                or any(not math.isfinite(x) or abs(x) > 1e8 for x in (*v[4:8], *v[10:]))
            ):
                raise ValueError("Параметры паутины")
        for v in BODY.iter_unpack(data[offset:]):
            if (
                v[0] < 100000
                or not 0 <= v[1] <= 467
                or any(not math.isfinite(x) or abs(x) > 1e8 for x in v[2:])
            ):
                raise ValueError("Параметры физики")
        seq = (struct.unpack_from("<I", self.worldmem)[0] + 2) & 0xFFFFFFFE
        WORLD_HEADER.pack_into(self.worldmem, 0, seq | 1, room, n, m, full)
        self.worldmem[WORLD_HEADER.size : offset] = data[WORLD_HEADER.size : offset]
        self.worldmem[BODY_START : BODY_START + m * BODY.size] = data[offset:]
        struct.pack_into("<I", self.worldmem, 0, seq)

    def ties_frame(self, last):
        seq, count = TIES_HEADER.unpack_from(self.tiesmem)
        if seq & 1 or seq == last or count > TIE_MAX:
            return None
        data = self.tiesmem[: TIES_HEADER.size + count * STRAND.size]
        if TIES_HEADER.unpack_from(self.tiesmem)[0] == seq:
            return seq, data

    def install_ties(self, data):
        _, count = TIES_HEADER.unpack_from(data)
        if count > TIE_MAX or len(data) != TIES_HEADER.size + count * STRAND.size:
            raise ValueError("Пакет паутины")
        for v in STRAND.iter_unpack(data[TIES_HEADER.size :]):
            if any(not math.isfinite(x) or abs(x) > 1e8 for x in (*v[4:8], *v[10:])):
                raise ValueError("Параметры паутины")
        seq = (TIES_HEADER.unpack_from(self.tiesmem)[0] + 2) & 0xFFFFFFFE
        TIES_HEADER.pack_into(self.tiesmem, 0, seq | 1, count)
        self.tiesmem[TIES_HEADER.size : len(data)] = data[TIES_HEADER.size :]
        TIES_HEADER.pack_into(self.tiesmem, 0, seq, count)

    def new_cuts(self):
        count = struct.unpack_from("<I", self.cutsmem, 0)[0]
        if count - self.cuts_sent > CUT_RING:
            self.cuts_sent = count - CUT_RING
        out = b""
        while self.cuts_sent != count:
            at = 4 + CUT.size * (self.cuts_sent % CUT_RING)
            out += self.cutsmem[at : at + CUT.size]
            self.cuts_sent = (self.cuts_sent + 1) & 0xFFFFFFFF
        return out

    def install_cuts(self, data):
        if len(data) % CUT.size or len(data) > CUT.size * CUT_RING:
            raise ValueError("Пакет разрезов")
        count = struct.unpack_from("<I", self.cutsmem, CUTS_IN)[0]
        for i, kind, x, y in CUT.iter_unpack(data):
            if kind not in (0, 1) or not (math.isfinite(x) and math.isfinite(y)):
                raise ValueError("Пакет разрезов")
            at = CUTS_IN + 4 + CUT.size * (count % CUT_RING)
            CUT.pack_into(self.cutsmem, at, i, kind, x, y)
            count = (count + 1) & 0xFFFFFFFF
        struct.pack_into("<I", self.cutsmem, CUTS_IN, count)

    def remote(self, data):
        if len(data) != SPIDER.size:
            raise ValueError("Размер кадра")
        v = list(SPIDER.unpack(data))
        if (
            v[2] > 2  # 0 none, 1 playing, 2 paused
            or v[3] > 1024
            or v[4] > 63
            or any(not math.isfinite(x) or abs(x) > 1e7 for x in (*v[5:13], *v[25:27]))
            or v[27] > 3
            or any(not math.isfinite(x) or abs(x) > 1e12 for x in v[28:30])
            or any(not math.isfinite(x) or abs(x) > 1e7 for x in v[30:33])
        ):
            raise ValueError("Недопустимое состояние паука")
        a = HEAD.size + SPIDER.size
        seq = (struct.unpack_from("<I", self.mem, a)[0] + 2) & 0xFFFFFFFE
        v[0:2] = seq | 1, now_ms()
        self.mem[a : a + SPIDER.size] = SPIDER.pack(*v)
        struct.pack_into("<I", self.mem, a, seq)


class WorldDelta:
    """Host side: of each published world, only what changed since the last one goes out."""

    def __init__(self):
        self.room = None
        self.strands = {}
        self.bodies = {}

    def encode(self, raw):
        _, room, n, m, complete = WORLD_HEADER.unpack_from(raw)
        offset = WORLD_HEADER.size
        strands = {
            STRAND.unpack_from(raw, offset + i * STRAND.size)[0]: raw[offset + i * STRAND.size : offset + (i + 1) * STRAND.size]
            for i in range(n)
        }
        offset += n * STRAND.size
        bodies = {
            BODY.unpack_from(raw, offset + i * BODY.size)[0]: raw[offset + i * BODY.size : offset + (i + 1) * BODY.size]
            for i in range(m)
        }
        if room != self.room:
            self.room, self.strands, self.bodies = room, {}, {}
        changed = [v for k, v in strands.items() if self.strands.get(k) != v]
        removed = [k for k in self.strands if k not in strands]
        moved = [v for k, v in bodies.items() if self.bodies.get(k) != v]
        gone = [k for k in self.bodies if k not in bodies]
        self.strands, self.bodies = strands, bodies
        if not (changed or removed or moved or gone):
            return None
        head = struct.pack("<6I", room, complete, len(changed), len(removed), len(moved), len(gone))
        ids = struct.pack(f"<{len(removed) + len(gone)}i", *removed, *gone)
        return head + b"".join(changed) + b"".join(moved) + ids


class WorldMirror:
    """Guest side: rebuilds the host's full world from the deltas and hands it to the game."""

    def __init__(self):
        self.room = None
        self.strands = {}
        self.bodies = {}

    def apply(self, body):
        room, complete, nc, nr, nm, ng = struct.unpack_from("<6I", body)
        if max(nc, nr) > 1536 or max(nm, ng) > 768 or complete > 1:
            raise ValueError("Пакет физического мира")
        need = 24 + nc * STRAND.size + nm * BODY.size + 4 * (nr + ng)
        if len(body) != need:
            raise ValueError("Размер физического мира")
        if room != self.room:
            self.room, self.strands, self.bodies = room, {}, {}
        offset = 24
        for _ in range(nc):
            rec = body[offset : offset + STRAND.size]
            self.strands[STRAND.unpack_from(rec)[0]] = rec
            offset += STRAND.size
        for _ in range(nm):
            rec = body[offset : offset + BODY.size]
            self.bodies[BODY.unpack_from(rec)[0]] = rec
            offset += BODY.size
        ids = struct.unpack_from(f"<{nr + ng}i", body, offset)
        for k in ids[:nr]:
            self.strands.pop(k, None)
        for k in ids[nr:]:
            self.bodies.pop(k, None)
        if len(self.strands) > 1536 or len(self.bodies) > 768:
            raise ValueError("Пакет физического мира")
        head = WORLD_HEADER.pack(0, room, len(self.strands), len(self.bodies), complete)
        return head + b"".join(self.strands.values()) + b"".join(self.bodies.values())


class Wire:
    def __init__(self, r, w, key):
        self.r = r
        self.w = w
        self.key = key
        self.rx = self.tx = 0
        self.write_lock = asyncio.Lock()
        sock = w.get_extra_info("socket")
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

    async def send(self, kind, payload=b""):
        async with self.write_lock:
            self.tx += 1
            body = kind + payload
            header = struct.pack("!IQ", len(body), self.tx)
            mac = hmac.digest(self.key, header + body, "sha256")
            self.w.write(header + mac + body)
            await self.w.drain()

    async def recv(self):
        hdr = await self.r.readexactly(12)
        n, seq = struct.unpack("!IQ", hdr)
        if not 1 <= n <= LIMIT or seq != self.rx + 1:
            raise ValueError("Кадр другой версии")
        tag = await self.r.readexactly(32)
        body = await self.r.readexactly(n)
        if not hmac.compare_digest(tag, hmac.digest(self.key, hdr + body, "sha256")):
            raise ValueError("Неверный ключ")
        self.rx = seq
        return body[:1], body[1:]


# Counters Webbed recomputes itself from the shared inventory (set_task_counter): merged by
# maximum, never added, or a pickup would be counted twice and close the quest early.
DERIVED_COUNTERS = {"ant_larvae", "baby_spiders", "golden_webs"}


def changes(previous, current, allowrooms):
    rooms = {
        r: current["room_maps"][r]
        for r in allowrooms
        if r in current["room_maps"]
        and current["room_maps"][r] != previous["room_maps"].get(r)
    }
    flags = {
        k: v
        for k, v in current["save_variables"].items()
        if previous["save_variables"].get(k) != v and type(v) in (float, int)
    }
    arrays = {
        k: current[k]
        for k in ARRAYS
        if current.get(k) != previous.get(k) and k in current
    }
    result = {
        k: v for k, v in (("rooms", rooms), ("flags", flags), ("arrays", arrays)) if v
    }
    if current.get("tasks") != previous.get("tasks"):
        result["tasks"] = current["tasks"]
        # Counters travel as increments: two players advancing one quest in parallel add up.
        before = {t.get("id"): t.get("counter", 0) for t in previous.get("tasks", [])}
        deltas = {
            t["id"]: t["counter"] - before.get(t["id"], 0)
            for t in current["tasks"]
            if t.get("id") not in DERIVED_COUNTERS
            and type(t.get("counter")) in (int, float)
            and t["counter"] > before.get(t["id"], 0)
        }
        if deltas:
            result["task_deltas"] = deltas
    if "prelaunch_hub" in current and current["prelaunch_hub"] != previous.get(
        "prelaunch_hub"
    ):
        result["hub"] = current["prelaunch_hub"]
    return result


def apply_model(world, p):
    world["room_maps"].update(p.get("rooms", {}))
    world["save_variables"].update(p.get("flags", {}))
    world.update(p.get("arrays", {}))
    if "tasks" in p:
        world["tasks"] = p["tasks"]
    if "hub" in p:
        world["prelaunch_hub"] = p["hub"]


def merge_tasks(old, updates, deltas=None):
    """Local tasks (fresh) + a peer's: completion never goes back, counters add the peer's steps."""
    deltas = deltas or {}
    tasks = {t["id"]: dict(t) for t in old}
    for t in updates:
        if t["id"] not in tasks:
            tasks[t["id"]] = dict(t)
            continue
        prev = tasks[t["id"]]
        for k in ("completed", "counter_total"):
            if k in t:
                prev[k] = max(prev.get(k, 0), t[k])
        if t["id"] in deltas:
            counter = prev.get("counter", 0) + deltas[t["id"]]
            total = prev.get("counter_total")
            if type(total) in (int, float) and total > 0:
                counter = min(counter, total)
            prev["counter"] = counter
        elif "counter" in t:
            prev["counter"] = max(prev.get("counter", 0), t["counter"])
    return list(tasks.values())


def safe_patch(p):
    if not isinstance(p, dict) or set(p) - {
        "rooms",
        "flags",
        "arrays",
        "tasks",
        "pickups",
        "task_deltas",
        "hub",
    }:
        raise ValueError("Формат мира")
    deltas = p.get("task_deltas", {})
    if not isinstance(deltas, dict) or any(
        not isinstance(k, str)
        or type(v) not in (int, float)
        or not math.isfinite(v)
        or not 0 < v < 10000
        for k, v in deltas.items()
    ):
        raise ValueError("Счётчики задач")
    if "hub" in p and (
        not isinstance(p["hub"], dict)
        or not isinstance(p["hub"].get("web_list"), list)
        or not isinstance(p["hub"].get("phys_map"), dict)
    ):
        raise ValueError("Состояние хаба")
    for room, key, kind in p.get("pickups", []):
        if not (
            type(kind) is int
            and 0 <= kind < 4
            and NAME.fullmatch(room)
            and room.startswith("room")
            and re.fullmatch(PICKUP_KINDS[kind] + r"_-?\d+_-?\d+", key)
        ):
            raise ValueError("Предмет мира")
    for k, v in p.get("rooms", {}).items():
        if (
            not k.startswith("room")
            or not NAME.fullmatch(k)
            or not isinstance(v, dict)
            or not isinstance(v.get("web_list"), list)
            or not isinstance(v.get("phys_map"), dict)
        ):
            raise ValueError("Формат комнаты")
    for k, v in p.get("arrays", {}).items():
        if (
            k not in ARRAYS
            or not isinstance(v, list)
            or len(v) > 64
            or any(type(x) not in (int, float) or not math.isfinite(x) for x in v)
        ):
            raise ValueError("Инвентарь")
    if "tasks" in p and (
        not isinstance(p["tasks"], list)
        or any(
            not isinstance(t, dict) or not isinstance(t.get("id"), str)
            for t in p["tasks"]
        )
    ):
        raise ValueError("Задачи")
    return p


PICKUP_KINDS = ("oPollen", "oFly", "oBabySpider", "oAntLarva")


def pickup_keys(world):
    return {
        (room, key): int(kind)
        for room, state in world["room_maps"].items()
        for key, kind in state.get("collectable_map", {}).items()
        if kind in range(4) and key.startswith(PICKUP_KINDS[int(kind)] + "_")
    }


class Inventory:
    def __init__(self, world):
        self.seen = pickup_keys(world)
        self.totals = list(world["collectables_found"])

    def add(self, items):
        fresh = []
        for room, key, kind in items:
            if (room, key) not in self.seen:
                self.seen[room, key] = kind
                self.totals[kind] += 1
                fresh.append((room, key, kind))
        return fresh


class Play:
    def __init__(self, g, wire, host, world):
        self.g = g
        self.w = wire
        self.host = host
        self.world = copy.deepcopy(world)
        self.baseline = world
        self.closed = asyncio.Event()
        self.last_room = world["current_room"]
        self.inventory = Inventory(world)
        self.delta = WorldDelta()
        self.mirror = WorldMirror()
        self.lock = asyncio.Lock()
        self.dirty = False

    async def receive(self):
        while True:
            kind, body = await self.w.recv()
            if kind == b"P":
                self.g.remote(body)
            elif kind == b"T" and self.host:
                self.g.install_ties(zlib.decompressobj().decompress(body, TIES_SIZE + 1))
            elif kind == b"X":
                self.g.install_cuts(body)
            elif kind == b"W" and not self.host:
                self.g.install_world(self.mirror.apply(zlib.decompress(body)))
            elif kind == b"Q":
                if self.g.word(7):
                    await self.checkpoint(self.g.word(8))
                await self.w.send(b"K")
                return
            elif kind == b"K":
                self.closed.set()
                return
            elif kind == b"D":
                p = safe_patch(unpack_json(body))
                async with self.lock:
                    # The game merges tasks and arrays into its live state (campaign.inc), so no
                    # fresh save is needed here: that save was a hitch whenever a friend moved rooms.
                    if p.get("pickups"):
                        self.inventory.add(p["pickups"])
                        p.setdefault("arrays", {})["collectables_found"] = list(
                            self.inventory.totals
                        )
                    here = self.baseline["current_room"]
                    if "rooms" in p:
                        p["rooms"].pop(here, None)
                    deltas = p.pop("task_deltas", None)
                    if "tasks" in p:
                        p["tasks"] = merge_tasks(self.world["tasks"], p["tasks"], deltas)
                    for k, v in p.get("arrays", {}).items():
                        if len(v) != len(self.world[k]):
                            raise ValueError("Размер инвентаря")
                        p["arrays"][k] = list(map(max, self.world[k], v))
                    await self.g.apply(p)
                    apply_model(self.world, p)
                    apply_model(self.baseline, p)
                    self.dirty = True
            else:
                raise ValueError("Неизвестное сообщение сеанса")

    async def frames(self):
        last = None
        last_world = 0
        last_ties = 0
        while not self.g.word(5):
            if not self.host and (ties := self.g.ties_frame(last_ties)):
                last_ties, raw = ties
                await self.w.send(b"T", zlib.compress(raw, 1))
            if cuts := self.g.new_cuts():
                await self.w.send(b"X", cuts)
            data = self.g.frame()
            if data and data != last:
                await self.w.send(b"P", data)
                last = data
            if self.host and (world := self.g.world_frame(last_world)):
                last_world, raw = world
                delta = self.delta.encode(raw)
                if delta:
                    await self.w.send(b"W", zlib.compress(delta, 1))
            await asyncio.sleep(1 / 60)
        if self.g.word(7):
            await self.checkpoint(self.g.word(8))
        await self.w.send(b"Q")
        await asyncio.wait_for(self.closed.wait(), 6)

    async def checkpoints(self):
        last_time = time.monotonic()
        room = self.g.word(8)
        while True:
            await asyncio.sleep(0.25)
            r = self.g.word(8)
            # A checkpoint is a full save of the game (a visible hitch), so take one when the
            # room changes (a loading moment anyway) and otherwise every three minutes.
            if not self.g.word(7) or (room == r and time.monotonic() - last_time < 180):
                continue
            await self.checkpoint(r)
            room = r
            last_time = time.monotonic()

    async def checkpoint(self, r):
        async with self.lock:
            await self._checkpoint(r)

    async def _checkpoint(self, r):
        current = await self.g.snapshot()
        name = current["current_room"]
        allowed = {name, self.last_room}
        p = changes(self.baseline, current, allowed)
        fresh = self.inventory.add(
            (room, key, kind) for (room, key), kind in pickup_keys(current).items()
        )
        if fresh:
            p["pickups"] = fresh
        totals = list(self.inventory.totals)
        if current["collectables_found"] != totals:
            await self.g.apply({"arrays": {"collectables_found": totals}})
            current["collectables_found"] = totals
        if fresh or "collectables_found" in p.get("arrays", {}):
            p.setdefault("arrays", {})["collectables_found"] = totals
        if not self.host:
            peer = SPIDER.unpack_from(self.g.mem, HEAD.size + SPIDER.size)
            if peer[2] and peer[3] == r and "rooms" in p:
                p["rooms"].pop(name, None)
        if any(p.values()):
            await self.w.send(b"D", zlib.compress(compact(p), 2))
            apply_model(self.world, p)
        self.baseline = current
        self.last_room = name
        if self.host and self.dirty:
            await self.g.cmd(4)
            self.dirty = False

    async def run(self):
        self.g.status(3, "Друг подключён")
        jobs = [
            asyncio.create_task(f())
            for f in (self.receive, self.frames, self.checkpoints)
        ]
        try:
            done, _ = await asyncio.wait(jobs, return_when=asyncio.FIRST_COMPLETED)
            for d in done:
                d.result()
        finally:
            for t in jobs:
                t.cancel()
            await asyncio.gather(*jobs, return_exceptions=True)
            if self.host and self.dirty and self.g.word(7):
                with suppress(Exception):
                    await self.g.cmd(4)


async def authenticate(r, w, secret, host):
    async with asyncio.timeout(6):
        if host:
            nonce = secrets.token_bytes(24)
            w.write(b"WB04" + nonce)
            await w.drain()
            proof = await r.readexactly(32)
            if not hmac.compare_digest(
                proof, hmac.digest(secret, b"join" + nonce, "sha256")
            ):
                raise ValueError("Неверное приглашение")
        else:
            if await r.readexactly(4) != b"WB04":
                raise ValueError("Нужна та же версия мода")
            nonce = await r.readexactly(24)
            w.write(hmac.digest(secret, b"join" + nonce, "sha256"))
            await w.drain()
        return Wire(r, w, hmac.digest(secret, b"session" + nonce, "sha256"))


async def play_pair(g, r, w, key, host):
    try:
        wire = await authenticate(r, w, key, host)
        if host:
            world = await g.snapshot()
            await wire.send(b"S", zlib.compress(compact(world), 2))
            kind, _ = await asyncio.wait_for(wire.recv(), 30)
            if kind != b"Y":
                raise ValueError("Гость не загрузил мир")
        else:
            kind, snapshot = await asyncio.wait_for(wire.recv(), 30)
            if kind != b"S":
                raise ValueError("Ожидалось сохранение")
            world = unpack_json(snapshot)
            if not isinstance(world, dict) or world.get(
                "current_room"
            ) not in world.get("room_maps", {}):
                raise ValueError("Сохранение хоста")
            await g.load(world)
            await wire.send(b"Y")
        await Play(g, wire, host, world).run()
    finally:
        w.close()
        with suppress(OSError):
            await w.wait_closed()


def steam_libraries():
    roots = []
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"Software\Valve\Steam") as key:
            roots.append(Path(winreg.QueryValueEx(key, "SteamPath")[0]))
    except OSError:
        pass
    roots.append(
        Path(os.environ.get("PROGRAMFILES(X86)", r"C:\Program Files (x86)")) / "Steam"
    )
    for root in list(roots):
        vdf = root / "steamapps" / "libraryfolders.vdf"
        with suppress(OSError):
            roots.extend(
                Path(s.replace("\\\\", "\\"))
                for s in re.findall(
                    r'"path"\s*"([^"]+)"', vdf.read_text(encoding="utf-8-sig")
                )
            )
    return list(dict.fromkeys(roots))


def porthole_exe():
    override = os.environ.get("WEBBED_COOP_PORTHOLE")
    if override and Path(override).is_file():
        return Path(override)
    for root in steam_libraries():
        exe = root / "steamapps" / "common" / "porthole" / "porthole.exe"
        if exe.is_file():
            return exe
    return None


DENIED = {
    "kicked": "Хост отключил вас",
    "banned": "Хост закрыл вам вход",
    "full": "В мире уже есть гость",
    "closed": "Хост закрыл мир",
    "refused": "Хост не отвечает · пусть откроет мир заново",
    "unreachable": "Мир не найден: приглашение устарело или хост закрыл мир - попросите новое",
    "wrong_password": "Приглашение устарело · попросите новое",
    "too_many_tries": "Слишком много попыток · подождите пару минут",
    "outdated": "Обновите Porthole в Steam",
    "host_outdated": "Хосту нужно обновить Porthole в Steam",
}

# Kill Porthole together with this helper even if the helper is terminated abruptly.
_job = None


def _bind_to_job(pid):
    global _job
    k = ctypes.windll.kernel32
    k.CreateJobObjectW.restype = ctypes.c_void_p
    k.OpenProcess.restype = ctypes.c_void_p
    if _job is None:
        job = k.CreateJobObjectW(None, None)
        if not job:
            return

        class Limits(ctypes.Structure):
            _fields_ = [
                ("a", ctypes.c_int64 * 2),
                ("LimitFlags", ctypes.c_uint32),
                ("b", ctypes.c_size_t * 2),
                ("c", ctypes.c_uint32),
                ("d", ctypes.c_size_t),
                ("e", ctypes.c_uint32 * 2),
                ("io", ctypes.c_uint64 * 6),
                ("f", ctypes.c_size_t * 4),
            ]

        limits = Limits()
        limits.LimitFlags = 0x2000  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        k.SetInformationJobObject(
            ctypes.c_void_p(job), 9, ctypes.byref(limits), ctypes.sizeof(limits)
        )
        _job = job
    proc = k.OpenProcess(0x0101, False, pid)  # PROCESS_TERMINATE | SET_QUOTA
    if proc:
        k.AssignProcessToJobObject(ctypes.c_void_p(_job), ctypes.c_void_p(proc))
        k.CloseHandle(ctypes.c_void_p(proc))


class Porthole:
    """One `porthole.exe --json` process: host (`expose`) or guest (`connect`)."""

    def __init__(self, exe, args):
        self.exe = exe
        self.args = args
        self.events = asyncio.Queue()
        self.proc = None
        self.reader = None

    async def start(self):
        extra = os.environ.get("WEBBED_COOP_PORTHOLE_ARGS", "").split()
        self.proc = await asyncio.create_subprocess_exec(
            str(self.exe),
            "--json",
            "--anonymous",
            *extra,
            *self.args,
            stdin=asyncio.subprocess.PIPE,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.DEVNULL,
            cwd=str(self.exe.parent),
            creationflags=0x08000000,  # CREATE_NO_WINDOW
        )
        with suppress(OSError):
            _bind_to_job(self.proc.pid)
        self.reader = asyncio.create_task(self._read())

    async def _read(self):
        while line := await self.proc.stdout.readline():
            with suppress(ValueError):
                event = json.loads(line)
                if isinstance(event, dict) and event.get("event") != "tick":
                    await self.events.put(event)
        await self.events.put({"event": "exited"})

    async def next(self):
        event = await self.events.get()
        name = event.get("event")
        if name == "exited":
            raise ConnectionError("Porthole остановился")
        if name == "denied":
            reason = str(event.get("reason", ""))
            raise ConnectionError(DENIED.get(reason, f"Хост не пустил: {reason}"))
        if name == "error":
            text = str(event.get("message") or event.get("msg") or "")
            if "password" in text:
                raise ConnectionError(DENIED["wrong_password"])
            if text:
                raise ConnectionError(("Porthole: " + text)[:85])
        return event

    async def wait(self, name, timeout, **match):
        async with asyncio.timeout(timeout):
            while True:
                event = await self.next()
                if event.get("event") == name and all(
                    event.get(k) == v for k, v in match.items()
                ):
                    return event

    async def drain(self):
        """Keep reading after setup so a dead tunnel surfaces as an error."""
        while True:
            await self.next()

    async def close(self):
        if self.proc and self.proc.returncode is None:
            with suppress(ProcessLookupError):
                self.proc.terminate()
            with suppress(asyncio.TimeoutError, ProcessLookupError):
                await asyncio.wait_for(self.proc.wait(), 5)
        if self.reader:
            self.reader.cancel()
            await asyncio.gather(self.reader, return_exceptions=True)


def lobby_password(secret):
    return base64.b32encode(hmac.digest(secret, b"porthole", "sha256")[:10]).decode()


def need_porthole():
    exe = porthole_exe()
    if exe:
        return exe
    with suppress(OSError):
        os.startfile(f"steam://install/{PORTHOLE_APP}")
    raise ConnectionError("Установите Porthole в Steam (бесплатно) и повторите")


async def link(pid, profile=None):
    # Windows wakes timers every 15.6 ms by default: a 1/60 s sleep would take one or two of
    # those and the friend's frames would leave late and unevenly. 1 ms while we run.
    with suppress(OSError, AttributeError):
        ctypes.windll.winmm.timeBeginPeriod(1)
    game = Game(pid, profile)
    host = game.word(1) == 1
    # A read-only process handle ties the helper lifetime to its own game instance.
    k = ctypes.windll.kernel32
    k.OpenProcess.restype = ctypes.c_void_p
    process = k.OpenProcess(0x100000, False, pid)

    async def watch():
        while True:
            if (
                (game.word(5) and game.word(2) != 3)
                or not process
                or k.WaitForSingleObject(ctypes.c_void_p(process), 0) == 0
            ):
                return
            await asyncio.sleep(0.15)

    async def network():
        if host:
            backup = game.profile.parent / "WebbedCoopBackup"
            backup.mkdir(parents=True, exist_ok=True)
            for slot in game.profile.glob("save*.sav"):
                if not (backup / slot.name).exists():
                    atomic(backup / slot.name, slot.read_bytes())
            exe = need_porthole()
            secret = secrets.token_bytes(16)
            token = base64.urlsafe_b64encode(secret).decode().rstrip("=")
            busy = False
            active = set()

            async def join(r, w):
                nonlocal busy
                if busy:
                    w.close()
                    return
                active.add(asyncio.current_task())
                busy = True
                try:
                    await play_pair(game, r, w, secret, True)
                except (
                    ConnectionError,
                    ValueError,
                    TimeoutError,
                    asyncio.IncompleteReadError,
                ):
                    pass
                finally:
                    active.discard(asyncio.current_task())
                    busy = False
                    game.status(1, "Мир открыт · друг войдёт через Steam")

            # Only Porthole reaches the game port: it listens on loopback alone.
            server = await asyncio.start_server(join, "127.0.0.1", 0)
            local = server.sockets[0].getsockname()[1]
            tunnel = None
            try:
                # Porthole sometimes quits before it reaches Steam (it is still releasing the
                # previous tunnel, or Steam is busy): start it again a couple of times.
                for attempt in range(1, 4):
                    game.status(
                        1,
                        "Запускается Porthole..."
                        if attempt == 1
                        else f"Запускаю Porthole снова ({attempt}/3)...",
                    )
                    tunnel = Porthole(
                        exe,
                        [
                            "expose",
                            f"tcp/127.0.0.1:{local}:{PORT}",
                            "--password",
                            lobby_password(secret),
                            "--name",
                            "Webbed",
                        ],
                    )
                    await tunnel.start()
                    try:
                        ready = await tunnel.wait("server_ready", 60)
                        break
                    except ConnectionError:
                        await tunnel.close()
                        tunnel = None
                        if attempt == 3:
                            raise
                        await asyncio.sleep(3)
                code = str(ready.get("code", ""))
                invite = f"WB3/{code}/{token}"
                if not INVITE.fullmatch(invite):
                    raise ConnectionError("Porthole выдал неизвестный код")
                setstr(game.mem, 36, 192, invite)
                game.status(1, "Мир открыт · друг войдёт через Steam")
                async with server:
                    watcher = asyncio.create_task(tunnel.drain())
                    serving = asyncio.create_task(server.serve_forever())
                    try:
                        done, _ = await asyncio.wait(
                            [watcher, serving], return_when=asyncio.FIRST_COMPLETED
                        )
                        for t in done:
                            t.result()
                    finally:
                        for t in (watcher, serving):
                            t.cancel()
                        await asyncio.gather(watcher, serving, return_exceptions=True)
            finally:
                tasks = list(active)
                for task in tasks:
                    task.cancel()
                await asyncio.gather(*tasks, return_exceptions=True)
                if tunnel:
                    await tunnel.close()
        else:
            match = INVITE.fullmatch(getstr(game.mem, 36, 192))
            if not match:
                raise ValueError("Неверное приглашение")
            code, key = match.groups()
            secret = base64.urlsafe_b64decode(key + "==")
            if len(secret) != 16:
                raise ValueError("Неверное приглашение")
            direct = os.environ.get("WEBBED_COOP_DIRECT")  # QA only: skip the tunnel
            tunnel = None
            try:
                if direct:
                    host_ip, port = "127.0.0.1", int(direct)
                else:
                    exe = need_porthole()
                    # A world opened a moment ago may not be visible on Steam yet: retry a few times.
                    for attempt in range(1, 4):
                        game.status(
                            2,
                            "Подключение через Steam..."
                            if attempt == 1
                            else f"Повторяю подключение ({attempt}/3)...",
                        )
                        tunnel = Porthole(
                            exe,
                            [
                                "connect",
                                code,
                                "--password",
                                lobby_password(secret),
                                "--auto-approve-ports",
                                f"tcp/{PORT}",
                                "--remap",
                                f"{PORT}:0",
                            ],
                        )
                        await tunnel.start()
                        try:
                            ready = await tunnel.wait("port_ready", 60, port=PORT)
                            break
                        except TimeoutError:
                            raise ConnectionError(
                                "Хост не ответил через Steam · проверьте приглашение"
                            ) from None
                        except ConnectionError as exc:
                            await tunnel.close()
                            tunnel = None
                            if attempt == 3 or str(exc) != DENIED["unreachable"]:
                                raise
                            await asyncio.sleep(4)
                    host_ip, port = str(ready["local"]).rsplit(":", 1)
                    host_ip = str(ipaddress.ip_address(host_ip.strip("[]")))
                    port = int(port)
                try:
                    result = await asyncio.wait_for(
                        asyncio.open_connection(host_ip, port), 15
                    )
                except TimeoutError:
                    raise ConnectionError("Туннель Steam не открылся") from None
                game.status(2, "Загружается мир хоста")
                session = asyncio.create_task(play_pair(game, *result, secret, False))
                jobs = [session]
                if tunnel:
                    jobs.append(asyncio.create_task(tunnel.drain()))
                try:
                    done, _ = await asyncio.wait(
                        jobs, return_when=asyncio.FIRST_COMPLETED
                    )
                    for t in done:
                        t.result()
                finally:
                    for t in jobs:
                        t.cancel()
                    await asyncio.gather(*jobs, return_exceptions=True)
            finally:
                if tunnel:
                    await tunnel.close()

    message = "Сеанс завершён"
    a, b = asyncio.create_task(network()), asyncio.create_task(watch())
    try:
        done, _ = await asyncio.wait([a, b], return_when=asyncio.FIRST_COMPLETED)
        for t in done:
            t.result()
    except (OSError, ValueError, TimeoutError, asyncio.IncompleteReadError) as exc:
        message = str(exc)[:85] or "Связь с другом потеряна"
    finally:
        for t in (a, b):
            t.cancel()
        await asyncio.gather(a, b, return_exceptions=True)
        game.status(0, message)
        game.patch.unlink(missing_ok=True)
        if process:
            k.CloseHandle(ctypes.c_void_p(process))
        game.worldmem.close()
        game.tiesmem.close()
        game.cutsmem.close()
        game.mem.close()
