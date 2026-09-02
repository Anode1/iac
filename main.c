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
 * sockets, no deps. Frame: <from>|<to>|<epoch>|<len>\n then <len> body bytes then \n.
 *
 * main.c holds argv and dispatches; each verb lives in its own module.
 * Copyright (c) 2026 Vasili Gavrilov. ISC License; see LICENSE.
 */
#include "send.h"
#include "recv.h"
#include "claim.h"
#include "presence.h"
#include "compact.h"
#include "frame.h"
#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* recv takes an option (-f/--follow) that may appear anywhere among its
 * positionals, so it gets its own parse rather than bloating the dispatch. */
static int run_recv(int argc, char **argv)
{
    const char *room = NULL, *me = NULL, *secs = NULL;
    int follow = 0, all = 0, t, i;
    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--follow") == 0 || strcmp(argv[i], "-f") == 0) follow = 1;
        else if (strcmp(argv[i], "--all") == 0 || strcmp(argv[i], "-a") == 0) all = 1;
        else if (room == NULL) room = argv[i];
        else if (me == NULL) me = argv[i];
        else if (secs == NULL) secs = argv[i];
    }
    if (me == NULL || !ok_name(me)) return die("usage: iac recv <room> <me> [seconds] [--follow] [--all]");
    t = (secs != NULL) ? atoi(secs) : 60;
    if (t < 0) t = 0;
    return follow ? cmd_follow(room, me, t) : cmd_recv(room, me, t, all);
}

/* One-screen help: every verb, and the env knobs that shape them. */
static void usage(FILE *out)
{
    fputs(
        "iac -- inter-agent communication over a shared-log room\n\n"
        "usage:\n"
        "  iac send  <room> <to> [text...]  append a message (to: name | a,b,c | * | ?; stdin if no text)\n"
        "  iac recv  <room> <me> [secs] [-f] block for the next message for me (default 60; -f/--follow: tail -f, no claim;\n"
        "                                   -a/--all: deliver the whole queued burst in one return, newline-separated)\n"
        "  iac drain <room> <me>            deliver ALL my queued messages at once, non-blocking (exit 1 if none)\n"
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
        if (!ok_spec(argv[3])) return die("bad recipient (name, a,b,c, *, or ?)");
        return cmd_send(argv[2], argv[3], argv, 4, argc);
    }
    if (strcmp(cmd, "recv") == 0) return run_recv(argc, argv);
    if (strcmp(cmd, "drain") == 0) {
        if (argc < 4 || !ok_name(argv[3])) return die("usage: iac drain <room> <me>");
        return cmd_drain(argv[2], argv[3]);
    }
    if (strcmp(cmd, "ack") == 0) {
        if (argc < 5 || !ok_name(argv[3])) return die("usage: iac ack <room> <me> <id>");
        return cmd_ack(argv[2], argv[3], atol(argv[4]));
    }
    if (strcmp(cmd, "ask") == 0) {
        if (argc < 4) return die("usage: iac ask <room> <to> [text...]");
        if (!ok_spec(argv[3])) return die("bad recipient (name, a,b,c, *, or ?)");
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
