/* iac -- send: append one frame under an flock (single writev, so concurrent
 * senders never interleave -- one total order). ask: send, then block for the reply.
 * Copyright (c) 2026 Vasili Gavrilov. ISC License; see LICENSE. */
#ifndef IAC_SEND_H
#define IAC_SEND_H

int cmd_send(const char *room, const char *to, char **argv, int argi, int argc);
int cmd_ask(const char *room, const char *to, char **argv, int argi, int argc);

#endif /* IAC_SEND_H */
