

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <err.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <signal.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <arpa/nameser.h>
#include <netdb.h>
#include <port.h>
#include <sys/debug.h>
#include <errno.h>

#include <sys/list.h>

#include <json-nvlist.h>
#include <nvpair_json.h>

#include "libcbuf.h"
#include "libcloop.h"

#define	LISTEN_PORT	"5757"

boolean_t cserver_debug = B_FALSE;

int keepidle = 1;
int keepcnt = 15;
int keepintvl = 1;

/*
 * EVENT ORDERING:
 *
 * 	Creation
 * 	  |
 * 	  +-->---------------------------------------\
 * 	  |                                          |
 * 	  V                                          |
 *	CCONN_CB_LINE_AVAILABLE <-\                  |
 *	  |                       |                  |
 *	  +-->--------------------/                  |
 *	  |                                          |
 *	  +-->------------------------------------:. |
 *	  |                                         \|
 *	  V                                          V
 *	CCONN_CB_END (end of inbound data stream)    |
 *	  |                                          |
 *	  +----> CCONN_CB_ERROR <--------------------/
 *	  |        |
 *	  V        V
 *	CCONN_CB_CLOSE (socket closed)
 */

typedef enum cconn_state {
	CCONN_ST_PRE_CONNECTION = 1,
	CCONN_ST_WAITING_FOR_LINE,
	CCONN_ST_LINE_AVAILABLE,
	CCONN_ST_READ_EOF,
	CCONN_ST_ERROR,
	CCONN_ST_CLOSED,
} cconn_state_t;

struct cconn {
	cserver_t *ccn_server;
	cconn_state_t ccn_state;

	cloop_ent_t *ccn_clent;
	struct sockaddr_storage ccn_remote_addr;
	char *ccn_remote_addr_str;

	custr_t *ccn_input;

	cbufq_t *ccn_recvq;
	boolean_t ccn_recvq_end;
	cbufq_t *ccn_sendq;
	boolean_t ccn_sendq_end;
	boolean_t ccn_sendq_flushed;

	cconn_cb_t *ccn_on_line_available;
	cconn_cb_t *ccn_on_end;
	cconn_cb_t *ccn_on_error;
	cconn_cb_t *ccn_on_close;

	list_node_t ccn_link;			/* cserver linkage */

	void *ccn_data;
};

struct cserver {
	cserver_type_t csrv_type;

	cloop_t *csrv_loop;
	cloop_ent_t *csrv_listen;

	struct sockaddr_storage csrv_addr;

	list_t csrv_connections;		/* list of cconn_t */

	/*
	 * Callbacks:
	 */
	cserver_cb_t *csrv_on_incoming;
};

static void cconn_destroy(cconn_t *ccn);
static void ccn_handle_incoming_data(cconn_t *ccn);

static char *
cconn_state_name(cconn_state_t s)
{
	return (s == CCONN_ST_PRE_CONNECTION ? "PRE_CONNECTION" :
	    s == CCONN_ST_WAITING_FOR_LINE ? "WAITING_FOR_LINE" :
	    s == CCONN_ST_LINE_AVAILABLE ? "LINE_AVAILABLE" :
	    s == CCONN_ST_READ_EOF ? "READ_EOF" :
	    s == CCONN_ST_ERROR ? "ERROR" :
	    s == CCONN_ST_CLOSED ? "CLOSED" :
	    "?UNKNOWN");
}

void *
cconn_data(cconn_t *ccn)
{
	return (ccn->ccn_data);
}

void
cconn_data_set(cconn_t *ccn, void *data)
{
	ccn->ccn_data = data;
}

static struct sockaddr_in *
cserver_sockaddr_in(cserver_t *csrv)
{
	VERIFY(csrv->csrv_type == CSERVER_TYPE_TCP);

	return ((struct sockaddr_in *)&csrv->csrv_addr);
}

static struct sockaddr_in *
cconn_sockaddr_in(cconn_t *ccn)
{
	return ((struct sockaddr_in *)&ccn->ccn_remote_addr);
}

