/* iac -- presence: a held roster flock means online; who probes it. join/leave
 * register a name; hold is a background beacon; presence_enter stamps last-seen.
 * Copyright (c) 2026 Vasili Gavrilov. ISC License; see LICENSE. */
#ifndef IAC_PRESENCE_H
#define IAC_PRESENCE_H

/* Stamp last-seen and hold a SHARED roster flock for the caller's life
 * (parked recv == online). Returns the held fd, or -1. Caller closes it. */
int presence_enter(const char *room, const char *me);

/* 1 if others are registered and none is online or seen within stale_s. */
int presence_alone(const char *room, const char *me, int stale_s);

int cmd_join(const char *room, const char *me);
int cmd_leave(const char *room, const char *me);
int cmd_hold(const char *room, const char *me);
int cmd_who(const char *room);

#endif /* IAC_PRESENCE_H */
