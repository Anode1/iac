/* iac -- see claim.h.
 * Copyright (c) 2026 Vasili Gavrilov. ISC License; see LICENSE. */
#include "claim.h"
#include "frame.h"
#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <dirent.h>
#include <sys/file.h>
#include <sys/stat.h>

/* Seconds an unacked claim stays owned before it is presumed dead and re-claimable. */
static long claim_ttl(void)
{
    const char *s = getenv("IAC_CLAIM_TTL");
    long t;
    if (s == NULL || (t = atol(s)) <= 0) return 300;
    return t;
}
/* Write a claim-state line to FD (already positioned at 0). Returns 0 on success. */
static int claim_write(int fd, const char *state, long epoch, const char *who)
{
    char line[300];
    int n = snprintf(line, sizeof line, "%s %ld %s\n", state, epoch, who);
    return (n > 0 && (size_t)n < sizeof line && write(fd, line, (size_t)n) == n) ? 0 : -1;
}

/* Win the fresh "?" at offset ID by creating its claim file (O_CREAT|O_EXCL); EEXIST = a peer has it, lose. */
int claim_fresh(const char *room, long id, const char *me)
{
    char cld[4096], clp[4096];
    int fd;
    if (snprintf(cld, sizeof cld, "%s/claims", room) >= (int)sizeof cld) return 0;
    mkdir(cld, 0700);
    if (snprintf(clp, sizeof clp, "%s/claims/%ld", room, id) >= (int)sizeof clp) return 0;
    fd = open(clp, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return 0;                     /* EEXIST: already claimed by a peer */
    claim_write(fd, "claimed", (long)time(NULL), me);
    close(fd);
    return 1;
}

/* Steal claim ID iff unacked past TTL (rewrite epoch under flock, so one stealer wins); done or active = lose. */
static int claim_steal(const char *room, long id, const char *me)
{
    char clp[4096], st[300];
    long ttl = claim_ttl(), now = (long)time(NULL), epoch = 0;
    int fd, won = 0;
    if (snprintf(clp, sizeof clp, "%s/claims/%ld", room, id) >= (int)sizeof clp) return 0;
    fd = open(clp, O_RDWR);
    if (fd < 0) return 0;
    flock(fd, LOCK_EX);
    {
        ssize_t r = read(fd, st, sizeof st - 1);
        st[r > 0 ? r : 0] = '\0';
        if (sscanf(st, "claimed %ld", &epoch) == 1 && now - epoch > ttl) {
            if (lseek(fd, 0, SEEK_SET) == 0 && ftruncate(fd, 0) == 0 &&
                claim_write(fd, "claimed", now, me) == 0) won = 1;   /* steal */
        }
    }
    flock(fd, LOCK_UN);
    close(fd);
    return won;
}

/* Mark the "?" at offset ID done (id is what recv printed on stderr) so it is never re-claimed. */
int cmd_ack(const char *room, const char *me, long id)
{
    char clp[4096];
    int fd, rc;
    if (snprintf(clp, sizeof clp, "%s/claims/%ld", room, id) >= (int)sizeof clp) return die_path();
    fd = open(clp, O_WRONLY | O_CREAT, 0600);
    if (fd < 0) return die("cannot open claim");
    flock(fd, LOCK_EX);
    rc = (lseek(fd, 0, SEEK_SET) == 0 && ftruncate(fd, 0) == 0 &&
          claim_write(fd, "done", (long)time(NULL), me) == 0) ? 0 : 1;
    flock(fd, LOCK_UN);
    close(fd);
    return rc ? die("write failed") : 0;
}

/* Steal one expired claim from claims/ (keyed by offset, so cursor-independent) and re-deliver its frame; 1 if recovered. */
int recover_orphan(const char *room, const char *me)
{
    char cld[4096], logp[4096], hdr[8192], from[256], to[4096];
    DIR *d = NULL;
    FILE *f = NULL;
    struct dirent *e;
    int rc = 0;
    if (snprintf(cld, sizeof cld, "%s/claims", room) >= (int)sizeof cld) return 0;
    if (p_log(logp, sizeof logp, room)) return 0;
    d = opendir(cld);
    if (d == NULL) return 0;                       /* no claims dir yet */
    while ((e = readdir(d)) != NULL) {
        long off, epoch;
        size_t len;
        char *end;
        if (e->d_name[0] == '.') continue;
        off = strtol(e->d_name, &end, 10);
        if (*end != '\0' || off < 0) continue;     /* not an offset-named claim */
        if (!claim_steal(room, off, me)) continue; /* done, active, or lost the race */
        f = fopen(logp, "r");                      /* stolen: re-deliver the frame */
        if (f == NULL) continue;
        if (fseek(f, off, SEEK_SET) == 0 &&
            fgets(hdr, sizeof hdr, f) != NULL && strchr(hdr, '\n') != NULL &&
            sscanf(hdr, "%255[^|]|%4095[^|]|%ld|%zu", from, to, &epoch, &len) == 4 &&
            len <= sizeof g_body && fread(g_body, 1, len, f) == len) {
            fprintf(stderr, "iac: from %s to ? at %ld claim %ld\n", from, epoch, off);
            fwrite(g_body, 1, len, stdout);
            fflush(stdout);
            rc = 1;
            goto done;                             /* single exit: close f + dir below */
        }
        fclose(f); f = NULL;
    }
done:
    if (f != NULL) fclose(f);
    if (d != NULL) closedir(d);
    return rc;
}
