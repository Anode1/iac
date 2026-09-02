/* iac -- see send.h.
 * Copyright (c) 2026 Vasili Gavrilov. ISC License; see LICENSE. */
#include "send.h"
#include "recv.h"
#include "frame.h"
#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/uio.h>

int cmd_send(const char *room, const char *to, char **argv, int argi, int argc)
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
    if (p_log(path, sizeof path, room)) return die_path();
    fd = open(path, O_WRONLY | O_APPEND | O_CREAT, 0600);
    if (fd < 0) return die("cannot open room log");

    hl = snprintf(hdr, sizeof hdr, "%s|%s|%ld|%zu\n", from, to, (long)time(NULL), blen);
    if (hl <= 0 || (size_t)hl >= sizeof hdr) { close(fd); return die("header too long"); }
    iov[0].iov_base = hdr;    iov[0].iov_len = (size_t)hl;
    iov[1].iov_base = g_body; iov[1].iov_len = blen;
    iov[2].iov_base = &nl;    iov[2].iov_len = 1;

    {
        struct stat st;
        size_t total = (size_t)hl + blen + 1;
        off_t pre;
        flock(fd, LOCK_EX);                 /* serialize senders -> total order */
        pre = (fstat(fd, &st) == 0) ? st.st_size : (off_t)-1;
        if ((size_t)writev(fd, iov, 3) != total) {
            /* torn/short append: roll the log back to before this frame so a
             * partial one never poisons the shared stream for every reader */
            if (pre >= 0 && ftruncate(fd, pre) != 0) { /* best effort */ }
            rc = 1;
        }
        flock(fd, LOCK_UN);
    }
    close(fd);
    return rc ? die("append failed") : 0;
}

/* One-process round-trip: send to <to>, then recv the next message for me (timeout $IAC_ASK_TIMEOUT); unfiltered, so nothing is dropped. */
int cmd_ask(const char *room, const char *to, char **argv, int argi, int argc)
{
    const char *me = getenv("IAC_FROM");
    const char *s = getenv("IAC_ASK_TIMEOUT");
    int t = (s != NULL && atoi(s) > 0) ? atoi(s) : 60;
    int rc;
    if (me == NULL || !ok_name(me)) me = "anon";
    rc = cmd_send(room, to, argv, argi, argc);   /* ask the question... */
    if (rc != 0) return rc;
    return cmd_recv(room, me, t, 0);              /* ...then wait for the one answer */
}
