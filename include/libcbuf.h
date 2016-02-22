#ifndef	_LIBCBUF_H
#define	_LIBCBUF_H

#include <stdlib.h>
#include <stdio.h>
#include "custr.h"

/*
 * 0 <= position <= limit <= capacity
 *
 * limit:      first element that should not be read/written
 * position:   the index of the next byte to be read/written
 */

typedef struct cbuf cbuf_t;
typedef struct cbufq cbufq_t;

/*
 * Create and free buffers.  At creation, the position is 0 and the limit is
 * the capacity, as if cbuf_clear() had been called.
 */
extern int cbuf_alloc(cbuf_t **cbufp, size_t capacity);
extern void cbuf_free(cbuf_t *cbuf);

extern int cbuf_extend(cbuf_t *cbuf, size_t new_capacity);
extern int cbuf_shrink(cbuf_t *cbuf);

/*
 * The number of bytes in the backing store for this buffer.
 */
extern size_t cbuf_capacity(cbuf_t *cbuf);

/*
 * Number of bytes from current position to limit.
 */
extern size_t cbuf_available(cbuf_t *cbuf);

/*
 * Number of unused bytes; i.e., number of bytes from current limit to
 * capacity.
 */
extern size_t cbuf_unused(cbuf_t *cbuf);

/*
 * Get or set the limit of the buffer.  The limit of the buffer is the first
 * byte of the buffer which should not be read or written to -- that is, one
 * index past the last byte.
 */
extern size_t cbuf_limit(cbuf_t *cbuf);
extern int cbuf_limit_set(cbuf_t *cbuf, size_t new_limit);

/*
 * Get or set the position of the buffer.  The position is the index of the
 * byte that will be the target of the next get or put operation.
 */
extern size_t cbuf_position(cbuf_t *cbuf);
extern int cbuf_position_set(cbuf_t *cbuf, size_t new_position);

/*
 * Make the buffer ready for puts; sets the limit to the capacity and the
 * position to 0.
 */
extern void cbuf_clear(cbuf_t *cbuf);

/*
 * Make the buffer ready for gets; sets the limit to the current position,
 * and the position to 0.
 */
extern void cbuf_flip(cbuf_t *cbuf);

/*
 * Rewind the position to the start of the buffer to allow gets to begin again
 * from the start.
 */
extern void cbuf_rewind(cbuf_t *cbuf);

/*
 * Make the buffer ready for puts, starting from current limit rather than
 * position 0; sets the limit to the capacity and the position to the previous
 * limit.
 */
extern void cbuf_resume(cbuf_t *cbuf);

/*
 * Bytes from current position to limit are copied to the beginning of the
 * buffer.  (i.e. pos moves to 0, pos+1 to 1, etc)
 */
extern void cbuf_compact(cbuf_t *cbuf);

typedef enum cbuf_order {
	CBUF_ORDER_BIG_ENDIAN = 1,
	CBUF_ORDER_LITTLE_ENDIAN
} cbuf_order_t;

extern void cbuf_byteorder_set(cbuf_t *cbuf, unsigned int order);
extern unsigned int cbuf_byteorder(cbuf_t *cbuf);

extern int cbuf_put_string(cbuf_t *cbuf, custr_t *cu);

extern int cbuf_get_char(cbuf_t *cbuf, char *val);
extern int cbuf_get_u8(cbuf_t *cbuf, uint8_t *val);
extern int cbuf_get_u16(cbuf_t *cbuf, uint16_t *val);
extern int cbuf_get_u32(cbuf_t *cbuf, uint32_t *val);
extern int cbuf_get_u64(cbuf_t *cbuf, uint64_t *val);

extern int cbuf_put_u8(cbuf_t *cbuf, uint8_t val);
extern int cbuf_put_u16(cbuf_t *cbuf, uint16_t val);
extern int cbuf_put_u32(cbuf_t *cbuf, uint32_t val);
extern int cbuf_put_u64(cbuf_t *cbuf, uint64_t val);

extern int cbuf_get_i8(cbuf_t *cbuf, int8_t *val);
extern int cbuf_get_i16(cbuf_t *cbuf, int16_t *val);
extern int cbuf_get_i32(cbuf_t *cbuf, int32_t *val);
extern int cbuf_get_i64(cbuf_t *cbuf, int64_t *val);

extern int cbuf_put_i8(cbuf_t *cbuf, int8_t val);
extern int cbuf_put_i16(cbuf_t *cbuf, int16_t val);
extern int cbuf_put_i32(cbuf_t *cbuf, int32_t val);
extern int cbuf_put_i64(cbuf_t *cbuf, int64_t val);

#if 0
extern int cbuf_get_ptr(cbuf_t *cbuf, size_t offset, size_t length, void **val);

#define	CBUF_GET_PTR(cbuf, offset, valpp) \
	cbuf_get_ptr(cbuf, offset, sizeof (**valpp), (void **)valpp)
#endif

#define	CBUF_SYSREAD_ENTIRE		0

extern int cbuf_sys_read(cbuf_t *cbuf, int fd, size_t want, size_t *actual);
extern int cbuf_sys_write(cbuf_t *cbuf, int fd, size_t want, size_t *actual);

extern size_t cbuf_copy(cbuf_t *, cbuf_t *);

extern void cbuf_dump(cbuf_t *cbuf, FILE *fp);
extern void cbufq_dump(cbufq_t *cbufq, FILE *fp);

extern int cbufq_pullup(cbufq_t *cbufq, size_t min_contig);

/*
 * BUFFER QUEUES
 */

extern int cbufq_alloc(cbufq_t **);
extern void cbufq_free(cbufq_t *);

extern void cbufq_enq(cbufq_t *, cbuf_t *);
extern cbuf_t *cbufq_deq(cbufq_t *);
extern cbuf_t *cbufq_peek(cbufq_t *);
extern cbuf_t *cbufq_peek_tail(cbufq_t *);

extern size_t cbufq_available(cbufq_t *);
extern size_t cbufq_count(cbufq_t *);

#endif	/* !_LIBCBUF_H */