void
cconn_advance_state(cconn_t *ccn, cconn_state_t nstate)
{
	cconn_state_t ostate;

	if (ccn->ccn_state == CCONN_ST_CLOSED) {
		/*
		 * Nothing left to do.
		 */
		return;
	}

	if (ccn->ccn_state == nstate) {
		/*
		 * This is not a transition.
		 */
		return;
	}

top:
	ostate = ccn->ccn_state;
	ccn->ccn_state = nstate;
	if (cserver_debug) {
		fprintf(stderr, "STATE: %s -> %s\n", cconn_state_name(ostate),
		    cconn_state_name(nstate));
	}

	switch (nstate) {
	case CCONN_ST_ERROR:
		VERIFY(ostate != CCONN_ST_CLOSED);
		if (ccn->ccn_on_error != NULL) {
			ccn->ccn_on_error(ccn, CCONN_CB_ERROR);
		}
		nstate = CCONN_ST_CLOSED;
		goto top;

	case CCONN_ST_CLOSED:
		cloop_ent_free(ccn->ccn_clent);
		ccn->ccn_clent = NULL;
		if (ccn->ccn_on_close != NULL) {
			ccn->ccn_on_close(ccn, CCONN_CB_CLOSE);
		}
		cconn_destroy(ccn);
		return;

	case CCONN_ST_LINE_AVAILABLE:
		VERIFY(ostate == CCONN_ST_WAITING_FOR_LINE);
		if (ccn->ccn_on_line_available != NULL) {
			ccn->ccn_on_line_available(ccn,
			    CCONN_CB_LINE_AVAILABLE);
		}
		return;

	case CCONN_ST_WAITING_FOR_LINE:
		VERIFY(ostate == CCONN_ST_PRE_CONNECTION ||
		    ostate == CCONN_ST_LINE_AVAILABLE);

		/*
		 * Trigger the incoming data routine.  This routine
		 * will mark the socket for read if needed.
		 */
		ccn_handle_incoming_data(ccn);
		return;

	case CCONN_ST_READ_EOF:
		VERIFY(ostate == CCONN_ST_WAITING_FOR_LINE);
		if (ccn->ccn_on_end != NULL) {
			ccn->ccn_on_end(ccn, CCONN_CB_END);
		}
		if (ccn->ccn_sendq_flushed) {
			/*
			 * Both the inbound and outbound data streams have
			 * come to an end.  Close down the socket.
			 */
			nstate = CCONN_ST_CLOSED;
			goto top;
		}
		return;

	case CCONN_ST_PRE_CONNECTION:
		abort();
		return;
	}

	abort();
}

int
cconn_fin(cconn_t *ccn)
{
	switch (ccn->ccn_state) {
	case CCONN_ST_LINE_AVAILABLE:
	case CCONN_ST_WAITING_FOR_LINE:
	case CCONN_ST_READ_EOF:
		break;

	default:
		errno = EINVAL;
		return (-1);
	}

	ccn->ccn_sendq_end = B_TRUE;
	cloop_ent_want(ccn->ccn_clent, CLOOP_CB_WRITE);
	return (0);
}

int
cconn_abort(cconn_t *ccn)
{
	switch (ccn->ccn_state) {
	case CCONN_ST_LINE_AVAILABLE:
	case CCONN_ST_WAITING_FOR_LINE:
	case CCONN_ST_READ_EOF:
		break;

	default:
		errno = EINVAL;
		return (-1);
	}

	/*
	 * Enable an immediate close of the connection by setting SO_LINGER
	 * with a timeout of zero.
	 */
	struct linger l;
	l.l_onoff = 1;
	l.l_linger = 0;
	if (setsockopt(cloop_ent_fd(ccn->ccn_clent), SOL_SOCKET, SO_LINGER,
	    &l, sizeof (l)) != 0) {
		return (-1);
	}

	cconn_advance_state(ccn, CCONN_ST_CLOSED);
	return (0);
}

