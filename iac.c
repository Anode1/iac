/* iac -- inter-agent communication: a shared-log dispatcher with rooms.
 *
 * A ROOM is a directory holding ONE append-only log (<room>/log). Every message
 * is appended once, whatever its audience, so BROADCAST costs one write, not N.
 * Each member keeps a byte CURSOR into the log (<room>/<name>.cur) and receives
 * by long-polling forward, delivering only the frames addressed to it:
 *
 *   to = "*"        broadcast: everyone (bar the sender)
 *   to = "bob"      point-to-point
 *   to = "a,b,c"    a subset (multicast)
 *   to = "?"        any ONE free member claims it (a work queue: competing consumers)
 *
 * A "?" claim is crash-recoverable: the winner acks on completion (iac ack); a
 * claim left unacked past $IAC_CLAIM_TTL seconds (default 300) is presumed dead
 * and becomes re-claimable, so a job whose worker crashes still runs to completion.
 *
 * The wait happens here in C, so a parked recv is one process asleep and returns
 * once, on delivery -- one wakeup per message, not per poll. No daemon, no
 * sockets, no deps: any number of agents on a shared filesystem get a symmetric,
 * totally-ordered, durable, greppable channel. Membership is a roster/ dir.
 *
 *   iac send  <room> <to>   [text...]   append one message (text from args/stdin)
 *   iac recv  <room> <me>   [seconds]   block for the next message addressed to me
 *   iac ack   <room> <me>   <id>        mark a claimed "?" task done (id from recv stderr)
 *   iac ask   <room> <to>   [text...]   send, then block for the reply, in one process
 *   iac join  <room> <me>               start at the log's end + register presence
 *   iac leave <room> <me>               drop presence
 *   iac hold  <room> <me>               presence BEACON: flock-hold until killed (run in bg)
 *   iac who   <room>                    list members: online (parked/held) or last-seen
 *   iac log   <room>                    print the whole room log
 *   iac compact <room>                  reclaim: drop frames every reader has passed
 *
 * Frame: <from>|<to>|<epoch>|<len>\n  then <len> body bytes  then \n
 * Sender name is $IAC_FROM (default "anon"). send appends with one writev under
 * flock, so concurrent senders never interleave and the log is a total order.
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <dirent.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/uio.h>

#define IAC_MSG_MAX (1 << 20)   /* 1 MiB per message; loud reject above  */
#define IAC_POLL_MS 100         /* recv poll interval while blocking (ms) */

static char g_body[IAC_MSG_MAX];   /* the one message buffer (send + recv) */

static int die(const char *msg) { fprintf(stderr, "iac: %s\n", msg); return 2; }

/* A NAME is one component: [A-Za-z0-9_-]+ (never escapes the room dir). */
static int ok_name(const char *s)
{
    if (s == NULL || *s == '\0') return 0;
    for (const char *p = s; *p; p++)
        if (!isalnum((unsigned char)*p) && *p != '_' && *p != '-') return 0;
    return 1;
}

/* A recipient SPEC is "*" or a comma-list of names (no empty segments). */
static int ok_spec(const char *s)
{
    size_t seg = 0;
    if (s != NULL && (strcmp(s, "*") == 0 || strcmp(s, "?") == 0)) return 1;
    if (s == NULL || *s == '\0') return 0;
    for (const char *p = s; *p; p++) {
        if (*p == ',') { if (seg == 0) return 0; seg = 0; }
        else if (isalnum((unsigned char)*p) || *p == '_' || *p == '-') seg++;
        else return 0;
    }
    return seg > 0;                          /* must not end on a comma */
}

/* Does recipient spec TO address ME? ("*" or ME is one of the comma segments) */
static int to_me(const char *to, const char *me)
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

