/* iac -- see compact.h.
 * Copyright (c) 2026 Vasili Gavrilov. ISC License; see LICENSE. */
#include "compact.h"
#include "frame.h"
#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/file.h>
#include <sys/stat.h>

/* Skip one frame in F, leaving it at the next; returns the frame's byte length, 0 at EOF, -1 on a partial tail. */
static long frame_skip(FILE *f)
{
    char hdr[8192], from[256], to[4096];
    long epoch, pos = ftell(f), flen;
    size_t len;
    if (pos < 0 || fgets(hdr, sizeof hdr, f) == NULL) return 0;      /* EOF */
    if (strchr(hdr, '\n') == NULL) return -1;                        /* partial header */
    if (sscanf(hdr, "%255[^|]|%4095[^|]|%ld|%zu", from, to, &epoch, &len) != 4) return -1;
    flen = (long)strlen(hdr) + (long)len + 1;
    if (fseek(f, pos + flen, SEEK_SET) != 0) return -1;
    return flen;
}

/* Print the room log; with TAIL > 0, only the last TAIL frames (count, then skip the front, then dump). */
int cmd_log(const char *room, long tail)
{
    char logp[4096], buf[8192];
    size_t n;
    FILE *f;
    if (p_log(logp, sizeof logp, room)) return die_path();
    f = fopen(logp, "r");
    if (f == NULL) return die("no such room");
    if (tail > 0) {
        long total = 0, skip;
        while (frame_skip(f) > 0) total++;           /* pass 1: count frames */
        rewind(f);
        skip = total > tail ? total - tail : 0;
        while (skip-- > 0 && frame_skip(f) > 0) { }   /* pass 2: drop the front */
    }
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) fwrite(buf, 1, n, stdout);
    fclose(f);
    return 0;
}

/* phase 1: keep = min over every <name>.cur; *nreaders gets the cursor count.
 * Returns the min cursor, -1 if no readers, -2 if the room won't open. */
static long compact_min_cursor(const char *room, long *nreaders)
{
    char p1[4096];
    long keep = -1, n = 0;
    DIR *d = opendir(room);
    struct dirent *e;
    if (d == NULL) return -2;
    while ((e = readdir(d)) != NULL) {
        size_t l = strlen(e->d_name);
        long c;
        if (l < 5 || strcmp(e->d_name + l - 4, ".cur") != 0) continue;
        if (snprintf(p1, sizeof p1, "%s/%s", room, e->d_name) >= (int)sizeof p1) continue;
        c = read_cursor(p1);
        if (keep < 0 || c < keep) keep = c;
        n++;
    }
    closedir(d);
    *nreaders = n;
    return keep;
}

/* phase 2: shift the log left by *keep bytes, in place, under the append lock
 * (so no concurrent append is lost). Caps *keep to the size. 0 ok, -1 on open fail. */
static int compact_shift_log(const char *logp, long *keep)
{
    struct stat st;
    char buf[65536];
    long src, dst, rem;
    int fd = open(logp, O_RDWR);
    if (fd < 0) return -1;
    flock(fd, LOCK_EX);
    if (fstat(fd, &st) != 0) { flock(fd, LOCK_UN); close(fd); return -2; }   /* stat failed */
    if (*keep > (long)st.st_size) *keep = (long)st.st_size;
    src = *keep; dst = 0; rem = (long)st.st_size - *keep;
    while (rem > 0) {
        size_t want = rem < (long)sizeof buf ? (size_t)rem : sizeof buf;
        ssize_t r = pread(fd, buf, want, src);
        if (r <= 0 || pwrite(fd, buf, (size_t)r, dst) != r) break;
        src += r; dst += r; rem -= r;
    }
    if (ftruncate(fd, (long)st.st_size - *keep) != 0) { /* best effort */ }
    flock(fd, LOCK_UN);
    close(fd);
    return 0;
}

/* phase 3: shift every cursor down by keep (all are >= keep, so none goes negative). */
static void compact_shift_cursors(const char *room, long keep)
{
    char p1[4096];
    DIR *d = opendir(room);
    struct dirent *e;
    if (d == NULL) return;
    while ((e = readdir(d)) != NULL) {
        size_t l = strlen(e->d_name);
        long c;
        if (l < 5 || strcmp(e->d_name + l - 4, ".cur") != 0) continue;
        if (snprintf(p1, sizeof p1, "%s/%s", room, e->d_name) >= (int)sizeof p1) continue;
        c = read_cursor(p1);
        write_cursor(p1, c > keep ? c - keep : 0);
    }
    closedir(d);
}

/* phase 4: drop claims < keep, shift survivors down by keep (two-phase .t rename,
 * no collisions). Returns the survivor count; sets *capped if the list overflowed. */
static int compact_rekey_claims(const char *room, long keep, int *capped)
{
    char cld[4096], p1[4096], p2[4096];
    long surv[8192];
    int ns = 0, i;
    DIR *d;
    struct dirent *e;
    *capped = 0;
    if (snprintf(cld, sizeof cld, "%s/claims", room) >= (int)sizeof cld) return 0;
    d = opendir(cld);
    if (d == NULL) return 0;
    while ((e = readdir(d)) != NULL) {
        char *ep;
        long o;
        if (e->d_name[0] == '.') continue;
        o = strtol(e->d_name, &ep, 10);
        if (*ep != '\0') continue;
        if (o < keep) {
            if (snprintf(p1, sizeof p1, "%s/%ld", cld, o) < (int)sizeof p1) unlink(p1);
        } else if (ns < (int)(sizeof surv / sizeof surv[0])) surv[ns++] = o;
        else *capped = 1;
    }
    closedir(d);
    for (i = 0; i < ns; i++)
        if (snprintf(p1, sizeof p1, "%s/%ld", cld, surv[i]) < (int)sizeof p1 &&
            snprintf(p2, sizeof p2, "%s/%ld.t", cld, surv[i] - keep) < (int)sizeof p2) rename(p1, p2);
    for (i = 0; i < ns; i++)
        if (snprintf(p1, sizeof p1, "%s/%ld.t", cld, surv[i] - keep) < (int)sizeof p1 &&
            snprintf(p2, sizeof p2, "%s/%ld", cld, surv[i] - keep) < (int)sizeof p2) rename(p1, p2);
    return ns;
}

/* Reclaim: drop frames every reader has passed, then re-key cursors and claims.
 * The log is shifted in place under the append lock, so no append is lost.
 * Maintenance op; run in a lull (readers take no log lock -- a racing recv may
 * exit once and simply be retried). */
int cmd_compact(const char *room)
{
    char logp[4096];
    long keep, cursors = 0;
    int ns, capped = 0, r;
    if (p_log(logp, sizeof logp, room)) return die_path();
    keep = compact_min_cursor(room, &cursors);
    if (keep == -2) return die("no such room");
    if (keep <= 0) { printf("compact: nothing to drop (min cursor %ld over %ld reader(s))\n",
                            keep < 0 ? 0 : keep, cursors); return 0; }
    r = compact_shift_log(logp, &keep);
    if (r == -1) return die("cannot open room log");
    if (r == -2) return die("stat failed");
    compact_shift_cursors(room, keep);
    ns = compact_rekey_claims(room, keep, &capped);
    printf("compact: dropped %ld bytes, shifted %ld cursor(s), re-keyed %d claim(s)%s\n",
           keep, cursors, ns, capped ? " (claim list capped)" : "");
    return 0;
}
