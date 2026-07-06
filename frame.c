/* iac -- see frame.h.
 * Copyright (c) 2026 Vasili Gavrilov. ISC License; see LICENSE. */
#include "frame.h"
#include "constants.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>

/* A NAME is one component: [A-Za-z0-9_-]+ (never escapes the room dir). */
int ok_name(const char *s)
{
    size_t n = 0;
    if (s == NULL || *s == '\0') return 0;
    for (const char *p = s; *p; p++, n++) {
        if (n >= IAC_NAME_MAX) return 0;    /* bounded so it always fits the frame header */
        if (!isalnum((unsigned char)*p) && *p != '_' && *p != '-') return 0;
    }
    return 1;
}

/* A recipient SPEC is "*"/"?" or a comma-list of names (no empty segments). */
int ok_spec(const char *s)
{
    size_t seg = 0, total = 0;
    if (s != NULL && (strcmp(s, "*") == 0 || strcmp(s, "?") == 0)) return 1;
    if (s == NULL || *s == '\0') return 0;
    for (const char *p = s; *p; p++) {
        if (++total > IAC_SPEC_MAX) return 0;               /* whole spec fits the header */
        if (*p == ',') { if (seg == 0) return 0; seg = 0; }
        else if (isalnum((unsigned char)*p) || *p == '_' || *p == '-') {
            if (++seg > IAC_NAME_MAX) return 0;             /* each segment is one name */
        }
        else return 0;
    }
    return seg > 0;                          /* must not end on a comma */
}

/* Does recipient spec TO address ME? ("*" or ME is one of the comma segments) */
int to_me(const char *to, const char *me)
{
    size_t ml = strlen(me);
    if (strcmp(to, "*") == 0) return 1;
    for (const char *p = to; *p; ) {
        const char *c = strchr(p, ',');
        size_t seg = c ? (size_t)(c - p) : strlen(p);
        if (seg == ml && strncmp(p, me, ml) == 0) return 1;
        if (c == NULL) break;
        p = c + 1;
    }
    return 0;
}

int p_log(char *b, size_t n, const char *room)
{ int k = snprintf(b, n, "%s/log", room); return (k > 0 && (size_t)k < n) ? 0 : -1; }
int p_cur(char *b, size_t n, const char *room, const char *me)
{ int k = snprintf(b, n, "%s/%s.cur", room, me); return (k > 0 && (size_t)k < n) ? 0 : -1; }
int p_ros(char *b, size_t n, const char *room, const char *me)
{ int k = snprintf(b, n, "%s/roster/%s", room, me); return (k > 0 && (size_t)k < n) ? 0 : -1; }

long read_cursor(const char *path)
{
    long c = 0;
    FILE *f = fopen(path, "r");
    if (f == NULL) return 0;
    if (fscanf(f, "%ld", &c) != 1) c = 0;
    fclose(f);
    return c < 0 ? 0 : c;
}
void write_cursor(const char *path, long c)
{
    FILE *f = fopen(path, "w");
    if (f == NULL) return;
    fprintf(f, "%ld\n", c);
    fclose(f);
}