static int p_log(char *b, size_t n, const char *room)
{ int k = snprintf(b, n, "%s/log", room); return (k > 0 && (size_t)k < n) ? 0 : -1; }
static int p_cur(char *b, size_t n, const char *room, const char *me)
{ int k = snprintf(b, n, "%s/%s.cur", room, me); return (k > 0 && (size_t)k < n) ? 0 : -1; }
static int p_ros(char *b, size_t n, const char *room, const char *me)
{ int k = snprintf(b, n, "%s/roster/%s", room, me); return (k > 0 && (size_t)k < n) ? 0 : -1; }

static long read_cursor(const char *path)
{
    long c = 0;
    FILE *f = fopen(path, "r");
    if (f == NULL) return 0;
    if (fscanf(f, "%ld", &c) != 1) c = 0;
    fclose(f);
    return c < 0 ? 0 : c;
}
static void write_cursor(const char *path, long c)
{
    FILE *f = fopen(path, "w");
    if (f == NULL) return;
    fprintf(f, "%ld\n", c);
    fclose(f);
}

/* ---- presence ---------------------------------------------------------- */
/* A roster entry is "<join_epoch> <pid> <seen_epoch>": when the member first
 * registered, the process behind the name, and when it was last active. Older
 * two-field entries are read with seen defaulting to join_epoch. */
static void roster_read(const char *rosp, long *join, long *pid, long *seen)
{
    char line[64];
    long a = 0, b = 0, c = 0;
    int n = 0;
    FILE *f = fopen(rosp, "r");
    *join = *pid = *seen = 0;
    if (f == NULL) return;
    if (fgets(line, sizeof line, f) != NULL) n = sscanf(line, "%ld %ld %ld", &a, &b, &c);
    fclose(f);
    if (n >= 2) { *join = a; *pid = b; *seen = (n >= 3) ? c : a; }
}
static void roster_put(const char *rosp, long join, long pid, long seen)
{
    FILE *f = fopen(rosp, "w");
    if (f == NULL) return;
    fprintf(f, "%ld %ld %ld\n", join, pid, seen);
    fclose(f);
}

/* presence via recv: a parked recv IS presence. Register ME (preserving an
 * existing join time / pid) with a fresh last-seen, and hold a SHARED flock on
 * the roster entry for the recv's whole life. who() probes with LOCK_EX|LOCK_NB,
 * so any held share reads as "online" -- a listening agent needs no separate
 * beacon. The lock is shared so an agent's own `iac hold` beacon and its recv
 * loop (the recommended pattern) coexist instead of deadlocking. Returns the
 * held fd (close to release) or -1 if presence could not be taken. */
static int presence_enter(const char *room, const char *me)
{
    char rosd[4096], rosp[4096];
    long join, pid, seen, now = (long)time(NULL);
    int fd;
    mkdir(room, 0700);
    if (snprintf(rosd, sizeof rosd, "%s/roster", room) >= (int)sizeof rosd) return -1;
    mkdir(rosd, 0700);
    if (p_ros(rosp, sizeof rosp, room, me)) return -1;
    fd = open(rosp, O_RDWR | O_CREAT, 0600);
    if (fd < 0) return -1;
    if (flock(fd, LOCK_SH) != 0) { close(fd); return -1; }
    roster_read(rosp, &join, &pid, &seen);
    roster_put(rosp, join > 0 ? join : now, pid > 0 ? pid : (long)getpid(), now);
    return fd;
}

