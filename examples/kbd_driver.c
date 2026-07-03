/* kbd_driver -- a keyboard-priority driver for an iac board (reference example).
 *
 * An LLM agent cannot be event-driven itself; a *driver* is the loop that decides
 * when to invoke it and with what. This one keeps a human in control of a fleet:
 * it waits on the keyboard and the board at once, in one kernel sleep, and always
 * services the keyboard first -- so a typed command preempts the message backlog,
 * and between commands the agent works through board messages one at a time.
 *
 * It is the runnable form of doc/dev/RECEIVE_MODEL.md section 6. The board is made
 * waitable by an inotify watch on <room>/log (Linux); elsewhere a 1s poll of the
 * board is the fallback. Messages are read with `iac recv <room> <me> 0` (the
 * non-blocking, one-at-a-time read), so the driver stays tiny -- it is glue over
 * iac's verbs, not a reimplementation.
 *
 *   cc examples/kbd_driver.c -o kbd_driver
 *   IAC=./iac ./kbd_driver <room> <me> ['model command']
 *
 * With no model command it just prints what it *would* hand the model, tagged by
 * source ([you] / [board]) -- run it, type a line, and `iac send <room> <me> hi`
 * from another shell to see keyboard-priority interleaving. Give a command (a
 * shell pipeline, your agent runner) and each input is fed to it on stdin.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <sys/wait.h>
#if defined(__linux__)
#include <sys/inotify.h>
#endif

/* the iac binary to shell out to (default: `iac` on $PATH) */
static const char *iac(void) { const char *p = getenv("IAC"); return (p && *p) ? p : "iac"; }

/* Hand INPUT to the model, tagged by SRC. No command -> just print it (demo). */
static void invoke_model(const char *model, const char *src, const char *input)
{
    FILE *p;
    if (model == NULL) {
        printf("[%s] %s", src, input);
        if (input[0] == '\0' || input[strlen(input) - 1] != '\n') putchar('\n');
        fflush(stdout);
        return;
    }
    fprintf(stderr, "[driver] %s -> model\n", src);
    p = popen(model, "w");                    /* the model's own stdout is inherited */
    if (p == NULL) { perror("popen"); return; }
    fputs(input, p);
    if (input[0] == '\0' || input[strlen(input) - 1] != '\n') fputc('\n', p);
    pclose(p);
}

/* Read ONE board message for ME into BUF via `iac recv 0`; 1 if one arrived, else 0. */
static int recv_one(const char *room, const char *me, char *buf, size_t n)
{
    char cmd[8192];
    FILE *p;
    size_t got;
    int st;
    if ((size_t)snprintf(cmd, sizeof cmd, "%s recv %s %s 0 2>/dev/null", iac(), room, me) >= sizeof cmd)
        return 0;
    p = popen(cmd, "r");
    if (p == NULL) return 0;
    got = fread(buf, 1, n - 1, p);
    buf[got] = '\0';
    st = pclose(p);
    return (st != -1 && WIFEXITED(st) && WEXITSTATUS(st) == 0) ? 1 : 0;   /* exit 0 = delivered */
}

/* Is stdin readable right now (data, or EOF/error to notice)? non-blocking. */
static int stdin_ready(void)
{
    struct pollfd pfd = { 0, POLLIN, 0 };
    return poll(&pfd, 1, 0) > 0 && (pfd.revents & (POLLIN | POLLHUP | POLLERR));
}

/* Sleep until the keyboard or the board has something (or 1s passes). */
static void wait_ready(int ino, const char *roomlog)
{
    struct pollfd pfd[2];
    int n = 1;
    pfd[0].fd = 0; pfd[0].events = POLLIN;
#if defined(__linux__)
    if (ino >= 0) { inotify_add_watch(ino, roomlog, IN_MODIFY);   /* idempotent; attaches once the log exists */
                    pfd[1].fd = ino; pfd[1].events = POLLIN; n = 2; }
#else
    (void)ino; (void)roomlog;
#endif
    poll(pfd, n, 1000);    /* inotify/keyboard wake instantly; 1s is the board safety re-check */
}

int main(int argc, char **argv)
{
    const char *room, *me, *model;
    char roomlog[4096], line[8192];
    int ino = -1;

    if (argc < 3) { fprintf(stderr, "usage: kbd_driver <room> <me> ['model command']\n"); return 2; }
    room = argv[1]; me = argv[2]; model = (argc >= 4) ? argv[3] : NULL;
    if ((size_t)snprintf(roomlog, sizeof roomlog, "%s/log", room) >= sizeof roomlog) return 2;

#if defined(__linux__)
    ino = inotify_init1(IN_NONBLOCK);
#endif
    fprintf(stderr, "[driver] up on room=%s as '%s' -- type to command (Ctrl-D quits); board messages run between your lines\n", room, me);

    for (;;) {
        wait_ready(ino, roomlog);
#if defined(__linux__)
        if (ino >= 0) { char b[4096]; while (read(ino, b, sizeof b) > 0) { } }   /* clear inotify (level-triggered) */
#endif
        for (;;) {                                       /* service until idle, KEYBOARD FIRST */
            if (stdin_ready()) {
                if (fgets(line, sizeof line, stdin) == NULL) { fprintf(stderr, "[driver] eof -- bye\n"); goto done; }
                if (line[0] != '\n') invoke_model(model, "you", line);   /* a human line jumps the queue */
                continue;
            }
            if (recv_one(room, me, line, sizeof line)) invoke_model(model, "board", line);
            else break;                                  /* board empty too -> back to sleep */
        }
    }
done:
    if (ino >= 0) close(ino);
    return 0;
}
