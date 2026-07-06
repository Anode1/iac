/* iac -- log: print the room log (optionally the last K frames). compact:
 * reclaim space by dropping frames every reader has passed, then re-key
 * cursors and claims. A maintenance op; run in a lull (readers take no log lock).
 * Copyright (c) 2026 Vasili Gavrilov. ISC License; see LICENSE. */
#ifndef IAC_COMPACT_H
#define IAC_COMPACT_H

int cmd_log(const char *room, long tail);
int cmd_compact(const char *room);

#endif /* IAC_COMPACT_H */