/* ---- send -------------------------------------------------------------- */
static int cmd_send(const char *room, const char *to, char **argv, int argi, int argc)
{
    size_t blen = 0;
    const char *from;
    char path[4096], hdr[4096];
    char nl = '\n';
    int fd, hl, rc = 0;
    struct iovec iov[3];

    if (argi < argc && strcmp(argv[argi], "--") == 0) argi++;   /* optional end-of-opts */
    if (argi < argc) {                      /* body from the remaining args   */
        for (int i = argi; i < argc; i++) {
            size_t l = strlen(argv[i]);
            if (blen + l + 1 > sizeof g_body) return die("message too large");
            if (i > argi) g_body[blen++] = ' ';
            memcpy(g_body + blen, argv[i], l);
            blen += l;
        }
    } else {                                /* body from stdin (exact bytes)  */
        ssize_t r;
        while ((r = read(0, g_body + blen, sizeof g_body - blen)) > 0) {
            blen += (size_t)r;
            if (blen == sizeof g_body) return die("message too large");
        }
        if (r < 0) return die("read stdin failed");
    }

    from = getenv("IAC_FROM");
    if (from == NULL || !ok_name(from)) from = "anon";

    mkdir(room, 0700);                      /* create the room on first use   */
    if (p_log(path, sizeof path, room)) return die("path too long");
    fd = open(path, O_WRONLY | O_APPEND | O_CREAT, 0600);
    if (fd < 0) return die("cannot open room log");

    hl = snprintf(hdr, sizeof hdr, "%s|%s|%ld|%zu\n", from, to, (long)time(NULL), blen);
    if (hl <= 0 || (size_t)hl >= sizeof hdr) { close(fd); return die("header too long"); }
    iov[0].iov_base = hdr;    iov[0].iov_len = (size_t)hl;
    iov[1].iov_base = g_body; iov[1].iov_len = blen;
    iov[2].iov_base = &nl;    iov[2].iov_len = 1;

    flock(fd, LOCK_EX);                     /* serialize senders -> total order */
    if (writev(fd, iov, 3) != (ssize_t)((size_t)hl + blen + 1)) rc = 1;
    flock(fd, LOCK_UN);
    close(fd);
    return rc ? die("append failed") : 0;
}

/* ---- recv -------------------------------------------------------------- */
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
/* Claim the FRESH "?" task at log offset ID for ME: win by being the one to
 * create its claim file (O_CREAT|O_EXCL). EEXIST means a peer already has it --
 * lose. This is the hot path: the first free worker to scan the frame wins it,
 * competing consumers, no coordinator. Recovery of a stuck claim is separate
 * (recover_orphan), so a loser here simply moves on and never revisits it. */
static int claim_fresh(const char *room, long id, const char *me)
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

/* Steal the EXISTING claim at offset ID for ME iff its worker is presumed dead:
 * under flock, a "done" marker means it already completed (lose); an active claim
 * within TTL means a live worker owns it (lose); a claim older than the TTL means
 * the worker died unacked, so rewrite the epoch to now and win. The flock
 * serializes the expiry race, so exactly one stealer wins. */
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

/* ack: mark the "?" task at offset ID done, so it is never re-claimed after its
 * TTL. The id is the one recv printed on stderr when it handed ME the task. */
static int cmd_ack(const char *room, const char *me, long id)
{
    char clp[4096];
    int fd, rc;
    if (snprintf(clp, sizeof clp, "%s/claims/%ld", room, id) >= (int)sizeof clp) return die("path too long");
    fd = open(clp, O_WRONLY | O_CREAT, 0600);
    if (fd < 0) return die("cannot open claim");
    flock(fd, LOCK_EX);
    rc = (lseek(fd, 0, SEEK_SET) == 0 && ftruncate(fd, 0) == 0 &&
          claim_write(fd, "done", (long)time(NULL), me) == 0) ? 0 : 1;
    flock(fd, LOCK_UN);
    close(fd);
    return rc ? die("write failed") : 0;
}

/* Re-deliver one orphaned "?" task: an entry in claims/ still "claimed" but whose
 * worker has been silent past the TTL. The claim's filename IS the task's log
 * offset, so recovery is independent of any cursor -- steal it, then re-read and
 * deliver the frame at that offset. Returns 1 (and prints the task) iff one was
 * recovered for ME. This is what turns "?" from best-effort into run-to-completion. */
static int recover_orphan(const char *room, const char *me)
{
    char cld[4096], logp[4096], hdr[8192], from[256], to[4096];
    DIR *d;
    struct dirent *e;
    if (snprintf(cld, sizeof cld, "%s/claims", room) >= (int)sizeof cld) return 0;
    if (p_log(logp, sizeof logp, room)) return 0;
    d = opendir(cld);
    if (d == NULL) return 0;                       /* no claims dir yet */
    while ((e = readdir(d)) != NULL) {
        long off, epoch;
        size_t len;
        char *end;
        FILE *f;
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
            fclose(f);
            closedir(d);
            fprintf(stderr, "iac: from %s to ? at %ld claim %ld\n", from, epoch, off);
            fwrite(g_body, 1, len, stdout);
            fflush(stdout);
            return 1;
        }
        fclose(f);
    }
    closedir(d);
    return 0;
}

