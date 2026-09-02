/* iac -- see recv.h.
 * Copyright (c) 2026 Vasili Gavrilov. ISC License; see LICENSE. */
#include "recv.h"
#include "frame.h"
#include "claim.h"
#include "presence.h"
#include "common.h"
#include "constants.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#if defined(__linux__)
#include <sys/inotify.h>        /* wake recv on log append, not on a fixed poll */
#include <poll.h>
#endif

/* An inotify fd for waking on <room>/log changes, or -1 (non-Linux, or on error -> poll). */
static int watch_open(void)
{
#if defined(__linux__)
    return inotify_init1(IN_NONBLOCK);
#else
    return -1;
#endif
}

/* One idle interval: block up to IAC_POLL_MS for LOGP to change. Linux waits on
 * inotify (kernel wake on append -- sub-ms, 0% CPU); otherwise a plain sleep.
 * Either way it returns within IAC_POLL_MS, so the caller's scan/timeout tick is
 * unchanged -- inotify only makes the wait wake early on a real append. */
static void idle_wait(int ino, const char *logp)
{
#if defined(__linux__)
    if (ino >= 0) {
        struct pollfd pfd = { ino, POLLIN, 0 };
        char buf[4096];
        inotify_add_watch(ino, logp, IN_MODIFY);   /* idempotent; catches on once the log exists */
        if (poll(&pfd, 1, IAC_POLL_MS) > 0) while (read(ino, buf, sizeof buf) > 0) { }
        return;
    }
#else
    (void)ino; (void)logp;
#endif
    { struct timespec s = { 0, IAC_POLL_MS * 1000000L }; nanosleep(&s, NULL); }
}

static int recv_loop(const char *room, const char *me, int timeout_s, int ino, int all, int stale_s)
{
    char logp[4096], curp[4096], hdr[8192], from[256], to[4096];
    long cursor, epoch, frame_end;
    size_t len;
    int waited_ms = 0, limit_ms = timeout_s * 1000, rc = 0, got = 0, ticks = 0;
    FILE *f = NULL;                                      /* held across the frame scan; closed once at `done` */

    if (p_log(logp, sizeof logp, room)) return die_path();
    if (p_cur(curp, sizeof curp, room, me)) return die_path();
    cursor = read_cursor(curp);

    for (;;) {
        struct stat st;
        if (stat(logp, &st) == 0 && st.st_size > cursor) {
            f = fopen(logp, "r");
            if (f == NULL) { rc = die("cannot open room log"); goto done; }
            if (fseek(f, cursor, SEEK_SET) != 0) { rc = die("seek failed"); goto done; }
            for (;;) {                                  /* scan frames forward */
                if (fgets(hdr, sizeof hdr, f) == NULL || strchr(hdr, '\n') == NULL)
                    break;                              /* no whole header yet */
                if (sscanf(hdr, "%255[^|]|%4095[^|]|%ld|%zu", from, to, &epoch, &len) != 4) {
                    rc = die("corrupt frame"); goto done;
                }
                frame_end = cursor + (long)strlen(hdr) + (long)len + 1;
                if (st.st_size < frame_end) break;      /* body not fully there */
                {
                    int is_claim = (strcmp(to, "?") == 0);
                    int mine = (strcmp(from, me) != 0) && (is_claim || to_me(to, me));
                    if (mine && is_claim && !claim_fresh(room, cursor, me)) mine = 0;   /* a peer claimed it */
                    if (mine) {
                        if (len > sizeof g_body) { rc = die("message too large"); goto done; }
                        if (fread(g_body, 1, len, f) != len) { rc = die("short read"); goto done; }
                        if (is_claim)   /* id lets the worker `iac ack` on completion */
                            fprintf(stderr, "iac: from %s to ? at %ld claim %ld\n", from, epoch, cursor);
                        else
                            fprintf(stderr, "iac: from %s to %s at %ld\n", from, to, epoch);
                        fwrite(g_body, 1, len, stdout);
                        if (all) fputc('\n', stdout);   /* separate successive bodies, as drain does */
                        fflush(stdout);
                        write_cursor(curp, frame_end);
                        got = 1;
                        if (!all) { rc = 0; goto done; }
                    }
                }
                cursor = frame_end;                     /* not for me (or lost claim): skip on */
                if (fseek(f, cursor, SEEK_SET) != 0) { rc = die("seek failed"); goto done; }
            }
            fclose(f); f = NULL;
            write_cursor(curp, cursor);                 /* persist scan progress */
            if (got) { rc = 0; goto done; }             /* -a: the burst is delivered */
        }
        if (recover_orphan(room, me)) { rc = 0; goto done; }   /* re-run a dead worker's task */
        if (stale_s > 0 && ++ticks % 20 == 0 && presence_alone(room, me, stale_s)) {
            fprintf(stderr, "iac: alone in %s (no peer active within %ds)\n", room, stale_s);
            rc = 3; goto done;                          /* -e: everyone else is gone */
        }
        if (waited_ms >= limit_ms) { rc = 1; goto done; }      /* nothing for me in time */
        idle_wait(ino, logp);                           /* wake on append (inotify) or after the tick */
        waited_ms += IAC_POLL_MS;
    }
done:
    if (f != NULL) fclose(f);
    return rc;
}