int
cconn_send(cconn_t *ccn, custr_t *cu)
{
	cbuf_t *cbuf;

	switch (ccn->ccn_state) {
	case CCONN_ST_LINE_AVAILABLE:
	case CCONN_ST_WAITING_FOR_LINE:
	case CCONN_ST_READ_EOF:
		break;

	default:
		errno = EINVAL;
		return (-1);
	}

	if (ccn->ccn_sendq_end) {
		errno = EPIPE;
		return (-1);
	}

	if (cbuf_alloc(&cbuf, custr_len(cu)) != 0) {
		return (-1);
	}

	VERIFY0(cbuf_put_string(cbuf, cu));
	cbuf_flip(cbuf);
	cbufq_enq(ccn->ccn_sendq, cbuf);
	cloop_ent_want(ccn->ccn_clent, CLOOP_CB_WRITE);
	return (0);
}

custr_t *
cconn_line(cconn_t *ccn)
{
	if (ccn->ccn_state != CCONN_ST_LINE_AVAILABLE) {
		return (NULL);
	}

	return (ccn->ccn_input);
}

void
cconn_next(cconn_t *ccn)
{
	if (ccn->ccn_state != CCONN_ST_LINE_AVAILABLE) {
		return;
	}

	custr_reset(ccn->ccn_input);
	cconn_advance_state(ccn, CCONN_ST_WAITING_FOR_LINE);
}

/*
 * XXX This routine currently copies bytes from the incoming buffer queue
 * into a custr_t until it finds a linefeed.  This could be so much
 * better than it is.
 */
static void
ccn_handle_incoming_data(cconn_t *ccn)
{
	cbuf_t *head;

	if (ccn->ccn_state == CCONN_ST_LINE_AVAILABLE) {
		/*
		 * We already have a line available.  Do nothing for now.
		 */
		return;
	}

	while ((head = cbufq_peek(ccn->ccn_recvq)) != NULL) {
		char val;

		if (cbuf_available(head) < 1) {
			cbuf_free(cbufq_deq(ccn->ccn_recvq));
			continue;
		}

		VERIFY0(cbuf_get_char(head, &val));

		if (val == '\n') {
			cconn_advance_state(ccn, CCONN_ST_LINE_AVAILABLE);
			return;
		}

		if (custr_appendc(ccn->ccn_input, val) != 0) {
			warn("custr_appendc");
			cconn_advance_state(ccn, CCONN_ST_ERROR);
			return;
		}
	}

	/*
	 * We have run out of buffers in the receive queue.
	 */
	if (ccn->ccn_recvq_end) {
		/*
		 * We have also hit EOF on the read side of the socket.
		 * Inform our consumer that there will be no more data.
		 */
		cconn_advance_state(ccn, CCONN_ST_READ_EOF);
	} else {
		/*
		 * As far as we know, there is more data to come.  Request
		 * additional reads.
		 */
		cloop_ent_want(ccn->ccn_clent, CLOOP_CB_READ);
	}
}

void
cconn_on_hangup(cloop_ent_t *clent, int ev)
{
	cconn_t *ccn = cloop_ent_data(clent);

	VERIFY(ev == CLOOP_CB_HANGUP);

	if (cserver_debug) {
		fprintf(stderr, "CCONN[%p] HANGUP\n", ccn);
	}
	cconn_advance_state(ccn, CCONN_ST_ERROR);
}

void
cconn_on_error(cloop_ent_t *clent, int ev)
{
	cconn_t *ccn = cloop_ent_data(clent);

	VERIFY(ev == CLOOP_CB_ERROR);

	if (cserver_debug) {
		fprintf(stderr, "CCONN[%p] ERROR\n", ccn);
	}
	cconn_advance_state(ccn, CCONN_ST_ERROR);
}

