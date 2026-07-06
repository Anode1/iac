/* iac -- frame primitives: name/spec validation, addressing, path builders, and
 * the per-reader cursor. A frame is <from>|<to>|<epoch>|<len>\n then the body.
 * Copyright (c) 2026 Vasili Gavrilov. ISC License; see LICENSE. */
#ifndef IAC_FRAME_H
#define IAC_FRAME_H

#include <stddef.h>

int  ok_name(const char *s);        /* one name: [A-Za-z0-9_-]+, bounded (IAC_NAME_MAX) */
int  ok_spec(const char *s);        /* recipient spec: "*", "?", or a comma-list of names */
int  to_me(const char *to, const char *me);   /* does spec TO address ME? */

int  p_log(char *b, size_t n, const char *room);
int  p_cur(char *b, size_t n, const char *room, const char *me);
int  p_ros(char *b, size_t n, const char *room, const char *me);

long read_cursor(const char *path);
void write_cursor(const char *path, long c);

#endif /* IAC_FRAME_H */