static int recv_loop(const char *room, const char *me, int timeout_s)
{
    char logp[4096], curp[4096], hdr[8192], from[256], to[4096];
    long cursor, epoch, frame_end;
    size_t len;
    int waited_ms = 0, limit_ms = timeout_s * 1000;
    struct timespec slp = { 0, IAC_POLL_MS * 1000000L };

    if (p_log(logp, sizeof logp, room)) return die("path too long");
    if (p_cur(curp, sizeof curp, room, me)) return die("path too long");
    cursor = read_cursor(curp);

    for (;;) {
        struct stat st;
        if (stat(logp, &st) == 0 && st.st_size > cursor) {
            FILE *f = fopen(logp, "r");
            if (f == NULL) return die("cannot open room log");
            if (fseek(f, cursor, SEEK_SET) != 0) { fclose(f); return die("seek failed"); }
            for (;;) {                                  /* scan frames forward */
                if (fgets(hdr, sizeof hdr, f) == NULL || strchr(hdr, '\n') == NULL)
                    break;                              /* no whole header yet */
                if (sscanf(hdr, "%255[^|]|%4095[^|]|%ld|%zu", from, to, &epoch, &len) != 4) {
                    fclose(f);
                    return die("corrupt frame");
                }
                frame_end = cursor + (long)strlen(hdr) + (long)len + 1;
                if (st.st_size < frame_end) break;      /* body not fully there */
                {
                    int is_claim = (strcmp(to, "?") == 0);
                    int mine = (strcmp(from, me) != 0) && (is_claim || to_me(to, me));
                    if (mine && is_claim && !claim_fresh(room, cursor, me)) mine = 0;   /* a peer claimed it */
                    if (mine) {
                        if (len > sizeof g_body) { fclose(f); return die("message too large"); }
                        if (fread(g_body, 1, len, f) != len) { fclose(f); return die("short read"); }
                        fclose(f);
                        if (is_claim)   /* id lets the worker `iac ack` on completion */
                            fprintf(stderr, "iac: from %s to ? at %ld claim %ld\n", from, epoch, cursor);
                        else
                            fprintf(stderr, "iac: from %s to %s at %ld\n", from, to, epoch);
                        fwrite(g_body, 1, len, stdout);
                        fflush(stdout);
                        write_cursor(curp, frame_end);
                        return 0;
                    }
                }
                cursor = frame_end;                     /* not for me (or lost claim): skip on */
                if (fseek(f, cursor, SEEK_SET) != 0) { fclose(f); return die("seek failed"); }
            }
            fclose(f);
            write_cursor(curp, cursor);                 /* persist scan progress */
        }
        if (recover_orphan(room, me)) return 0;         /* re-run a dead worker's task */
        if (waited_ms >= limit_ms) return 1;            /* nothing for me in time */
        nanosleep(&slp, NULL);
        waited_ms += IAC_POLL_MS;
    }
}

/* recv wrapper: hold presence (a shared roster flock + fresh last-seen) for the
 * whole blocking wait, so a parked receiver reads as "online" in who() with no
 * separate beacon. The lock releases when recv returns (or the process dies). */
static int cmd_recv(const char *room, const char *me, int timeout_s)
{
    int pfd = presence_enter(room, me);
    int rc = recv_loop(room, me, timeout_s);
    if (pfd >= 0) close(pfd);
    return rc;
}

/* follow: tail -f for my messages -- stream every frame addressed to me as it
 * lands (body to stdout with a trailing newline separator, from/to/when to
 * stderr), instead of returning after one. Observational, so it does NOT claim
 * "?" work. It advances my cursor like a normal recv (it IS a recv variant), and
 * holds presence while parked. Returns after IDLE_S seconds with nothing new. */