void
cconn_on_write(cloop_ent_t *clent, int ev)
{
	cconn_t *ccn = cloop_ent_data(clent);

	VERIFY(ev == CLOOP_CB_WRITE);

	if (cserver_debug) {
		fprintf(stderr, "CCONN[%p] WRITE DATA\n", ccn);
	}

	boolean_t qempty = (cbufq_peek(ccn->ccn_sendq) == NULL);
	if (qempty && !ccn->ccn_sendq_end) {
		/*
		 * Nothing to send.
		 */
		return;
	}

	if (qempty) {
		/*
		 * The outbound queue is empty _and_ we have no more data to
		 * send.  Proceed with a FIN.
		 */
		if (shutdown(cloop_ent_fd(clent), SHUT_WR) != 0) {
			warn("shutdown(SHUT_WR)");
			cconn_advance_state(ccn, CCONN_ST_ERROR);
			return;
		}
		ccn->ccn_sendq_flushed = B_TRUE;

		if (ccn->ccn_state == CCONN_ST_READ_EOF) {
			/*
			 * If the read side has already shut down,
			 * we can close the whole connection now.
			 */
			cconn_advance_state(ccn, CCONN_ST_CLOSED);
		}
		return;
	}

	cbuf_t *head;
	while ((head = cbufq_peek(ccn->ccn_sendq)) != NULL) {
		size_t actual = 0;
		size_t want;

		if ((want = cbuf_available(head)) < 1) {
			cbuf_free(cbufq_deq(ccn->ccn_sendq));
			continue;
		}

retry:
		if (cbuf_sys_write(head, cloop_ent_fd(clent),
		    CBUF_SYSREAD_ENTIRE, &actual) != 0) {
			switch (errno) {
			case EINTR:
				goto retry;

			case EAGAIN:
				cloop_ent_want(clent, CLOOP_CB_WRITE);
				return;

			case ECONNRESET:
				if (cserver_debug) {
					fprintf(stderr, "CCONN[%p] "
					    "ECONNRESET\n", ccn);
				}
				return;

			default:
				err(1, "cbuf_sys_write");
			}
		}
	}

	cloop_ent_want(clent, CLOOP_CB_WRITE);
}

void
cconn_on_read(cloop_ent_t *clent, int ev)
{
	cconn_t *ccn = cloop_ent_data(clent);
	cbuf_t *cbuf = NULL;
	size_t actual = 0;
	boolean_t new_cbuf = B_FALSE;

	VERIFY(ev == CLOOP_CB_READ);

	if (cserver_debug) {
		fprintf(stderr, "CCONN[%p] READ DATA\n", ccn);
	}

	/*
	 * Check to see if we have space in the tail of the buffer queue:
	 */
	if ((cbuf = cbufq_peek_tail(ccn->ccn_recvq)) != NULL &&
	    cbuf_unused(cbuf) > 64) {
		cbuf_resume(cbuf);
		VERIFY(cbuf_available(cbuf) > 64);
	} else {
		/*
		 * Allocate a new buffer:
		 */
		new_cbuf = B_TRUE;
		if (cbuf_alloc(&cbuf, 2048) != 0) {
			err(1, "cbuf_alloc");
		}
	}

retry:
	if (cbuf_sys_read(cbuf, cloop_ent_fd(clent), CBUF_SYSREAD_ENTIRE,
	    &actual) != 0) {
		switch (errno) {
		case EINTR:
			goto retry;

		case EAGAIN:
			cloop_ent_want(clent, CLOOP_CB_READ);
			goto out;

		case ECONNRESET:
			cconn_advance_state(ccn, CCONN_ST_ERROR);
			goto out;

		default:
			err(1, "cbuf_sys_read");
		}
	} else if (actual == 0) {
		/*
		 * Mark the receive queue as finished.  We will transition
		 * to EOF once the line handling function has run out of
		 * data.
		 */
		ccn->ccn_recvq_end = B_TRUE;
		if (cserver_debug) {
			fprintf(stderr, "CCONN[%p] READ EOF\n", ccn);
		}
	} else if (cserver_debug) {
		fprintf(stderr, "CCONN[%p] READ %u BYTES\n", ccn, actual);
	}

	cbuf_flip(cbuf);
	if (new_cbuf) {
		if (actual == 0) {
			cbuf_free(cbuf);
		} else {
			cbufq_enq(ccn->ccn_recvq, cbuf);
		}
	}

	/*
	 * Handle any new data that has arrived:
	 */
	ccn_handle_incoming_data(ccn);
	return;

out:
	if (new_cbuf) {
		cbuf_free(cbuf);
	} else {
		cbuf_flip(cbuf);
	}
}

