
#include <stdlib.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/debug.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <inttypes.h>
#include <unistd.h>
#include <libcmdutils.h>

#include "libcbuf_impl.h"
#include "libcbuf.h"

int
cbuf_safe_add(size_t *res, size_t a, size_t b)
{
	if (a > SIZE_MAX - b) {
		errno = EOVERFLOW;
		return (-1);
	}

	*res = a + b;
	return (0);
}

int
cbuf_alloc(cbuf_t **cbufp, size_t capacity)
{
	cbuf_t *cbuf;

	*cbufp = NULL;

	if ((cbuf = calloc(1, sizeof (*cbuf))) == NULL) {
		return (-1);
	}
	cbuf->cbuf_capacity = capacity;
	cbuf->cbuf_limit = cbuf->cbuf_capacity;
	cbuf->cbuf_position = 0;
	cbuf->cbuf_order = CBUF_ORDER_BIG_ENDIAN;

	if ((cbuf->cbuf_data = malloc(cbuf->cbuf_capacity)) == NULL) {
		free(cbuf);
		return (-1);
	}

	*cbufp = cbuf;
	return (0);
}

void
cbuf_free(cbuf_t *cbuf)
{
	if (cbuf == NULL) {
		return;
	}

	VERIFY(!list_link_active(&cbuf->cbuf_link));

	free(cbuf->cbuf_data);
	free(cbuf);
}

int
cbuf_extend(cbuf_t *cbuf, size_t new_capacity)
{
	void *new_data;

	if (new_capacity <= cbuf->cbuf_capacity) {
		return (0);
	}

	if ((new_data = realloc(cbuf->cbuf_data, new_capacity)) == NULL) {
		return (-1);
	}

	cbuf->cbuf_data = new_data;
	cbuf->cbuf_capacity = new_capacity;

	return (0);
}

int
cbuf_shrink(cbuf_t *cbuf)
{
	void *new_data;

	if ((new_data = realloc(cbuf->cbuf_data, cbuf->cbuf_limit)) == NULL) {
		return (-1);
	}

	cbuf->cbuf_data = new_data;
	cbuf->cbuf_capacity = cbuf->cbuf_limit;

	return (0);
}

unsigned int
cbuf_byteorder(cbuf_t *cbuf)
{
	return (cbuf->cbuf_order);
}

void
cbuf_byteorder_set(cbuf_t *cbuf, unsigned int order)
{
	switch (order) {
	case CBUF_ORDER_BIG_ENDIAN:
	case CBUF_ORDER_LITTLE_ENDIAN:
		cbuf->cbuf_order = order;
		break;

	default:
		abort();
		break;
	}
}

/*
 * Use read(2) to append data to a buffer.
 */
int
cbuf_sys_read(cbuf_t *cbuf, int fd, size_t want, size_t *actual)
{
	ssize_t rsz;
	size_t pos = cbuf_position(cbuf);

	if (want == CBUF_SYSREAD_ENTIRE) {
		if ((want = cbuf_available(cbuf)) == 0) {
			errno = ENOSPC;
			return (-1);
		}
	} else if (want == 0) {
		errno = EINVAL;
		return (-1);
	} else if (want > cbuf_available(cbuf)) {
		errno = ENOSPC;
		return (-1);
	}

	if ((rsz = read(fd, &cbuf->cbuf_data[pos], want)) < 0) {
		return (-1);
	}
	VERIFY0(cbuf_position_set(cbuf, pos + rsz));

	if (actual != NULL) {
		*actual = (size_t)rsz;
	}
	return (0);
}

/*
 * Use write(2) to consume data from the buffer.
 */
