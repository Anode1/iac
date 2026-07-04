/* End-to-end tests for iac: drive the real binary over scratch rooms, one room
 * per case so cursors stay isolated. Linear and self-contained. */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE          /* glibc: re-expose BSD mkdtemp()/etc. under strict _POSIX_C_SOURCE */
#define _DARWIN_C_SOURCE         /* macOS: the same (strict _POSIX_C_SOURCE hides them); no-op on glibc */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0;
#define CHECK(c, m) do { if (c) printf("  ok   %s\n", m); \
    else { printf("  FAIL %s\n", m); fails++; } } while (0)

static const char *slurp(const char *path, char *buf, size_t n)
{
    FILE *f = fopen(path, "r");
    size_t r = 0;
    if (f != NULL) { r = fread(buf, 1, n - 1, f); fclose(f); }
    buf[r] = '\0';
    return buf;
}

int main(void)
{
    char base[] = "/tmp/iac_ut_XXXXXX";
    char cmd[8192], out[4096], path[1200], room[1100];

    if (mkdtemp(base) == NULL) { perror("mkdtemp"); return 2; }

    /* 1. point-to-point: only the addressee gets it */
    snprintf(room, sizeof room, "%s/r1", base);
    snprintf(cmd, sizeof cmd, "IAC_FROM=A ./iac send %s B -- ping", room); if (system(cmd)) {}
    snprintf(cmd, sizeof cmd, "./iac recv %s B 3 >%s/o 2>/dev/null", room, base); if (system(cmd)) {}
    snprintf(path, sizeof path, "%s/o", base);
    CHECK(strcmp(slurp(path, out, sizeof out), "ping") == 0, "p2p: the addressee receives");
    snprintf(cmd, sizeof cmd, "./iac recv %s A 1 >/dev/null 2>&1; printf %%d $? >%s/r", room, base); if (system(cmd)) {}
    snprintf(path, sizeof path, "%s/r", base);
    CHECK(strcmp(slurp(path, out, sizeof out), "1") == 0, "p2p: a non-addressee gets nothing");

    /* 2. broadcast: one append, every member receives (own cursor each) */
    snprintf(room, sizeof room, "%s/r2", base);
    snprintf(cmd, sizeof cmd, "IAC_FROM=X ./iac send %s '*' -- hi-all", room); if (system(cmd)) {}
    snprintf(cmd, sizeof cmd, "./iac recv %s A 3 >%s/oa 2>/dev/null", room, base); if (system(cmd)) {}
    snprintf(cmd, sizeof cmd, "./iac recv %s B 3 >%s/ob 2>/dev/null", room, base); if (system(cmd)) {}
    snprintf(path, sizeof path, "%s/oa", base);
    CHECK(strcmp(slurp(path, out, sizeof out), "hi-all") == 0, "broadcast: member A receives");
    snprintf(path, sizeof path, "%s/ob", base);
    CHECK(strcmp(slurp(path, out, sizeof out), "hi-all") == 0, "broadcast: member B receives");

    /* 3. subset/multicast: only the listed names receive */
    snprintf(room, sizeof room, "%s/r3", base);
    snprintf(cmd, sizeof cmd, "IAC_FROM=X ./iac send %s A,C -- subset", room); if (system(cmd)) {}
    snprintf(cmd, sizeof cmd, "./iac recv %s A 3 >%s/oa 2>/dev/null", room, base); if (system(cmd)) {}
    snprintf(cmd, sizeof cmd, "./iac recv %s C 3 >%s/oc 2>/dev/null", room, base); if (system(cmd)) {}
    snprintf(path, sizeof path, "%s/oa", base);
    CHECK(strcmp(slurp(path, out, sizeof out), "subset") == 0, "multicast: a listed member (A) receives");
    snprintf(path, sizeof path, "%s/oc", base);
    CHECK(strcmp(slurp(path, out, sizeof out), "subset") == 0, "multicast: a listed member (C) receives");
    snprintf(cmd, sizeof cmd, "./iac recv %s B 1 >/dev/null 2>&1; printf %%d $? >%s/r", room, base); if (system(cmd)) {}
    snprintf(path, sizeof path, "%s/r", base);
    CHECK(strcmp(slurp(path, out, sizeof out), "1") == 0, "multicast: an unlisted member (B) gets nothing");

    /* 4. the sender does not receive its own broadcast */
    snprintf(room, sizeof room, "%s/r4", base);
    snprintf(cmd, sizeof cmd, "IAC_FROM=A ./iac send %s '*' -- mine", room); if (system(cmd)) {}
    snprintf(cmd, sizeof cmd, "./iac recv %s A 1 >/dev/null 2>&1; printf %%d $? >%s/r", room, base); if (system(cmd)) {}
    snprintf(path, sizeof path, "%s/r", base);
    CHECK(strcmp(slurp(path, out, sizeof out), "1") == 0, "no self-delivery: sender skips its own message");

    /* 5. cursor advances: two messages arrive in order */
    snprintf(room, sizeof room, "%s/r5", base);
    snprintf(cmd, sizeof cmd, "IAC_FROM=X ./iac send %s A -- one; IAC_FROM=X ./iac send %s A -- two", room, room); if (system(cmd)) {}
    snprintf(cmd, sizeof cmd, "./iac recv %s A 3 >%s/o1 2>/dev/null; ./iac recv %s A 3 >%s/o2 2>/dev/null", room, base, room, base); if (system(cmd)) {}
    snprintf(path, sizeof path, "%s/o1", base); slurp(path, out, sizeof out);
    CHECK(strcmp(out, "one") == 0, "order: first recv returns the first message");
    snprintf(path, sizeof path, "%s/o2", base); slurp(path, out, sizeof out);
    CHECK(strcmp(out, "two") == 0, "order: second recv returns the second, cursor advanced");

    /* 6. a blocking recv wakes on a message that arrives mid-wait */
    snprintf(room, sizeof room, "%s/r6", base);
    snprintf(cmd, sizeof cmd,
             "( sleep 1; IAC_FROM=X ./iac send %s A -- late ) & ./iac recv %s A 5 >%s/o 2>/dev/null; wait",
             room, room, base); if (system(cmd)) {}
    snprintf(path, sizeof path, "%s/o", base);
    CHECK(strcmp(slurp(path, out, sizeof out), "late") == 0, "long-poll: recv wakes on a mid-wait message");

    /* 7. multi-line body round-trips exactly */
    snprintf(room, sizeof room, "%s/r7", base);
    snprintf(cmd, sizeof cmd, "printf 'l1\\nl2\\n' | IAC_FROM=X ./iac send %s A", room); if (system(cmd)) {}
    snprintf(cmd, sizeof cmd, "./iac recv %s A 3 >%s/o 2>/dev/null", room, base); if (system(cmd)) {}
    snprintf(path, sizeof path, "%s/o", base);
    CHECK(strcmp(slurp(path, out, sizeof out), "l1\nl2\n") == 0, "framing: a multi-line body round-trips exactly");

    /* 8. join starts at the log's end: no backlog, but future messages arrive */
    snprintf(room, sizeof room, "%s/r8", base);
    snprintf(cmd, sizeof cmd, "IAC_FROM=X ./iac send %s '*' -- old; ./iac join %s C", room, room); if (system(cmd)) {}
    snprintf(cmd, sizeof cmd, "./iac recv %s C 1 >/dev/null 2>&1; printf %%d $? >%s/r", room, base); if (system(cmd)) {}
    snprintf(path, sizeof path, "%s/r", base);
    CHECK(strcmp(slurp(path, out, sizeof out), "1") == 0, "join: a joiner skips the backlog");
    snprintf(cmd, sizeof cmd, "IAC_FROM=X ./iac send %s '*' -- fresh", room); if (system(cmd)) {}
    snprintf(cmd, sizeof cmd, "./iac recv %s C 3 >%s/o 2>/dev/null", room, base); if (system(cmd)) {}
    snprintf(path, sizeof path, "%s/o", base);
    CHECK(strcmp(slurp(path, out, sizeof out), "fresh") == 0, "join: a joiner receives messages sent after it joined");

    /* 9. who lists current members */
    snprintf(room, sizeof room, "%s/r9", base);
    snprintf(cmd, sizeof cmd, "./iac join %s alice; ./iac join %s bob; ./iac who %s >%s/o 2>/dev/null", room, room, room, base); if (system(cmd)) {}
    snprintf(path, sizeof path, "%s/o", base); slurp(path, out, sizeof out);
    CHECK(strstr(out, "alice") && strstr(out, "bob"), "who: the roster lists joined members");

    /* 10. "whoever is free": a to="?" task is claimed by exactly one worker */
    snprintf(room, sizeof room, "%s/r_q", base);
    snprintf(cmd, sizeof cmd, "IAC_FROM=hub ./iac send %s '?' -- claimjob", room); if (system(cmd)) {}
    snprintf(cmd, sizeof cmd, "./iac recv %s w1 3 >%s/o 2>/dev/null", room, base); if (system(cmd)) {}
    snprintf(path, sizeof path, "%s/o", base);
    CHECK(strcmp(slurp(path, out, sizeof out), "claimjob") == 0, "claim: the first free worker gets the ? task");
    snprintf(cmd, sizeof cmd, "./iac recv %s w2 1 >/dev/null 2>&1; printf %%d $? >%s/r", room, base); if (system(cmd)) {}
    snprintf(path, sizeof path, "%s/r", base);
    CHECK(strcmp(slurp(path, out, sizeof out), "1") == 0, "claim: a second worker gets nothing (already claimed)");

    /* 11. presence: a held beacon reads online + maps name->pid; it self-clears
     *     to offline when the process dies (flock released by the OS) */
    snprintf(room, sizeof room, "%s/r10", base);
    snprintf(cmd, sizeof cmd,
             "./iac hold %s nick & echo $! >%s/hp; sleep 0.4; "
             "./iac who %s >%s/w1 2>/dev/null; "
             "kill $(cat %s/hp) 2>/dev/null; sleep 0.4; "
             "./iac who %s >%s/w2 2>/dev/null", room, base, room, base, base, room, base);
    if (system(cmd)) {}
    snprintf(path, sizeof path, "%s/w1", base); slurp(path, out, sizeof out);
    CHECK(strstr(out, "nick") && strstr(out, "online") && strstr(out, "pid "),
          "presence: a held beacon shows online and maps name -> pid");
    snprintf(path, sizeof path, "%s/w2", base); slurp(path, out, sizeof out);
    CHECK(strstr(out, "offline") != NULL,
          "presence: it self-clears to offline when the beacon process dies");

    /* 12. crash-recovery: an unacked "?" claim is protected within TTL, re-claimable past it */
    snprintf(room, sizeof room, "%s/r_rec", base);
    snprintf(cmd, sizeof cmd, "IAC_FROM=hub ./iac send %s '?' -- recjob", room); if (system(cmd)) {}
    snprintf(cmd, sizeof cmd, "./iac recv %s w1 3 >%s/o1 2>/dev/null", room, base); if (system(cmd)) {}
    snprintf(path, sizeof path, "%s/o1", base);
    CHECK(strcmp(slurp(path, out, sizeof out), "recjob") == 0, "recovery: first worker claims the ? task");
    /* within TTL (default 300s), a second worker must NOT steal the live claim */
    snprintf(cmd, sizeof cmd, "./iac recv %s w2 1 >/dev/null 2>&1; printf %%d $? >%s/r", room, base); if (system(cmd)) {}
    snprintf(path, sizeof path, "%s/r", base);
    CHECK(strcmp(slurp(path, out, sizeof out), "1") == 0, "recovery: an active claim is not stolen within TTL");
    /* w1 never acked (simulated crash); past a short TTL, w2 re-claims and reruns it */
    snprintf(cmd, sizeof cmd, "sleep 2; IAC_CLAIM_TTL=1 ./iac recv %s w2 3 >%s/o2 2>/dev/null", room, base); if (system(cmd)) {}
    snprintf(path, sizeof path, "%s/o2", base);
    CHECK(strcmp(slurp(path, out, sizeof out), "recjob") == 0, "recovery: an unacked claim past TTL is re-claimed");

    /* 13. ack: a completed "?" task is never re-run, even past its TTL */
    snprintf(room, sizeof room, "%s/r_ack", base);
    snprintf(cmd, sizeof cmd, "IAC_FROM=hub ./iac send %s '?' -- ackjob", room); if (system(cmd)) {}
    /* w1 claims, reads the claim id off recv's stderr, and acks it */
    snprintf(cmd, sizeof cmd,
             "./iac recv %s w1 3 >%s/o 2>%s/e; "
             "id=$(sed -n 's/.*claim //p' %s/e | tr -d ' \\n'); "
             "grep -q 'claim ' %s/e && printf yes >%s/g; "
             "./iac ack %s w1 \"$id\"",
             room, base, base, base, base, base, room); if (system(cmd)) {}
    snprintf(path, sizeof path, "%s/o", base);
    CHECK(strcmp(slurp(path, out, sizeof out), "ackjob") == 0, "ack: worker receives the ? task");
    snprintf(path, sizeof path, "%s/g", base);
    CHECK(strcmp(slurp(path, out, sizeof out), "yes") == 0, "ack: recv prints the claim id on stderr");
    /* past the TTL, a done-marked task stays done: nobody re-runs it */
    snprintf(cmd, sizeof cmd, "sleep 2; IAC_CLAIM_TTL=1 ./iac recv %s w3 1 >/dev/null 2>&1; printf %%d $? >%s/r", room, base); if (system(cmd)) {}
    snprintf(path, sizeof path, "%s/r", base);
    CHECK(strcmp(slurp(path, out, sizeof out), "1") == 0, "ack: an acked task is not re-claimed past TTL");

    /* 14. fork-storm: N workers race one "?" task -> exactly ONE claims it */
    snprintf(room, sizeof room, "%s/r_race", base);
    snprintf(cmd, sizeof cmd, "IAC_FROM=hub ./iac send %s '?' -- racejob", room); if (system(cmd)) {}
    snprintf(cmd, sizeof cmd,
             "for i in $(seq 20); do ./iac recv %s w$i 3 >%s/rc.$i 2>/dev/null & done; wait; "
             "n=0; for i in $(seq 20); do [ -s %s/rc.$i ] && n=$((n+1)); done; printf %%d $n >%s/n",
             room, base, base, base); if (system(cmd)) {}
    snprintf(path, sizeof path, "%s/n", base);
    CHECK(strcmp(slurp(path, out, sizeof out), "1") == 0, "fork-storm: exactly one of 20 workers wins the ? task");

    /* 15. fork-storm: N senders append at once -> all frames land intact and distinct */
    snprintf(room, sizeof room, "%s/r_storm", base);
    snprintf(cmd, sizeof cmd, "./iac join %s rdr", room); if (system(cmd)) {}   /* start at end */
    snprintf(cmd, sizeof cmd,
             "for i in $(seq -w 1 16); do IAC_FROM=s$i ./iac send %s rdr -- storm$i & done; wait",
             room); if (system(cmd)) {}
    snprintf(cmd, sizeof cmd,
             "rm -f %s/drain; for i in $(seq 16); do ./iac recv %s rdr 3 >>%s/drain 2>/dev/null && echo >>%s/drain; done",
             base, room, base, base); if (system(cmd)) {}
    snprintf(cmd, sizeof cmd,
             "printf '%%s %%s' \"$(grep -c '^storm' %s/drain)\" \"$(grep '^storm' %s/drain | sort -u | wc -l)\" >%s/cnt",
             base, base, base); if (system(cmd)) {}
    snprintf(path, sizeof path, "%s/cnt", base);
    CHECK(strcmp(slurp(path, out, sizeof out), "16 16") == 0, "fork-storm: 16 concurrent senders append 16 intact, distinct frames");

    /* 16. presence-in-recv: parked recv shows online (no beacon), then "seen Ns ago" after */
    snprintf(room, sizeof room, "%s/r_pir", base);
    snprintf(cmd, sizeof cmd,
             "( ./iac recv %s parker 5 >/dev/null 2>&1 ) & rp=$!; sleep 1; "
             "./iac who %s >%s/w1 2>/dev/null; "
             "IAC_FROM=hub ./iac send %s parker -- wake; wait $rp; "
             "./iac who %s >%s/w2 2>/dev/null",
             room, room, base, room, room, base); if (system(cmd)) {}
    snprintf(path, sizeof path, "%s/w1", base); slurp(path, out, sizeof out);
    CHECK(strstr(out, "parker") && strstr(out, "online"),
          "presence-in-recv: a parked recv shows online without a beacon");
    snprintf(path, sizeof path, "%s/w2", base); slurp(path, out, sizeof out);
    CHECK(strstr(out, "parker") && strstr(out, "offline") && strstr(out, "seen "),
          "presence-in-recv: who reports last-seen once recv returns");

    /* 17. a hold beacon and a recv on the SAME name coexist (shared lock), not deadlock */
    snprintf(room, sizeof room, "%s/r_coex", base);
    snprintf(cmd, sizeof cmd,
             "./iac hold %s dual & hp=$!; sleep 1; "
             "IAC_FROM=hub ./iac send %s dual -- ping; "
             "timeout 4 ./iac recv %s dual 3 >%s/o 2>/dev/null; printf %%d $? >%s/rc; "
             "kill $hp 2>/dev/null",
             room, room, room, base, base); if (system(cmd)) {}
    snprintf(path, sizeof path, "%s/o", base); slurp(path, out, sizeof out);
    CHECK(strcmp(out, "ping") == 0, "coexist: recv delivers while the agent's own hold beacon is up");
    snprintf(path, sizeof path, "%s/rc", base);
    CHECK(strcmp(slurp(path, out, sizeof out), "0") == 0, "coexist: recv does not deadlock against its own beacon");

    /* 18. ask: send a question and block for the reply in one process (bob echoes it back) */
    snprintf(room, sizeof room, "%s/r_ask", base);
    snprintf(cmd, sizeof cmd,
             "( q=$(./iac recv %s bob 3 2>/dev/null); IAC_FROM=bob ./iac send %s alice -- \"reply:$q\" ) & "
             "sleep 1; IAC_ASK_TIMEOUT=3 IAC_FROM=alice ./iac ask %s bob -- question >%s/o 2>/dev/null; wait",
             room, room, room, base); if (system(cmd)) {}
    snprintf(path, sizeof path, "%s/o", base);
    CHECK(strcmp(slurp(path, out, sizeof out), "reply:question") == 0, "ask: send-and-await-reply returns the peer's answer");

    /* 19. log -n K: orientation shows only the last K frames, not the whole log */
    snprintf(room, sizeof room, "%s/r_tail", base);
    snprintf(cmd, sizeof cmd,
             "for m in one two three four; do IAC_FROM=x ./iac send %s '*' -- $m; done; "
             "./iac log %s -n 2 >%s/o 2>/dev/null", room, room, base); if (system(cmd)) {}
    snprintf(path, sizeof path, "%s/o", base); slurp(path, out, sizeof out);
    CHECK(strstr(out, "three") && strstr(out, "four") && !strstr(out, "one") && !strstr(out, "two"),
          "log -n K: prints only the last K frames");

    /* 20. recv --follow: stream successive messages for me, skip "?" work, return after idle */
    snprintf(room, sizeof room, "%s/r_follow", base);
    snprintf(cmd, sizeof cmd,
             "( sleep 1; IAC_FROM=x ./iac send %s watch -- alpha; "
             "IAC_FROM=y ./iac send %s '?' -- work; "
             "IAC_FROM=z ./iac send %s watch -- beta ) & "
             "./iac recv %s watch 3 --follow >%s/o 2>/dev/null; wait",
             room, room, room, room, base); if (system(cmd)) {}
    snprintf(path, sizeof path, "%s/o", base); slurp(path, out, sizeof out);
    CHECK(strstr(out, "alpha") && strstr(out, "beta") && !strstr(out, "work"),
          "recv --follow: streams successive messages and skips ? work");

    /* 21. compact: drop frames every reader has passed, shift cursors so recv resumes right */
    snprintf(room, sizeof room, "%s/r_compact", base);
    snprintf(cmd, sizeof cmd,
             "for m in one two three four; do IAC_FROM=x ./iac send %s a -- $m; done; "
             "./iac recv %s a 2 >/dev/null 2>&1; ./iac recv %s a 2 >/dev/null 2>&1; "  /* a reads one,two */
             "./iac compact %s >/dev/null 2>&1; "
             "./iac log %s >%s/lg 2>/dev/null; ./iac recv %s a 2 >%s/nx 2>/dev/null",
             room, room, room, room, room, base, room, base); if (system(cmd)) {}
    snprintf(path, sizeof path, "%s/lg", base); slurp(path, out, sizeof out);
    CHECK(strstr(out, "three") && strstr(out, "four") && !strstr(out, "one") && !strstr(out, "two"),
          "compact: drops frames every reader has already passed");
    snprintf(path, sizeof path, "%s/nx", base); slurp(path, out, sizeof out);
    CHECK(strcmp(out, "three") == 0, "compact: cursors shift so recv resumes at the right frame");

    /* 22. drain: one non-blocking call returns the whole backlog for me, in order */
    snprintf(room, sizeof room, "%s/r_drain", base);
    snprintf(cmd, sizeof cmd,
             "IAC_FROM=x ./iac send %s a -- one; IAC_FROM=y ./iac send %s a -- two; "
             "IAC_FROM=z ./iac send %s b -- other; IAC_FROM=w ./iac send %s a -- three; "
             "./iac drain %s a >%s/o 2>/dev/null; printf %%d $? >%s/rc",
             room, room, room, room, room, base, base); if (system(cmd)) {}
    snprintf(path, sizeof path, "%s/o", base); slurp(path, out, sizeof out);
    CHECK(strcmp(out, "one\ntwo\nthree\n") == 0, "drain: returns my whole backlog in order (skips others)");
    snprintf(path, sizeof path, "%s/rc", base);
    CHECK(strcmp(slurp(path, out, sizeof out), "0") == 0, "drain: exit 0 when it delivered messages");
    snprintf(cmd, sizeof cmd, "./iac drain %s a >/dev/null 2>&1; printf %%d $? >%s/r", room, base); if (system(cmd)) {}
    snprintf(path, sizeof path, "%s/r", base);
    CHECK(strcmp(slurp(path, out, sizeof out), "1") == 0, "drain: exit 1 on an empty box");

    /* 22. who reports a LIVE holder's pid, not a stale stored one: join writes a
     *     pid that dies, then a recv-only park must refresh it (kill -0 the shown pid) */
    snprintf(room, sizeof room, "%s/r_pidlive", base);
    snprintf(cmd, sizeof cmd,
             "./iac join %s x >/dev/null 2>&1; "                 /* roster pid = the join proc, which exits */
             "./iac recv %s x 3 >/dev/null 2>&1 & sleep 1; "     /* a recv-only park holds presence + refreshes pid */
             "p=$(./iac who %s | awk '/^x /{print $4}'); "
             "kill -0 \"$p\" 2>/dev/null && printf alive >%s/o || printf dead >%s/o; wait",
             room, room, room, base, base); if (system(cmd)) {}
    snprintf(path, sizeof path, "%s/o", base);
    CHECK(strcmp(slurp(path, out, sizeof out), "alive") == 0, "who: an online member's pid is the live flock holder, not stale");

    snprintf(cmd, sizeof cmd, "rm -rf %s", base); if (system(cmd)) {}
    printf("%s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