static int cmd_follow(const char *room, const char *me, int idle_s)
{
    char logp[4096], curp[4096], hdr[8192], from[256], to[4096];
    long cursor, epoch, frame_end;
    size_t len;
    int waited_ms = 0, limit_ms = idle_s * 1000, pfd;
    struct timespec slp = { 0, IAC_POLL_MS * 1000000L };
    if (p_log(logp, sizeof logp, room) || p_cur(curp, sizeof curp, room, me)) return die("path too long");
    pfd = presence_enter(room, me);
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
            nanosleep(&slp, NULL);
            waited_ms += IAC_POLL_MS;
        }
    }
    if (pfd >= 0) close(pfd);
    return 0;
}

/* ask: a round-trip in one process -- send the question to <to>, then block for
 * the next message addressed to me (timeout $IAC_ASK_TIMEOUT, default 60s). In a
 * 1:1 exchange that next message IS the reply. By design ask does NOT filter to
 * <to>: if some other message for me lands first, ask returns THAT rather than
 * dropping it -- filtering would mean consuming and losing it (one cursor, one
 * total order), which would break iac's no-message-loss guarantee. recv already
 * skips my own send, so my just-sent question never comes back to me. */
static int cmd_ask(const char *room, const char *to, char **argv, int argi, int argc)
{
    const char *me = getenv("IAC_FROM");
    const char *s = getenv("IAC_ASK_TIMEOUT");
    int t = (s != NULL && atoi(s) > 0) ? atoi(s) : 60;
    int rc;
    if (me == NULL || !ok_name(me)) me = "anon";
    rc = cmd_send(room, to, argv, argi, argc);   /* ask the question... */
    if (rc != 0) return rc;
    return cmd_recv(room, me, t);                 /* ...then wait for the answer */
}

/* ---- join / leave / who ------------------------------------------------ */
static int cmd_join(const char *room, const char *me)
{
    char logp[4096], curp[4096], rosd[4096], rosp[4096];
    struct stat st;
    long end;
    mkdir(room, 0700);
    if (snprintf(rosd, sizeof rosd, "%s/roster", room) >= (int)sizeof rosd) return die("path too long");
    mkdir(rosd, 0700);
    if (p_log(logp, sizeof logp, room)) return die("path too long");
    end = (stat(logp, &st) == 0) ? (long)st.st_size : 0;   /* future messages only */
    if (p_cur(curp, sizeof curp, room, me)) return die("path too long");
    write_cursor(curp, end);
    if (p_ros(rosp, sizeof rosp, room, me)) return die("path too long");
    { long now = (long)time(NULL); roster_put(rosp, now, (long)getpid(), now); }
    return 0;
}

static int cmd_leave(const char *room, const char *me)
{
    char rosp[4096];
    if (p_ros(rosp, sizeof rosp, room, me)) return die("path too long");
    unlink(rosp);
    return 0;
}

/* hold: a presence BEACON. Take a SHARED flock on this agent's roster entry for
 * the process's whole life and block. The lock releases automatically when the
 * process dies (even on SIGKILL), so who() reads liveness with nothing to reap.
 * The share is intentional: an agent's own recv loop also takes a shared lock on
 * the same entry (presence_enter), so a beacon and a parked recv coexist rather
 * than fight -- who() only cares that the share is held by *someone*. An agent
 * runs this in the background for its life; roster/<name> IS the name-to-pid map. */
static int cmd_hold(const char *room, const char *me)
{
    char rosd[4096], rosp[4096];
    long join, pid, seen, now = (long)time(NULL);
    int fd;
    mkdir(room, 0700);                       /* room created on demand, reused after */
    if (snprintf(rosd, sizeof rosd, "%s/roster", room) >= (int)sizeof rosd) return die("path too long");
    mkdir(rosd, 0700);
    if (p_ros(rosp, sizeof rosp, room, me)) return die("path too long");
    fd = open(rosp, O_RDWR | O_CREAT, 0600);
    if (fd < 0) return die("cannot open presence file");
    if (flock(fd, LOCK_SH) != 0) { close(fd); return die("cannot hold presence"); }
    roster_read(rosp, &join, &pid, &seen);   /* the beacon owns the name->pid map */
    roster_put(rosp, join > 0 ? join : now, (long)getpid(), now);
    for (;;) pause();                         /* hold until signaled/killed */
    return 0;                                /* not reached */
}

