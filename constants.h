/* iac -- tunable sizes and limits, in one place.
 * Copyright (c) 2026 Vasili Gavrilov. ISC License; see LICENSE. */
#ifndef IAC_CONSTANTS_H
#define IAC_CONSTANTS_H

#define IAC_MSG_MAX (1 << 20)   /* 1 MiB per message; loud reject above  */
#define IAC_POLL_MS 100         /* max idle wait between scans (ms); an upper bound, not a spin */
#define IAC_NAME_MAX 64         /* max chars in one name; keeps it well inside the frame header */
#define IAC_SPEC_MAX 1024       /* max chars in a recipient spec (a comma list of names)        */

#endif /* IAC_CONSTANTS_H */
