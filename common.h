/* iac -- primitives shared by every module: the fatal-exit helper and the one
 * message buffer (send fills it, recv drains into it -- never both in a process).
 * Copyright (c) 2026 Vasili Gavrilov. ISC License; see LICENSE. */
#ifndef IAC_COMMON_H
#define IAC_COMMON_H

#include "constants.h"

extern char g_body[IAC_MSG_MAX];    /* the one message buffer (send + recv), no heap */

int die(const char *msg);           /* print "iac: <msg>" to stderr, return 2 */
int die_path(void);                 /* the common one: die("path too long")   */

#endif /* IAC_COMMON_H */