int
cbuf_sys_write(cbuf_t *cbuf, int fd, size_t want, size_t *actual)
{
	ssize_t wsz;
	size_t pos = cbuf_position(cbuf);

	if (want == CBUF_SYSREAD_ENTIRE) {
		if ((want = cbuf_available(cbuf)) == 0) {
			errno = ENOSPC;
			return (-1);
		}
	} else if (want == 0) {
		errno = EINVAL;
		return (-1);
	} else if (want > cbuf_available(cbuf)) {
		errno = ENOSPC;
		return (-1);
	}

	if ((wsz = write(fd, &cbuf->cbuf_data[pos], want)) < 0) {
		return (-1);
	}
	VERIFY0(cbuf_position_set(cbuf, pos + wsz));

	if (actual != NULL) {
		*actual = (size_t)wsz;
	}
	return (0);
}

size_t
cbuf_capacity(cbuf_t *cbuf)
{
	return (cbuf->cbuf_capacity);
}

size_t
cbuf_position(cbuf_t *cbuf)
{
	return (cbuf->cbuf_position);
}

int
cbuf_position_set(cbuf_t *cbuf, size_t sz)
{
	if (sz > cbuf->cbuf_limit) {
		errno = EOVERFLOW;
		return (-1);
	}

	cbuf->cbuf_position = sz;
	return (0);
}

size_t
cbuf_limit(cbuf_t *cbuf)
{
	return (cbuf->cbuf_limit);
}

int
cbuf_limit_set(cbuf_t *cbuf, size_t sz)
{
	if (sz > cbuf->cbuf_capacity) {
		errno = EOVERFLOW;
		return (-1);
	}

	cbuf->cbuf_limit = sz;
	if (cbuf->cbuf_limit > cbuf->cbuf_position) {
		cbuf->cbuf_position = cbuf->cbuf_limit;
	}

	return (0);
}

size_t
cbuf_available(cbuf_t *cbuf)
{
	return (cbuf->cbuf_limit - cbuf->cbuf_position);
}

size_t
cbuf_unused(cbuf_t *cbuf)
{
	return (cbuf->cbuf_capacity - cbuf->cbuf_limit);
}

void
cbuf_flip(cbuf_t *cbuf)
{
	cbuf->cbuf_limit = cbuf->cbuf_position;
	cbuf->cbuf_position = 0;
}

void
cbuf_clear(cbuf_t *cbuf)
{
	cbuf->cbuf_limit = cbuf->cbuf_capacity;
	cbuf->cbuf_position = 0;
}

void
cbuf_resume(cbuf_t *cbuf)
{
	cbuf->cbuf_position = cbuf->cbuf_limit;
	cbuf->cbuf_limit = cbuf->cbuf_capacity;
}

void
cbuf_rewind(cbuf_t *cbuf)
{
	cbuf->cbuf_position = 0;
}

void
cbuf_compact(cbuf_t *cbuf)
{
	size_t start = cbuf_position(cbuf);
	size_t copysz = cbuf_available(cbuf);

	if (cbuf->cbuf_position == 0 || copysz == 0) {
		return;
	}

	memmove(&cbuf->cbuf_data[0], &cbuf->cbuf_data[start], copysz);
	cbuf->cbuf_position = 0;
	VERIFY3U(cbuf->cbuf_limit, >=, start);
	cbuf->cbuf_limit -= start;
}

/*
 * Copy bytes from "from", starting at "position" and advancing until "limit"
 * is reached.  The bytes will be copied into "to", starting at "position", and
 * stopping before "limit".
 */
size_t
cbuf_copy(cbuf_t *cbuf_from, cbuf_t *cbuf_to)
{
	size_t srcsz = cbuf_available(cbuf_from);
	size_t dstsz = cbuf_available(cbuf_to);
	size_t copysz;

	/*
	 * Copy only as many bytes as will fit in the destination buffer.
	 */
	copysz = (srcsz <= dstsz) ? srcsz : dstsz;
	if (copysz < 1) {
		return (0);
	}

	memcpy(&cbuf_to->cbuf_data[cbuf_to->cbuf_position],
	    &cbuf_from->cbuf_data[cbuf_from->cbuf_position],
	    copysz);

	cbuf_to->cbuf_position += copysz;
	cbuf_from->cbuf_position += copysz;

	return (copysz);
}

