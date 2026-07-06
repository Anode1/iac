/* iac -- see presence.h.
 * Copyright (c) 2026 Vasili Gavrilov. ISC License; see LICENSE. */
#include "presence.h"
#include "frame.h"
#include "common.h"

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <dirent.h>
#include <sys/file.h>
#include <sys/stat.h>

/* Parse a roster entry "<join> <pid> <seen>" (old 2-field: seen defaults to join). */
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

int presence_enter(const char *room, const char *me)
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
    /* refresh pid to THIS live holder every enter: keeping the stored pid let a
     * dead beacon's pid linger while a recv-only agent held the flock (stale who). */
    roster_put(rosp, join > 0 ? join : now, (long)getpid(), now);
    (void)pid; (void)seen;
    return fd;
}

int cmd_join(const char *room, const char *me)
{
    char logp[4096], curp[4096], rosd[4096], rosp[4096];
    struct stat st;
    long end;
    mkdir(room, 0700);
    if (snprintf(rosd, sizeof rosd, "%s/roster", room) >= (int)sizeof rosd) return die_path();
    mkdir(rosd, 0700);
    if (p_log(logp, sizeof logp, room)) return die_path();
    end = (stat(logp, &st) == 0) ? (long)st.st_size : 0;   /* future messages only */
    if (p_cur(curp, sizeof curp, room, me)) return die_path();
    write_cursor(curp, end);
    if (p_ros(rosp, sizeof rosp, room, me)) return die_path();
    { long now = (long)time(NULL); roster_put(rosp, now, (long)getpid(), now); }
    return 0;
}

int cmd_leave(const char *room, const char *me)
{
    char rosp[4096];
    if (p_ros(rosp, sizeof rosp, room, me)) return die_path();
    unlink(rosp);
    return 0;
}

/* Presence beacon: hold a SHARED roster flock until killed (shared so it coexists with the agent's own recv); the OS drops it on death. */
int cmd_hold(const char *room, const char *me)
{
    char rosd[4096], rosp[4096];
    long join, pid, seen, now = (long)time(NULL);
    int fd;
    mkdir(room, 0700);                       /* room created on demand, reused after */
    if (snprintf(rosd, sizeof rosd, "%s/roster", room) >= (int)sizeof rosd) return die_path();
    mkdir(rosd, 0700);
    if (p_ros(rosp, sizeof rosp, room, me)) return die_path();
    fd = open(rosp, O_RDWR | O_CREAT, 0600);
    if (fd < 0) return die("cannot open presence file");
    if (flock(fd, LOCK_SH) != 0) { close(fd); return die("cannot hold presence"); }
    roster_read(rosp, &join, &pid, &seen);   /* the beacon owns the name->pid map */
    roster_put(rosp, join > 0 ? join : now, (long)getpid(), now);
    (void)pid; (void)seen;
    for (;;) pause();                         /* hold until signaled/killed */
    return 0;                                /* not reached */
}

/* List members: online if the roster flock is held (beacon or parked recv), else offline with "seen Ns ago". */
int cmd_who(const char *room)
{
    char rosd[4096], rosp[4096];
    struct dirent *e;
    DIR *d;
    long now = time(NULL);
    if (snprintf(rosd, sizeof rosd, "%s/roster", room) >= (int)sizeof rosd) return die_path();
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