/* who: the name-to-pid roster, with liveness. A member is ONLINE if its entry is
 * flock held right now (a live beacon OR a parked recv), else OFFLINE -- and for
 * an offline member, "seen Ns ago" (its last recv) tells live-but-busy from gone,
 * so even a send/recv-only agent that never held a beacon is visible and legible.
 * The lock probe is the self-clearing signal: a crashed agent shows offline with
 * nothing to reap. */
static int cmd_who(const char *room)
{
    char rosd[4096], rosp[4096];
    struct dirent *e;
    DIR *d;
    long now = time(NULL);
    if (snprintf(rosd, sizeof rosd, "%s/roster", room) >= (int)sizeof rosd) return die("path too long");
    d = opendir(rosd);
    if (d == NULL) return 0;                 /* no members yet */
    while ((e = readdir(d)) != NULL) {
        long join = 0, pid = 0, seen = 0;
        int fd, held = 0;
        if (e->d_name[0] == '.') continue;
        if (p_ros(rosp, sizeof rosp, room, e->d_name)) continue;
        roster_read(rosp, &join, &pid, &seen);
        fd = open(rosp, O_RDONLY);
        if (fd >= 0) {
            if (flock(fd, LOCK_EX | LOCK_NB) != 0) held = 1;   /* held elsewhere = alive */
            else flock(fd, LOCK_UN);
            close(fd);
        }
        if (held)
            printf("%-14s online   pid %-8ld active now\n", e->d_name, pid);
        else
            printf("%-14s offline  pid %-8ld seen %lds ago\n", e->d_name, pid, now - seen);
    }
    closedir(d);
    return 0;
}

/* Step over one frame in F from the current offset, skipping its body. Returns
 * the frame's byte length, 0 at clean EOF, or -1 on a partial/corrupt tail.
 * Leaves F positioned at the start of the next frame. */
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

/* Print the room log. With TAIL > 0, print only the last TAIL frames (orientation
 * for a freshly spawned agent) -- two forward passes: count, then skip all but
 * the last TAIL and dump the rest verbatim. */
