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
 * Copyright (c) 2014, Joyent, Inc.
 */

#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <wchar.h>
#include <sys/debug.h>
#include <stdarg.h>
#include <assert.h>

#include "libnvpair.h"
#include "libcmdutils.h"

#define	JSON_MARKER		".__json_"
#define	JSON_MARKER_ARRAY	JSON_MARKER "array"

#define	FPRINTF(cu, ...)						\
	do {								\
		if (custr_append_printf(cu, __VA_ARGS__) != 0)		\
			return (-1);					\
	} while (0)

static int cmon_nvlist_do_json(nvlist_t *nvl, custr_t *out);

/*
 * When formatting a string for JSON output we must escape certain characters,
 * as described in RFC4627.  This applies to both member names and
 * DATA_TYPE_STRING values.
 *
 * This function will only operate correctly if the following conditions are
 * met:
 *
 *       1. The input String is encoded in the current locale.
 *
 *       2. The current locale includes the Basic Multilingual Plane (plane 0)
 *          as defined in the Unicode standard.
 *
 * The output will be entirely 7-bit ASCII (as a subset of UTF-8) with all
 * representable Unicode characters included in their escaped numeric form.
 */
static int
cmon_nvlist_print_json_string(const char *input, custr_t *out)
{
	mbstate_t mbr;
	wchar_t c;
	size_t sz;

	bzero(&mbr, sizeof (mbr));

	FPRINTF(out, "\"");
	while ((sz = mbrtowc(&c, input, MB_CUR_MAX, &mbr)) > 0) {
		switch (c) {
		case '"':
			FPRINTF(out, "\\\"");
			break;
		case '\n':
			FPRINTF(out, "\\n");
			break;
		case '\r':
			FPRINTF(out, "\\r");
			break;
		case '\\':
			FPRINTF(out, "\\\\");
			break;
		case '\f':
			FPRINTF(out, "\\f");
			break;
		case '\t':
			FPRINTF(out, "\\t");
			break;
		case '\b':
			FPRINTF(out, "\\b");
			break;
		default:
			if ((c >= 0x00 && c <= 0x1f) ||
			    (c > 0x7f && c <= 0xffff)) {
				/*
				 * Render both Control Characters and Unicode
				 * characters in the Basic Multilingual Plane
				 * as JSON-escaped multibyte characters.
				 */
				FPRINTF(out, "\\u%04x", (int)(0xffff & c));
			} else if (c >= 0x20 && c <= 0x7f) {
				/*
				 * Render other 7-bit ASCII characters directly
				 * and drop other, unrepresentable characters.
				 */
				FPRINTF(out, "%c", (int)(0xff & c));
			}
			break;
		}
		input += sz;
	}

	if (sz == (size_t)-1 || sz == (size_t)-2) {
		/*
		 * We last read an invalid multibyte character sequence,
		 * so return an error.
		 */
		return (-1);
	}

	FPRINTF(out, "\"");
	return (0);
}