static void
cconn_destroy(cconn_t *ccn)
{
	int e = errno;

	if (ccn == NULL)
		return;

	if (ccn->ccn_server != NULL) {
		list_remove(&ccn->ccn_server->csrv_connections, ccn);
	}

	cloop_ent_free(ccn->ccn_clent);
	cbufq_free(ccn->ccn_recvq);
	cbufq_free(ccn->ccn_sendq);
	custr_free(ccn->ccn_input);
	free(ccn->ccn_remote_addr_str);

	free(ccn);

	errno = e;
}

static int
cconn_alloc(cconn_t **ccnp)
{
	cconn_t *ccn = NULL;

	if ((ccn = calloc(1, sizeof (*ccn))) == NULL) {
		return (-1);
	}

	if (cloop_ent_alloc(&ccn->ccn_clent) != 0 ||
	    cbufq_alloc(&ccn->ccn_recvq) != 0 ||
	    cbufq_alloc(&ccn->ccn_sendq) != 0 ||
	    custr_alloc(&ccn->ccn_input) != 0) {
		cconn_destroy(ccn);
		return (-1);
	}

	/*
	 * Link the cloop entity and the connection object.
	 */
	cloop_ent_data_set(ccn->ccn_clent, ccn);

	/*
	 * Install the appropriate cloop entity event callbacks.
	 */
	cloop_ent_on(ccn->ccn_clent, CLOOP_CB_HANGUP, cconn_on_hangup);
	cloop_ent_on(ccn->ccn_clent, CLOOP_CB_READ, cconn_on_read);
	cloop_ent_on(ccn->ccn_clent, CLOOP_CB_WRITE, cconn_on_write);
	cloop_ent_on(ccn->ccn_clent, CLOOP_CB_ERROR, cconn_on_error);

	/*
	 * Set the connection to the pre-connection state:
	 */
	ccn->ccn_state = CCONN_ST_PRE_CONNECTION;

	*ccnp = ccn;
	return (0);
}

int
cserver_accept(cserver_t *csrv, cconn_t **ccnp)
{
	cconn_t *ccn = NULL;
	size_t sz;
	int e;
	int fd;

	if (cconn_alloc(&ccn) != 0) {
		*ccnp = NULL;
		return (-1);
	}

	sz = sizeof (ccn->ccn_remote_addr);
retry:
	if ((fd = accept4(cloop_ent_fd(csrv->csrv_listen),
	    (struct sockaddr *)&ccn->ccn_remote_addr, &sz,
	    SOCK_CLOEXEC | SOCK_NONBLOCK)) < 0) {
		switch (errno) {
		case EINTR:
			goto retry;

		case ECONNABORTED:
		case EWOULDBLOCK:
			/*
			 * Not actionable.  Back to sleep.
			 */
			e = EWOULDBLOCK;
			cloop_ent_want(csrv->csrv_listen, CLOOP_CB_READ);
			goto fail;

		default:
			err(1, "accept4");
		}
	}

	/*
	 * Set socket options.
	 */
	int opt_on = 1;
	if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &opt_on,
	    sizeof (opt_on)) != 0) {
		e = errno;
		warn("could not set SO_KEEPALIVE");
		goto fail;
	}
	(void) setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &keepidle,
	    sizeof (keepidle));
	(void) setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &keepcnt,
	    sizeof (keepcnt));
	(void) setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &keepintvl,
	    sizeof (keepintvl));

	/*
	 * We want to be notified when there are more incoming connections.
	 */
	cloop_ent_want(csrv->csrv_listen, CLOOP_CB_READ);

	/*
	 * Link this connection into the server connection list:
	 */
	ccn->ccn_server = csrv;
	list_insert_tail(&csrv->csrv_connections, ccn);

	/*
	 * Attach our cloop entity to the event loop:
	 */
	cloop_attach_ent(csrv->csrv_loop, ccn->ccn_clent, fd);

	/*
	 * Mark this connection as waiting for incoming data.
	 */
	cconn_advance_state(ccn, CCONN_ST_WAITING_FOR_LINE);

	ccn->ccn_remote_addr_str = strdup(inet_ntoa(cconn_sockaddr_in(
	    ccn)->sin_addr));
	if (cserver_debug) {
		fprintf(stderr, "ACCEPTED (%s)\n", ccn->ccn_remote_addr_str);
	}

	*ccnp = ccn;
	return (0);

