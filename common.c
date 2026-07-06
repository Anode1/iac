/* iac -- see common.h.
 * Copyright (c) 2026 Vasili Gavrilov. ISC License; see LICENSE. */
#include "common.h"

#include <stdio.h>

char g_body[IAC_MSG_MAX];           /* the one message buffer (send + recv) */

int die(const char *msg) { fprintf(stderr, "iac: %s\n", msg); return 2; }
int die_path(void) { return die("path too long"); }