static int
cmon_nvlist_do_json_value(nvpair_t *curr, custr_t *out)
{
	data_type_t type = nvpair_type(curr);

	switch (type) {
	case DATA_TYPE_STRING: {
		char *string = fnvpair_value_string(curr);
		if (cmon_nvlist_print_json_string(string, out)== -1) {
			return (-1);
		}
		break;
	}

	case DATA_TYPE_BOOLEAN: {
		FPRINTF(out, "true");
		break;
	}

	case DATA_TYPE_BOOLEAN_VALUE: {
		FPRINTF(out, "%s",
		    fnvpair_value_boolean_value(curr) == B_TRUE ?
		    "true" : "false");
		break;
	}

	case DATA_TYPE_BYTE: {
		FPRINTF(out, "%hhu", fnvpair_value_byte(curr));
		break;
	}

	case DATA_TYPE_INT8: {
		FPRINTF(out, "%hhd", fnvpair_value_int8(curr));
		break;
	}

	case DATA_TYPE_UINT8: {
		FPRINTF(out, "%hhu", fnvpair_value_uint8_t(curr));
		break;
	}

	case DATA_TYPE_INT16: {
		FPRINTF(out, "%hd", fnvpair_value_int16(curr));
		break;
	}

	case DATA_TYPE_UINT16: {
		FPRINTF(out, "%hu", fnvpair_value_uint16(curr));
		break;
	}

	case DATA_TYPE_INT32: {
		FPRINTF(out, "%d", fnvpair_value_int32(curr));
		break;
	}

	case DATA_TYPE_UINT32: {
		FPRINTF(out, "%u", fnvpair_value_uint32(curr));
		break;
	}

	case DATA_TYPE_INT64: {
		FPRINTF(out, "%lld",
		    (long long)fnvpair_value_int64(curr));
		break;
	}

	case DATA_TYPE_UINT64: {
		FPRINTF(out, "%llu",
		    (unsigned long long)fnvpair_value_uint64(curr));
		break;
	}

	case DATA_TYPE_HRTIME: {
		hrtime_t val;
		VERIFY0(nvpair_value_hrtime(curr, &val));
		FPRINTF(out, "%llu", (unsigned long long)val);
		break;
	}

	case DATA_TYPE_DOUBLE: {
		double val;
		VERIFY0(nvpair_value_double(curr, &val));
		FPRINTF(out, "%f", val);
		break;
	}

	case DATA_TYPE_NVLIST: {
		if (cmon_nvlist_do_json(fnvpair_value_nvlist(curr), out) != 0) {
			return (-1);
		}
		break;
	}

	case DATA_TYPE_STRING_ARRAY: {
		char **val;
		uint_t valsz, i;
		VERIFY0(nvpair_value_string_array(curr, &val, &valsz));
		FPRINTF(out, "[");
		for (i = 0; i < valsz; i++) {
			if (i > 0)
				FPRINTF(out, ",");
			if (cmon_nvlist_print_json_string(val[i], out) != 0) {
				return (-1);
			}
		}
		FPRINTF(out, "]");
		break;
	}

	case DATA_TYPE_NVLIST_ARRAY: {
		nvlist_t **val;
		uint_t valsz, i;
		VERIFY0(nvpair_value_nvlist_array(curr, &val, &valsz));
		FPRINTF(out, "[");
		for (i = 0; i < valsz; i++) {
			if (i > 0)
				FPRINTF(out, ",");
			if (cmon_nvlist_do_json(val[i], out) != 0) {
				return (-1);
			}
		}
		FPRINTF(out, "]");
		break;
	}

	case DATA_TYPE_BOOLEAN_ARRAY: {
		boolean_t *val;
		uint_t valsz, i;
		VERIFY0(nvpair_value_boolean_array(curr, &val, &valsz));
		FPRINTF(out, "[");
		for (i = 0; i < valsz; i++) {
			if (i > 0)
				FPRINTF(out, ",");
			FPRINTF(out, val[i] == B_TRUE ?
			    "true" : "false");
		}
		FPRINTF(out, "]");
		break;
	}

	case DATA_TYPE_BYTE_ARRAY: {
		uchar_t *val;
		uint_t valsz, i;
		VERIFY0(nvpair_value_byte_array(curr, &val, &valsz));
		FPRINTF(out, "[");
		for (i = 0; i < valsz; i++) {
			if (i > 0)
				FPRINTF(out, ",");
			FPRINTF(out, "%hhu", val[i]);
		}
		FPRINTF(out, "]");
		break;
	}

	case DATA_TYPE_UINT8_ARRAY: {
		uint8_t *val;
		uint_t valsz, i;
		VERIFY0(nvpair_value_uint8_array(curr, &val, &valsz));
		FPRINTF(out, "[");
		for (i = 0; i < valsz; i++) {
			if (i > 0)
				FPRINTF(out, ",");
			FPRINTF(out, "%hhu", val[i]);
		}
		FPRINTF(out, "]");
		break;
	}

	case DATA_TYPE_INT8_ARRAY: {
		int8_t *val;
		uint_t valsz, i;
		VERIFY0(nvpair_value_int8_array(curr, &val, &valsz));
		FPRINTF(out, "[");
		for (i = 0; i < valsz; i++) {
			if (i > 0)
				FPRINTF(out, ",");
			FPRINTF(out, "%hd", val[i]);
		}
		FPRINTF(out, "]");
		break;
	}

	case DATA_TYPE_UINT16_ARRAY: {
		uint16_t *val;
		uint_t valsz, i;
		VERIFY0(nvpair_value_uint16_array(curr, &val, &valsz));
		FPRINTF(out, "[");
		for (i = 0; i < valsz; i++) {
			if (i > 0)
				FPRINTF(out, ",");
			FPRINTF(out, "%hu", val[i]);
		}
		FPRINTF(out, "]");
		break;
	}

	case DATA_TYPE_INT16_ARRAY: {
		int16_t *val;
		uint_t valsz, i;
		VERIFY0(nvpair_value_int16_array(curr, &val, &valsz));
		FPRINTF(out, "[");
		for (i = 0; i < valsz; i++) {
			if (i > 0)
				FPRINTF(out, ",");
			FPRINTF(out, "%hd", val[i]);
		}
		FPRINTF(out, "]");
		break;
	}

	case DATA_TYPE_UINT32_ARRAY: {
		uint32_t *val;
		uint_t valsz, i;
		VERIFY0(nvpair_value_uint32_array(curr, &val, &valsz));
		FPRINTF(out, "[");
		for (i = 0; i < valsz; i++) {
			if (i > 0)
				FPRINTF(out, ",");
			FPRINTF(out, "%u", val[i]);
		}
		FPRINTF(out, "]");
		break;
	}

	case DATA_TYPE_INT32_ARRAY: {
		int32_t *val;
		uint_t valsz, i;
		VERIFY0(nvpair_value_int32_array(curr, &val, &valsz));
		FPRINTF(out, "[");
		for (i = 0; i < valsz; i++) {
			if (i > 0)
				FPRINTF(out, ",");
			FPRINTF(out, "%d", val[i]);
		}
		FPRINTF(out, "]");
		break;
	}

	case DATA_TYPE_UINT64_ARRAY: {
		uint64_t *val;
		uint_t valsz, i;
		VERIFY0(nvpair_value_uint64_array(curr, &val, &valsz));
		FPRINTF(out, "[");
		for (i = 0; i < valsz; i++) {
			if (i > 0)
				FPRINTF(out, ",");
			FPRINTF(out, "%llu",
			    (unsigned long long)val[i]);
		}
		FPRINTF(out, "]");
		break;
	}

	case DATA_TYPE_INT64_ARRAY: {
		int64_t *val;
		uint_t valsz, i;
		VERIFY0(nvpair_value_int64_array(curr, &val, &valsz));
		FPRINTF(out, "[");
		for (i = 0; i < valsz; i++) {
			if (i > 0)
				FPRINTF(out, ",");
			FPRINTF(out, "%lld",
			    (long long)val[i]);
		}
		FPRINTF(out, "]");
		break;
	}

	case DATA_TYPE_UNKNOWN:
		return (-1);
	}

	return (0);
}