fail:
	cconn_destroy(ccn);
	errno = e;
	return (-1);
}

static void
cserver_on_incoming(cloop_ent_t *clent, int event)
{
	cserver_t *csrv = cloop_ent_data(clent);

	VERIFY(event == CLOOP_CB_READ);


	if (csrv->csrv_on_incoming != NULL) {
		/*
		 * Report a possible incoming connection to our consumer.
		 * Note that we do not rearm for read events; this happens
		 * in cserver_accept().
		 */
		csrv->csrv_on_incoming(csrv, CSERVER_CB_INCOMING);
	} else {
		/*
		 * XXX Should we abort here?
		 */
		fprintf(stderr, "CSERVER[%p]: INCOMING WITH NO HANDLER!\n",
		    csrv);
		cloop_ent_want(clent, CLOOP_CB_READ);
	}
}

const char *
cconn_remote_addr_str(cconn_t *ccn)
{
	return (ccn->ccn_remote_addr_str);
}

void
cconn_on(cconn_t *ccn, int event, cconn_cb_t *func)
{
	switch ((cconn_cb_type_t) event) {
	case CCONN_CB_LINE_AVAILABLE:
		ccn->ccn_on_line_available = func;
		return;

	case CCONN_CB_ERROR:
		ccn->ccn_on_error = func;
		return;

	case CCONN_CB_END:
		ccn->ccn_on_end = func;
		return;

	case CCONN_CB_CLOSE:
		ccn->ccn_on_close = func;
		return;
	}

	warnx("unknown cconn cb %d\n", event);
	abort();
}

void
cserver_on(cserver_t *csrv, int event, cserver_cb_t *func)
{
	switch (event) {
	case CSERVER_CB_INCOMING:
		csrv->csrv_on_incoming = func;
		break;

	default:
		warnx("unknown cserver cb %d\n", event);
		abort();
		break;
	}
}

static void
cserver_on_error(cloop_ent_t *clent, int event)
{
	cserver_t *csrv = cloop_ent_data(clent);

	VERIFY(event == CLOOP_CB_ERROR);

	/*
	 * It's not clear why this would happen to us on a listen socket.
	 */
	fprintf(stderr, "CSERVER[%p]: ERROR!\n", csrv);
	abort();
}

static void
cserver_on_hangup(cloop_ent_t *clent, int event)
{
	cserver_t *csrv = cloop_ent_data(clent);

	VERIFY(event == CLOOP_CB_HANGUP);

	/*
	 * It's not clear why this would happen to us on a listen socket.
	 */
	fprintf(stderr, "CSERVER[%p]: HANGUP!\n", csrv);
	abort();
}

void
cserver_free(cserver_t *csrv)
{
	if (csrv == NULL) {
		return;
	}

	cserver_close(csrv);

	/*
	 * Destroy all connections.
	 */
	while (!list_is_empty(&csrv->csrv_connections)) {
		cconn_destroy(list_head(&csrv->csrv_connections));
	}

	free(csrv);
}

int
cserver_alloc(cserver_t **csrvp)
{
	cserver_t *csrv = NULL;
	cloop_ent_t *clent = NULL;

	if (getenv("CSERVER_DEBUG") != NULL) {
		cserver_debug = B_TRUE;
	}

	if ((csrv = calloc(1, sizeof (*csrv))) == NULL) {
		return (-1);
	}
	csrv->csrv_type = CSERVER_TYPE_NONE;

	if (cloop_ent_alloc(&clent) != 0) {
		free(csrv);
		return (-1);
	}

	/*
	 * Create list of connection objects for this server:
	 */
	list_create(&csrv->csrv_connections, sizeof (cconn_t),
	    offsetof(cconn_t, ccn_link));

	/*
	 * Link the listen server to the cloop entity:
	 */
	csrv->csrv_listen = clent;
	cloop_ent_data_set(clent, csrv);

	*csrvp = csrv;
	return (0);
}