static int cmd_log(const char *room, long tail)
{
    char logp[4096], buf[8192];
    size_t n;
    FILE *f;
    if (p_log(logp, sizeof logp, room)) return die("path too long");
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

/* compact: reclaim a room whose log and claims/ have grown unbounded. Drop every
 * frame that lies before `keep` = the minimum over all <name>.cur cursors (the
 * point every registered reader has already consumed), then shift cursors and
 * re-key claims by that amount so the offset-addressed world stays consistent.
 *
 * The log is shifted left IN PLACE under its append lock -- no inode swap, so a
 * sender blocked on the lock still appends to the live file (nothing lost) and
 * the log is never left partial. It is a maintenance op: run it in a lull. A recv
 * that races the shift may see one frame wrong and exit 2 -- the caller just
 * recv's again, now off the correct (shifted) cursor. */
static int cmd_compact(const char *room)
{
    char logp[4096], cld[4096], p1[4096], p2[4096];
    long keep = -1, cursors = 0, surv[8192];
    int fd, ns = 0, capped = 0, i;
    DIR *d;
    struct dirent *e;

    if (p_log(logp, sizeof logp, room)) return die("path too long");

    /* 1. keep = min over every <name>.cur */
    d = opendir(room);
    if (d == NULL) return die("no such room");
    while ((e = readdir(d)) != NULL) {
        size_t l = strlen(e->d_name);
        long c;
        if (l < 5 || strcmp(e->d_name + l - 4, ".cur") != 0) continue;
        if (snprintf(p1, sizeof p1, "%s/%s", room, e->d_name) >= (int)sizeof p1) continue;
        c = read_cursor(p1);
        if (keep < 0 || c < keep) keep = c;
        cursors++;
    }
    closedir(d);
    if (keep <= 0) { printf("compact: nothing to drop (min cursor %ld over %ld reader(s))\n",
                            keep < 0 ? 0 : keep, cursors); return 0; }

    /* 2. shift the log left by `keep` bytes, in place, under the append lock */
    fd = open(logp, O_RDWR);
    if (fd < 0) return die("cannot open room log");
    flock(fd, LOCK_EX);
    {
        struct stat st;
        char buf[65536];
        long src, dst, rem;
        if (fstat(fd, &st) != 0) { flock(fd, LOCK_UN); close(fd); return die("stat failed"); }
        if (keep > (long)st.st_size) keep = (long)st.st_size;
        src = keep; dst = 0; rem = (long)st.st_size - keep;
        while (rem > 0) {
            size_t want = rem < (long)sizeof buf ? (size_t)rem : sizeof buf;
            ssize_t r = pread(fd, buf, want, src);
            if (r <= 0 || pwrite(fd, buf, (size_t)r, dst) != r) break;
            src += r; dst += r; rem -= r;
        }
        if (ftruncate(fd, (long)st.st_size - keep) != 0) rem = -1;   /* note failure below */
        (void)rem;
    }
    flock(fd, LOCK_UN);
    close(fd);

    /* 3. shift every cursor down by keep (all are >= keep, so none goes negative) */
    d = opendir(room);
    if (d != NULL) {
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

    /* 4. re-key claims: drop those whose frame was dropped (offset < keep), shift
     *    survivors down by keep. Collect first (no readdir-vs-rename races), then
     *    rename in two phases via a .t suffix so numeric names never collide. */
    if (snprintf(cld, sizeof cld, "%s/claims", room) < (int)sizeof cld) {
        d = opendir(cld);
        if (d != NULL) {
            while ((e = readdir(d)) != NULL) {
                char *ep;
                long o;
                if (e->d_name[0] == '.') continue;
                o = strtol(e->d_name, &ep, 10);
                if (*ep != '\0') continue;
                if (o < keep) {
                    if (snprintf(p1, sizeof p1, "%s/%ld", cld, o) < (int)sizeof p1) unlink(p1);
                } else if (ns < (int)(sizeof surv / sizeof surv[0])) surv[ns++] = o;
                else capped = 1;
            }
            closedir(d);
            for (i = 0; i < ns; i++)
                if (snprintf(p1, sizeof p1, "%s/%ld", cld, surv[i]) < (int)sizeof p1 &&
                    snprintf(p2, sizeof p2, "%s/%ld.t", cld, surv[i] - keep) < (int)sizeof p2) rename(p1, p2);
            for (i = 0; i < ns; i++)
                if (snprintf(p1, sizeof p1, "%s/%ld.t", cld, surv[i] - keep) < (int)sizeof p1 &&
                    snprintf(p2, sizeof p2, "%s/%ld", cld, surv[i] - keep) < (int)sizeof p2) rename(p1, p2);
        }
    }
    printf("compact: dropped %ld bytes, shifted %ld cursor(s), re-keyed %d claim(s)%s\n",
           keep, cursors, ns, capped ? " (claim list capped)" : "");
    return 0;
}

/* One-screen help: every verb, and the env knobs that shape them. */
static void usage(FILE *out)
{
    fputs(
        "iac -- inter-agent communication over a shared-log room\n\n"
        "usage:\n"
        "  iac send  <room> <to> [text...]  append a message (to: name | a,b,c | * | ?; stdin if no text)\n"
        "  iac recv  <room> <me> [secs]     block for the next message addressed to me (default 60)\n"
        "  iac ask   <room> <to> [text...]  send, then block for the reply (timeout $IAC_ASK_TIMEOUT)\n"
        "  iac ack   <room> <me> <id>       mark a claimed \"?\" task done (id from recv's stderr)\n"
        "  iac join  <room> <me>            register and start at the log's end (skip backlog)\n"
        "  iac leave <room> <me>            drop registration\n"
        "  iac hold  <room> <me>            presence beacon: hold until killed (run in background)\n"
        "  iac who   <room>                 list members: online (parked/held) or last-seen\n"
        "  iac log   <room> [-n K]          print the room log (last K frames with -n K)\n"
        "  iac compact <room>               drop frames every reader has passed; re-key cursors/claims\n\n"
        "env:\n"
        "  IAC_FROM=<name>       sender name (default anon)\n"
        "  IAC_CLAIM_TTL=<secs>  a \"?\" task is re-claimable this long after an unacked claim (300)\n"
        "  IAC_ASK_TIMEOUT=<secs> how long `ask` waits for the reply (60)\n",
        out);
}

int main(int argc, char **argv)
{
    const char *cmd;
    if (argc >= 2 && (strcmp(argv[1], "help") == 0 || strcmp(argv[1], "-h") == 0 ||
                      strcmp(argv[1], "--help") == 0)) { usage(stdout); return 0; }
    if (argc < 3) { usage(stderr); return 2; }
    cmd = argv[1];

    if (strcmp(cmd, "send") == 0) {
        if (argc < 4) return die("usage: iac send <room> <to> [text...]");
        if (!ok_spec(argv[3])) return die("bad recipient (name, a,b,c, or *)");
        return cmd_send(argv[2], argv[3], argv, 4, argc);
    }
    if (strcmp(cmd, "recv") == 0) {
        const char *room = NULL, *me = NULL, *secs = NULL;
        int follow = 0, t, i;
        for (i = 2; i < argc; i++) {           /* positionals, with --follow anywhere */
            if (strcmp(argv[i], "--follow") == 0 || strcmp(argv[i], "-f") == 0) follow = 1;
            else if (room == NULL) room = argv[i];
            else if (me == NULL) me = argv[i];
            else if (secs == NULL) secs = argv[i];
        }
        if (me == NULL || !ok_name(me)) return die("usage: iac recv <room> <me> [seconds] [--follow]");
        t = (secs != NULL) ? atoi(secs) : 60;
        if (t < 0) t = 0;
        return follow ? cmd_follow(room, me, t) : cmd_recv(room, me, t);
    }
    if (strcmp(cmd, "ack") == 0) {
        if (argc < 5 || !ok_name(argv[3])) return die("usage: iac ack <room> <me> <id>");
        return cmd_ack(argv[2], argv[3], atol(argv[4]));
    }
    if (strcmp(cmd, "ask") == 0) {
        if (argc < 4) return die("usage: iac ask <room> <to> [text...]");
        if (!ok_spec(argv[3])) return die("bad recipient (name, a,b,c, or *)");
        return cmd_ask(argv[2], argv[3], argv, 4, argc);
    }
    if (strcmp(cmd, "join") == 0) {
        if (argc < 4 || !ok_name(argv[3])) return die("usage: iac join <room> <me>");
        return cmd_join(argv[2], argv[3]);
    }
    if (strcmp(cmd, "leave") == 0) {
        if (argc < 4 || !ok_name(argv[3])) return die("usage: iac leave <room> <me>");
        return cmd_leave(argv[2], argv[3]);
    }
    if (strcmp(cmd, "hold") == 0) {
        if (argc < 4 || !ok_name(argv[3])) return die("usage: iac hold <room> <me>");
        return cmd_hold(argv[2], argv[3]);
    }
    if (strcmp(cmd, "who") == 0)  return cmd_who(argv[2]);
    if (strcmp(cmd, "compact") == 0) return cmd_compact(argv[2]);
    if (strcmp(cmd, "log") == 0) {
        long tail = (argc >= 5 && strcmp(argv[3], "-n") == 0) ? atol(argv[4]) : 0;
        return cmd_log(argv[2], tail);
    }
    usage(stderr);
    return die("unknown command");
}
