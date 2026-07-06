/* iac -- the "?" work queue: a job is won by one atomic O_CREAT|O_EXCL claim
 * keyed on the log offset; an unacked claim past IAC_CLAIM_TTL is stolen and
 * re-delivered, so a crashed worker's task still runs. Crash-recoverable, no coordinator.
 * Copyright (c) 2026 Vasili Gavrilov. ISC License; see LICENSE. */
#ifndef IAC_CLAIM_H
#define IAC_CLAIM_H

int claim_fresh(const char *room, long id, const char *me);    /* win a fresh "?" at offset ID; 0 if a peer has it */
int recover_orphan(const char *room, const char *me);          /* steal+redeliver one expired claim; 1 if recovered */
int cmd_ack(const char *room, const char *me, long id);        /* mark a claimed "?" done */

#endif /* IAC_CLAIM_H */
