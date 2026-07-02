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
 * The wait happens here in C, so a parked recv is one process asleep and returns
 * once, on delivery -- one wakeup per message, not per poll. No daemon, no
 * sockets, no deps: any number of agents on a shared filesystem get a symmetric,
 * totally-ordered, durable, greppable channel. Membership is a roster/ dir.
 *
 *   iac send  <room> <to>   [text...]   append one message (text from args/stdin)
 *   iac recv  <room> <me>   [seconds]   block for the next message addressed to me
 *   iac join  <room> <me>               start at the log's end + register presence
 *   iac leave <room> <me>               drop presence
 *   iac hold  <room> <me>               presence BEACON: flock-hold until killed (run in bg)
 *   iac who   <room>                    list members: online (beacon held) or offline
 *   iac log   <room>                    print the whole room log
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
/* Atomically claim the task at log offset ID (a to="?" message): the first
 * recv-er to create the claim file wins (O_CREAT|O_EXCL); the rest skip it. So a
 * "?" task runs exactly once, whoever is free -- competing consumers, no coordinator. */
static int claim_won(const char *room, long id)
{
    char cld[4096], clp[4096];
    int fd;
    if (snprintf(cld, sizeof cld, "%s/claims", room) >= (int)sizeof cld) return 0;
    mkdir(cld, 0700);
    if (snprintf(clp, sizeof clp, "%s/claims/%ld", room, id) >= (int)sizeof clp) return 0;
    fd = open(clp, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return 0;                     /* EEXIST: already claimed by a peer */
    close(fd);
    return 1;
}

static int cmd_recv(const char *room, const char *me, int timeout_s)
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
                    if (mine && is_claim && !claim_won(room, cursor)) mine = 0;   /* lost the race */
                    if (mine) {
                        if (len > sizeof g_body) { fclose(f); return die("message too large"); }
                        if (fread(g_body, 1, len, f) != len) { fclose(f); return die("short read"); }
                        fclose(f);
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
        if (waited_ms >= limit_ms) return 1;            /* nothing for me in time */
        nanosleep(&slp, NULL);
        waited_ms += IAC_POLL_MS;
    }
}

/* ---- join / leave / who ------------------------------------------------ */
static int cmd_join(const char *room, const char *me)
{
    char logp[4096], curp[4096], rosd[4096], rosp[4096];
    struct stat st;
    long end;
    FILE *f;
    mkdir(room, 0700);
    if (snprintf(rosd, sizeof rosd, "%s/roster", room) >= (int)sizeof rosd) return die("path too long");
    mkdir(rosd, 0700);
    if (p_log(logp, sizeof logp, room)) return die("path too long");
    end = (stat(logp, &st) == 0) ? (long)st.st_size : 0;   /* future messages only */
    if (p_cur(curp, sizeof curp, room, me)) return die("path too long");
    write_cursor(curp, end);
    if (p_ros(rosp, sizeof rosp, room, me)) return die("path too long");
    f = fopen(rosp, "w");
    if (f != NULL) { fprintf(f, "%ld %ld\n", (long)time(NULL), (long)getpid()); fclose(f); }
    return 0;
}

static int cmd_leave(const char *room, const char *me)
{
    char rosp[4096];
    if (p_ros(rosp, sizeof rosp, room, me)) return die("path too long");
    unlink(rosp);
    return 0;
}

/* hold: a presence BEACON. flock this agent's roster entry (name -> pid) for the
 * process's whole life and block. The lock releases automatically when the
 * process dies (even on SIGKILL), so who() reads liveness with nothing to reap
 * and no stale-pid guessing. An agent runs this in the background for its life;
 * from outside, roster/<name> IS the name-to-pid map. */
static int cmd_hold(const char *room, const char *me)
{
    char rosd[4096], rosp[4096], line[64];
    int fd, n;
    mkdir(room, 0700);                       /* room created on demand, reused after */
    if (snprintf(rosd, sizeof rosd, "%s/roster", room) >= (int)sizeof rosd) return die("path too long");
    mkdir(rosd, 0700);
    if (p_ros(rosp, sizeof rosp, room, me)) return die("path too long");
    fd = open(rosp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return die("cannot open presence file");
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) { close(fd); return die("presence already held for this name"); }
    n = snprintf(line, sizeof line, "%ld %ld\n", (long)time(NULL), (long)getpid());
    if (n <= 0 || write(fd, line, (size_t)n) != n) { close(fd); return die("write failed"); }
    for (;;) pause();                        /* hold until signaled/killed */
    return 0;                                /* not reached */
}

/* who: the name-to-pid roster, with liveness. A member is ONLINE if its entry is
 * flock held right now (a live beacon), else OFFLINE. The lock probe is the
 * self-clearing signal: a crashed agent shows offline with nothing to clean. */
static int cmd_who(const char *room)
{
    char rosd[4096], rosp[4096], line[64];
    struct dirent *e;
    DIR *d;
    long now = time(NULL);
    if (snprintf(rosd, sizeof rosd, "%s/roster", room) >= (int)sizeof rosd) return die("path too long");
    d = opendir(rosd);
    if (d == NULL) return 0;                 /* no members yet */
    while ((e = readdir(d)) != NULL) {
        long epoch = 0, pid = 0;
        int fd, held = 0;
        if (e->d_name[0] == '.') continue;
        if (p_ros(rosp, sizeof rosp, room, e->d_name)) continue;
        fd = open(rosp, O_RDONLY);
        if (fd >= 0) {
            ssize_t r = read(fd, line, sizeof line - 1);
            line[r > 0 ? r : 0] = '\0';
            sscanf(line, "%ld %ld", &epoch, &pid);
            if (flock(fd, LOCK_EX | LOCK_NB) != 0) held = 1;   /* held elsewhere = alive */
            else flock(fd, LOCK_UN);
            close(fd);
        }
        printf("%-14s %-8s pid %-8ld since %lds ago\n",
               e->d_name, held ? "online" : "offline", pid, now - epoch);
    }
    closedir(d);
    return 0;
}

static int cmd_log(const char *room)
{
    char logp[4096], buf[8192];
    size_t n;
    FILE *f;
    if (p_log(logp, sizeof logp, room)) return die("path too long");
    f = fopen(logp, "r");
    if (f == NULL) return die("no such room");
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) fwrite(buf, 1, n, stdout);
    fclose(f);
    return 0;
}

int main(int argc, char **argv)
{
    const char *cmd;
    if (argc < 3) {
        fprintf(stderr, "usage: iac send|recv|join|leave|hold|who|log <room> [name] ...\n");
        return 2;
    }
    cmd = argv[1];

    if (strcmp(cmd, "send") == 0) {
        if (argc < 4) return die("usage: iac send <room> <to> [text...]");
        if (!ok_spec(argv[3])) return die("bad recipient (name, a,b,c, or *)");
        return cmd_send(argv[2], argv[3], argv, 4, argc);
    }
    if (strcmp(cmd, "recv") == 0) {
        int t;
        if (argc < 4) return die("usage: iac recv <room> <me> [seconds]");
        if (!ok_name(argv[3])) return die("bad name");
        t = (argc >= 5) ? atoi(argv[4]) : 60;
        if (t < 0) t = 0;
        return cmd_recv(argv[2], argv[3], t);
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
    if (strcmp(cmd, "log") == 0)  return cmd_log(argv[2]);
    return die("unknown command (send|recv|join|leave|hold|who|log)");
}