/* recv, wrapped to hold presence + an inotify wake for the whole blocking wait.
 * all=1 (-a): after the first message, deliver every further frame already
 * queued for me in the same return -- one wakeup per burst, not per frame,
 * so an LLM seat pays one turn where recv-then-drain would cost two.
 * stale_s>0 (-e): exit 3 when others are registered and none is online or
 * seen within stale_s -- the C child watches the roster so a seat never
 * spends model turns discovering that its peers are gone. */
int cmd_recv(const char *room, const char *me, int timeout_s, int all, int stale_s)
{
    int pfd = presence_enter(room, me);
    int ino = watch_open();
    int rc = recv_loop(room, me, timeout_s, ino, all, stale_s);
    if (ino >= 0) close(ino);
    if (pfd >= 0) close(pfd);
    return rc;
}

/* drain: one non-blocking sweep -- deliver EVERY queued frame for me in order (claiming fresh "?" too), advance past all. exit 0 if any, 1 if empty. */
int cmd_drain(const char *room, const char *me)
{
    char logp[4096], curp[4096], hdr[8192], from[256], to[4096];
    long cursor, epoch, frame_end;
    size_t len;
    int got = 0, pfd, rc;
    struct stat st;
    FILE *f;
    if (p_log(logp, sizeof logp, room) || p_cur(curp, sizeof curp, room, me)) return die_path();
    pfd = presence_enter(room, me);          /* stamp last-seen; drain is instant, no need to hold the lock */
    if (pfd >= 0) close(pfd);
    cursor = read_cursor(curp);
    if (stat(logp, &st) != 0 || st.st_size <= cursor) return 1;   /* empty box */
    f = fopen(logp, "r");
    if (f == NULL) return die("cannot open room log");
    if (fseek(f, cursor, SEEK_SET) != 0) { rc = die("seek failed"); goto done; }
    for (;;) {
        int is_claim, mine;
        if (fgets(hdr, sizeof hdr, f) == NULL || strchr(hdr, '\n') == NULL) break;
        if (sscanf(hdr, "%255[^|]|%4095[^|]|%ld|%zu", from, to, &epoch, &len) != 4) { rc = die("corrupt frame"); goto done; }
        frame_end = cursor + (long)strlen(hdr) + (long)len + 1;
        if (st.st_size < frame_end) break;       /* body not fully written yet */
        is_claim = (strcmp(to, "?") == 0);
        mine = (strcmp(from, me) != 0) && (is_claim ? claim_fresh(room, cursor, me) : to_me(to, me));
        if (mine) {
            if (len > sizeof g_body) { rc = die("message too large"); goto done; }
            if (fread(g_body, 1, len, f) != len) { rc = die("short read"); goto done; }
            if (is_claim) fprintf(stderr, "iac: from %s to ? at %ld claim %ld\n", from, epoch, cursor);
            else fprintf(stderr, "iac: from %s to %s at %ld\n", from, to, epoch);
            fwrite(g_body, 1, len, stdout);
            fputc('\n', stdout);                 /* separate successive bodies */
            got++;
        }
        cursor = frame_end;
        if (fseek(f, cursor, SEEK_SET) != 0) { rc = die("seek failed"); goto done; }
    }
    write_cursor(curp, cursor);
    fflush(stdout);
    rc = got > 0 ? 0 : 1;
done:
    fclose(f);
    return rc;
}

/* tail -f for my messages: stream each frame for me as it lands (never claims "?"); return after IDLE_S of silence. */
int cmd_follow(const char *room, const char *me, int idle_s)
{
    char logp[4096], curp[4096], hdr[8192], from[256], to[4096];
    long cursor, epoch, frame_end;
    size_t len;
    int waited_ms = 0, limit_ms = idle_s * 1000, pfd, ino;
    if (p_log(logp, sizeof logp, room) || p_cur(curp, sizeof curp, room, me)) return die_path();
    pfd = presence_enter(room, me);
    ino = watch_open();
    cursor = read_cursor(curp);
    for (;;) {
        struct stat st;
        int got = 0;
        if (stat(logp, &st) == 0 && st.st_size > cursor) {
            FILE *f = fopen(logp, "r");
            if (f != NULL && fseek(f, cursor, SEEK_SET) == 0) {
                for (;;) {
                    if (fgets(hdr, sizeof hdr, f) == NULL || strchr(hdr, '\n') == NULL) break;
                    if (sscanf(hdr, "%255[^|]|%4095[^|]|%ld|%zu", from, to, &epoch, &len) != 4) break;
                    frame_end = cursor + (long)strlen(hdr) + (long)len + 1;
                    if (st.st_size < frame_end) break;             /* body not all there yet */
                    if (strcmp(from, me) != 0 && strcmp(to, "?") != 0 && to_me(to, me) &&
                        len <= sizeof g_body && fread(g_body, 1, len, f) == len) {
                        fprintf(stderr, "iac: from %s to %s at %ld\n", from, to, epoch);
                        fwrite(g_body, 1, len, stdout);
                        fputc('\n', stdout);
                        fflush(stdout);
                        got = 1;
                    }
                    cursor = frame_end;
                    if (fseek(f, cursor, SEEK_SET) != 0) break;
                }
            }
            if (f != NULL) fclose(f);
            write_cursor(curp, cursor);
        }
        if (got) waited_ms = 0;                                    /* activity resets the idle clock */
        else {
            if (waited_ms >= limit_ms) break;
            idle_wait(ino, logp);                                  /* wake on append or after the tick */
            waited_ms += IAC_POLL_MS;
        }
    }
    if (ino >= 0) close(ino);
    if (pfd >= 0) close(pfd);
    return 0;
}
