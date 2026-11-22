#ifndef WEBBED_SESSION_H
#define WEBBED_SESSION_H
#include <stdint.h>
/* Same ABI as link.py. Only the helper for this Webbed process maps this name. */
typedef struct
{
    uint32_t sequence, heartbeat, active, room, buttons;
    float x, y, vx, vy, hm, vm, mx, my;
    uint32_t down[6], up[6];
    float wx, wy;   /* where the web being woven starts (flags & SPIDER_WEAVING) */
    uint32_t flags;
} Spider;
#define SPIDER_WEAVING 1u
typedef struct
{
    uint32_t magic, role, status, request, ack, disconnect, pulse, playing, room;
    char invite[192], status_text[192], slot[64];
    Spider local, remote;
} Bus;
enum
{
    OFFLINE,
    HOST,
    GUEST
};
enum
{
    IDLE,
    LISTENING,
    CONNECTING,
    CONNECTED,
    FAILED
};
enum
{
    EXPORT_SAVE = 1,
    LOAD_GUEST,
    APPLY_PATCH
};
#endif