#define	CBUF_APPEND_COMMON(cbuf, val)					\
	do {								\
		if (cbuf_available(cbuf) < sizeof (val)) {		\
			errno = ENOSPC;					\
			return (-1);					\
		}							\
									\
		memcpy(&cbuf->cbuf_data[cbuf->cbuf_position], &val,	\
		    sizeof (val));					\
									\
		cbuf->cbuf_position += sizeof (val);			\
		VERIFY(cbuf->cbuf_position <= cbuf->cbuf_limit);	\
									\
		return (0);						\
	} while (0)

int
cbuf_put_u8(cbuf_t *cbuf, uint8_t val)
{
	CBUF_APPEND_COMMON(cbuf, val);
}

int
cbuf_put_u16(cbuf_t *cbuf, uint16_t val)
{
	val = (cbuf->cbuf_order == CBUF_ORDER_BIG_ENDIAN) ? htons(val) : val;

	CBUF_APPEND_COMMON(cbuf, val);
}

int
cbuf_put_u32(cbuf_t *cbuf, uint32_t val)
{
	val = (cbuf->cbuf_order == CBUF_ORDER_BIG_ENDIAN) ? htonl(val) : val;

	CBUF_APPEND_COMMON(cbuf, val);
}

int
cbuf_put_u64(cbuf_t *cbuf, uint64_t val)
{
	val = (cbuf->cbuf_order == CBUF_ORDER_BIG_ENDIAN) ? htonll(val) : val;

	CBUF_APPEND_COMMON(cbuf, val);
}

int
cbuf_get_char(cbuf_t *cbuf, char *val)
{
	if (cbuf_available(cbuf) < 1) {
		errno = ENOSPC;
		return (-1);
	}

	*val = cbuf->cbuf_data[cbuf->cbuf_position];
	cbuf->cbuf_position++;
	VERIFY3U(cbuf->cbuf_position, <=, cbuf->cbuf_limit);

	return (0);
}

int
cbuf_put_string(cbuf_t *cbuf, custr_t *cu)
{
	if (cbuf_available(cbuf) < custr_len(cu)) {
		errno = ENOSPC;
		return (-1);
	}

	memcpy(&cbuf->cbuf_data[cbuf->cbuf_position], custr_cstr(cu),
	    custr_len(cu));
	VERIFY0(cbuf_safe_add(&cbuf->cbuf_position, cbuf->cbuf_position,
	    custr_len(cu)));

	return (0);
}

#if 0
void *
cbuf_read_ptr(cbuf_t *cbuf, size_t sz)
{
	size_t limit;

	VERIFY(sz > 0);

	if (cbuf_safe_add(&limit, cbuf_pos(cbuf, CBUF_POS_READ), sz) != 0) {
		return (NULL);
	}

	if (limit < cbuf_pos(cbuf, CBUF_POS_END)) {
	}

	if (cbuf->cbuf_len < sz) {
		errno = EIO;
		return (NULL);
	}
}
#endif

void
cbuf_dump(cbuf_t *cbuf, FILE *fp)
{
	unsigned per_row = 8;

	fprintf(fp, "cbuf[%p]: pos %8u lim %8u cap %8u\n",
	    cbuf, cbuf->cbuf_position, cbuf->cbuf_limit, cbuf->cbuf_capacity);

	for (size_t i = 0; i < cbuf->cbuf_limit; i += per_row) {
		fprintf(fp, "    %04x: ", i);
		for (unsigned x = 0; x < per_row; x++) {
			if (i + x >= cbuf->cbuf_limit) {
				break;
			}

			fprintf(fp, " %02x", cbuf->cbuf_data[i + x]);
		}
		fprintf(fp, "\n");
	}
	fprintf(fp, "\n");
}
