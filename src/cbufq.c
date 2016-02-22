
#include <stdlib.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/debug.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <inttypes.h>

#include "libcbuf_impl.h"
#include "libcbuf.h"

int
cbufq_alloc(cbufq_t **cbufqp)
{
	cbufq_t *cbufq;

	*cbufqp = NULL;

	if ((cbufq = calloc(1, sizeof (*cbufq))) == NULL) {
		return (-1);
	}

	list_create(&cbufq->cbufq_bufs, sizeof (cbuf_t), offsetof(cbuf_t,
	    cbuf_link));

	*cbufqp = cbufq;
	return (0);
}

size_t
cbufq_count(cbufq_t *cbufq)
{
	return (cbufq->cbufq_count);
}

void
cbufq_free(cbufq_t *cbufq)
{
	cbuf_t *cbuf;

	if (cbufq == NULL) {
		return;
	}

	/*
	 * Free any buffers left in the queue.
	 */
	while ((cbuf = list_remove_head(&cbufq->cbufq_bufs)) != NULL) {
		cbuf_free(cbuf);
	}

	free(cbufq);
}

void
cbufq_enq(cbufq_t *cbufq, cbuf_t *cbuf)
{
	VERIFY(!list_link_active(&cbuf->cbuf_link));

	/*
	 * Ensure that either "cbuf_flip()", "cbuf_rewind()" or "cbuf_compact()"
	 * has been called on this buffer before insertion in the queue.
	 */
	VERIFY(cbuf_position(cbuf) == 0);

	if (list_is_empty(&cbufq->cbufq_bufs)) {
		VERIFY(cbufq->cbufq_count == 0);
	} else {
		VERIFY(cbufq->cbufq_count >= 1);
	}

	cbufq->cbufq_count++;
	list_insert_tail(&cbufq->cbufq_bufs, cbuf);
}

static cbuf_t *
cbufq_deq_common(cbufq_t *cbufq, int remove)
{
	cbuf_t *head;

	if (list_is_empty(&cbufq->cbufq_bufs)) {
		VERIFY(cbufq->cbufq_count == 0);
		return (NULL);
	}

	VERIFY(cbufq->cbufq_count >= 1);
	if (remove) {
		cbufq->cbufq_count--;
		head = list_remove_head(&cbufq->cbufq_bufs);
	} else {
		head = list_head(&cbufq->cbufq_bufs);
	}

	/*
	 * Ensure the useful data in the buffer starts at index 0.
	 */
	cbuf_compact(head);

	return (head);
}

cbuf_t *
cbufq_deq(cbufq_t *cbufq)
{
	return (cbufq_deq_common(cbufq, 1));
}

cbuf_t *
cbufq_peek(cbufq_t *cbufq)
{
	return (cbufq_deq_common(cbufq, 0));
}

cbuf_t *
cbufq_peek_tail(cbufq_t *cbufq)
{
	if (list_is_empty(&cbufq->cbufq_bufs)) {
		VERIFY(cbufq->cbufq_count == 0);
		return (NULL);
	}

	VERIFY(cbufq->cbufq_count >= 1);
	cbuf_t *tail = list_tail(&cbufq->cbufq_bufs);
	if (tail != NULL) {
		cbuf_compact(tail);
	}

	return (tail);
}


size_t
cbufq_available(cbufq_t *cbufq)
{
	size_t tots = 0;
	cbuf_t *cbuf;

	for (cbuf = list_head(&cbufq->cbufq_bufs); cbuf != NULL;
	    cbuf = list_next(&cbufq->cbufq_bufs, cbuf)) {
		VERIFY0(cbuf_safe_add(&tots, tots, cbuf_available(cbuf)));
	}

	return (tots);
}

int
cbufq_pullup(cbufq_t *cbufq, size_t min_contig)
{
	cbuf_t *cbuf0, *cbuf1;
	size_t sz, pos0;

	if (min_contig == 0) {
		return (0);
	}

top:
	/*
	 * Case 1: There are no buffers.
	 */
	if (cbufq->cbufq_count == 0) {
		VERIFY(list_is_empty(&cbufq->cbufq_bufs));

		errno = EIO;
		return (-1);
	}

	/*
	 * Case 2: There is one buffer.
	 */
	if (cbufq->cbufq_count == 1) {
		VERIFY(!list_is_empty(&cbufq->cbufq_bufs));
		VERIFY(list_head(&cbufq->cbufq_bufs) ==
		    list_tail(&cbufq->cbufq_bufs));

		if (min_contig > cbuf_available(list_head(
		    &cbufq->cbufq_bufs))) {
			errno = EIO;
			return (-1);
		}

		return (0);
	}

	/*
	 * Case 3: There are two or more buffers.
	 */
	VERIFY(!list_is_empty(&cbufq->cbufq_bufs));
	VERIFY(list_head(&cbufq->cbufq_bufs) != list_tail(&cbufq->cbufq_bufs));

	cbuf0 = list_head(&cbufq->cbufq_bufs);
	if (min_contig <= cbuf_available(cbuf0)) {
		/*
		 * The first buffer is long enough.
		 */
		return (0);
	}

	VERIFY0(cbuf_safe_add(&sz, cbuf_available(cbuf0), cbuf_unused(cbuf0)));
	if (min_contig > sz) {
		/*
		 * The first buffer does not even have enough backing store to
		 * allow for the requested minimum contiguous length.  Extend
		 * the buffer.
		 */
		if (cbuf_extend(cbuf0, min_contig - cbuf_position(cbuf0)) != 0) {
			return (-1);
		}
	}

	/*
	 * Preserve the original position for the first buffer in the queue; it
	 * will be restored after the copying.  Resume putting to the buffer at
	 * the current limit.
	 */
	pos0 = cbuf_position(cbuf0);
	cbuf_resume(cbuf0);

	cbuf1 = list_next(&cbufq->cbufq_bufs, cbuf0);

	cbuf_copy(cbuf1, cbuf0);
	if (cbuf_available(cbuf1) == 0) {
		/*
		 * Consign this buffer to the scrap heap, as it is now empty.
		 */
		list_remove(&cbufq->cbufq_bufs, cbuf1);
		cbufq->cbufq_count--;
		cbuf_free(cbuf1);
	}

	cbuf_flip(cbuf0);
	VERIFY0(cbuf_position_set(cbuf0, pos0));
	goto top;
}

void
cbufq_dump(cbufq_t *cbufq, FILE *fp)
{
	unsigned i = 0;

	fprintf(fp, "cbufq[%p]: count %8u\n", cbufq, cbufq->cbufq_count);
	for (cbuf_t *cbuf = list_head(&cbufq->cbufq_bufs); cbuf != NULL;
	    cbuf = list_next(&cbufq->cbufq_bufs, cbuf)) {
		fprintf(fp, "--- entry %8u ---\n", i++);
		cbuf_dump(cbuf, fp);
	}
	fprintf(fp, "cbufq[%p]: end\n\n", cbufq);
}
