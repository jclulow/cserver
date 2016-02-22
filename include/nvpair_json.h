/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 */

/*
 * Copyright 2016 Joyent, Inc.
 */

#ifndef _NVPAIR_JSON_H
#define _NVPAIR_JSON_H

#include <libnvpair.h>
#include "custr.h"

#ifdef __cplusplus
extern "C" {
#endif

extern int cmon_nvlist_to_json(nvlist_t *, custr_t *);

#ifdef __cplusplus
}
#endif

#endif	/* _NVPAIR_JSON_H */
