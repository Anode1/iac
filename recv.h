/* iac -- receive: block for the next frame addressed to me (recv), sweep the
 * whole backlog at once (drain), or tail -f my frames without claiming "?" (follow).
 * The wait sleeps on an inotify watch of <room>/log (Linux) or a 100 ms poll.
 * Copyright (c) 2026 Vasili Gavrilov. ISC License; see LICENSE. */
#ifndef IAC_RECV_H
#define IAC_RECV_H

int cmd_recv(const char *room, const char *me, int timeout_s, int all);
int cmd_drain(const char *room, const char *me);
int cmd_follow(const char *room, const char *me, int idle_s);

#endif /* IAC_RECV_H */
