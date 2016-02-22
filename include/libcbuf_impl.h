#include "libcbuf.h"

#ifndef	_LIBCBUF_IMPL_H
#define	_LIBCBUF_IMPL_H

#include <sys/list.h>

struct cbuf {
	uint8_t *cbuf_data;
	size_t cbuf_capacity;

	size_t cbuf_limit;
	size_t cbuf_position;

	cbuf_order_t cbuf_order;

	list_node_t cbuf_link;		/* cbufq_t linkage */
};

struct cbufq {
	size_t cbufq_count;

	list_t cbufq_bufs;		/* queue of cbuf_t */
};

extern int cbuf_safe_add(size_t *, size_t, size_t);

#endif	/* !_LIBCBUF_IMPL_H */