static int
cserver_parse_ipv4addr(const char *ipaddr, const char *port,
    struct sockaddr_in *addr)
{
	bzero(addr, sizeof (*addr));

	addr->sin_family = AF_INET;
	addr->sin_port = htons(atoi(port));

	switch (inet_pton(AF_INET, ipaddr, &addr->sin_addr)) {
	case 1:
		return (0);
	case 0:
		warnx("inet_pton (%s) invalid address", ipaddr);
		errno = EPROTO;
		return (-1);
	default:
		warn("inet_pton (%s) failure", ipaddr);
		return (-1);
	}
}

int
cserver_listen_tcp(cserver_t *csrv, cloop_t *cloop, const char *ipaddr,
    const char *port)
{
	int e;
	int sock = -1;
	struct sockaddr_in addr;

	if (csrv->csrv_type != CSERVER_TYPE_NONE) {
		/*
		 * This server is already listening.
		 */
		errno = EINVAL;
		return (-1);
	}

	/*
	 * Create TCP listen socket.
	 */
	if ((sock = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
	    0)) < 0) {
		e = errno;
		warn("socket failed");
		goto fail;
	}

	/*
	 * Set socket options.
	 */
	int opt_on = 1;
	if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt_on,
	    sizeof (opt_on)) != 0) {
		warn("could not set SO_REUSEADDR");
		goto fail;
	}

	/*
	 * Set TCP Listen Address.
	 */
	if (cserver_parse_ipv4addr(ipaddr != NULL ? ipaddr : "0.0.0.0", port,
	    &addr) != 0) {
		e = errno;
		warn("cserver_parse_ipv4addr failed");
		goto fail;
	}

	/*
	 * Bind to the Listen Address.
	 */
	if (bind(sock, (struct sockaddr *)&addr, sizeof (addr)) != 0) {
		e = errno;
		warn("bind failed");
		goto fail;
	}

	/*
	 * Listen.
	 */
	if (listen(sock, 1000) != 0) {
		e = errno;
		warn("listen failed");
		goto fail;
	}

	/*
	 * We were successful in establishing the listen socket.  Copy
	 * the relevant data into the server object:
	 */
	csrv->csrv_type = CSERVER_TYPE_TCP;
	*cserver_sockaddr_in(csrv) = addr;

	/*
	 * Register our callbacks:
	 */
	cloop_ent_t *clent = csrv->csrv_listen;
	cloop_ent_on(clent, CLOOP_CB_READ, cserver_on_incoming);
	cloop_ent_on(clent, CLOOP_CB_HANGUP, cserver_on_hangup);
	cloop_ent_on(clent, CLOOP_CB_ERROR, cserver_on_error);

	/*
	 * We want to be notified of incoming connections:
	 */
	cloop_ent_want(clent, CLOOP_CB_READ);

	/*
	 * Attach the cloop entity to the loop:
	 */
	csrv->csrv_loop = cloop;
	cloop_attach_ent(cloop, clent, sock);
	return (0);

fail:
	if (sock != -1) {
		VERIFY0(close(sock));
	}
	errno = e;
	return (-1);
}

/*
 * Close the listen socket so as to stop accepting incoming connections.
 */
void
cserver_close(cserver_t *csrv)
{
	if (csrv->csrv_listen == NULL)
		return;

	cloop_ent_free(csrv->csrv_listen);
	csrv->csrv_listen = NULL;
}

void
cserver_abort(cserver_t *csrv)
{
	/*
	 * Abort all connections.
	 */
	while (!list_is_empty(&csrv->csrv_connections)) {
		cconn_abort(list_head(&csrv->csrv_connections));
	}
}