/*
 * The nvlist_t does not natively support arrays where each array element may
 * be a different type.  In order to allow this part of the JSON specification
 * to be represented, JSON arrays may be stored as a nested nvlist.
 *
 * The nvlist must have the JSON_MARKER_ARRAY boolean, as well as a uint32
 * "length".  Each element from 0 to (length - 1) must be represented as an
 * nvpair in this list, where the key name is the string version of the index
 * -- e.g.  for index 32, the key would be "32".
 */
static int
cmon_nvlist_do_json_array(nvlist_t *nvl, custr_t *out)
{
	uint32_t length, i;
	boolean_t first = B_TRUE;

	if (nvlist_lookup_uint32(nvl, "length", &length) != 0) {
		errno = EPROTO;
		return (-1);
	}

	FPRINTF(out, "[");

	for (i = 0; i < length; i++) {
		char buf[64];
		nvpair_t *curr;

		if (!first) {
			FPRINTF(out, ",");
		} else {
			first = B_FALSE;
		}

		(void) snprintf(buf, sizeof (buf), "%u", i);
		if (nvlist_lookup_nvpair(nvl, buf, &curr) != 0) {
			/*
			 * This array element appears to be missing.
			 */
			errno = EPROTO;
			return (-1);
		}

		if (cmon_nvlist_do_json_value(curr, out) != 0) {
			return (-1);
		}
	}

	FPRINTF(out, "]");
	return (0);
}


static int
cmon_nvlist_do_json(nvlist_t *nvl, custr_t *out)
{
	nvpair_t *curr;
	boolean_t first = B_TRUE;

	/*
	 * If this nvlist contains the special JSON array marker boolean,
	 * we will attempt to format it as an array in the JSON output.
	 */
	if (nvlist_lookup_boolean(nvl, JSON_MARKER_ARRAY) == 0) {
		return (cmon_nvlist_do_json_array(nvl, out));
	}

	FPRINTF(out, "{");

	for (curr = nvlist_next_nvpair(nvl, NULL); curr;
	    curr = nvlist_next_nvpair(nvl, curr)) {
		if (!first) {
			FPRINTF(out, ",");
		} else {
			first = B_FALSE;
		}

		if (cmon_nvlist_print_json_string(nvpair_name(curr),
		    out) != 0) {
			return (-1);
		}

		FPRINTF(out, ":");

		if (cmon_nvlist_do_json_value(curr, out) != 0) {
			return (-1);
		}
	}

	FPRINTF(out, "}");
	return (0);
}

int
cmon_nvlist_to_json(nvlist_t *nvl, custr_t *out)
{
	return (cmon_nvlist_do_json(nvl, out));
}
