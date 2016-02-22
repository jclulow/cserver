

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
#include <sys/time.h>

#include <sys/list.h>

#include <json-nvlist.h>
#include <nvpair_json.h>

#include "libcbuf.h"
#include "libcloop.h"

#define	LISTEN_PORT	"5757"

static cserver_t *csrv;
static custr_t *scratch;
static int cmon_next_id = 1;
static nvlist_t *nvl_hbmsg;

list_t cmon_list;

typedef struct cmon {
	int cmon_id;
	cconn_t *cmon_conn;
	list_node_t cmon_link;
	hrtime_t cmon_last_recv;
	hrtime_t cmon_last_send;
} cmon_t;

void
cmon_on_close(cconn_t *ccn, int event)
{
	cmon_t *cmon = cconn_data(ccn);

	VERIFY(event == CCONN_CB_CLOSE);

	fprintf(stderr, "[%p]<%3d> closed\n", ccn, cmon->cmon_id);

	list_remove(&cmon_list, cmon);
	free(cmon);
}

void
cmon_on_end(cconn_t *ccn, int event)
{
	cmon_t *cmon = cconn_data(ccn);

	VERIFY(event == CCONN_CB_END);

	fprintf(stderr, "[%p]<%3d> read side ended\n", ccn, cmon->cmon_id);

	cconn_fin(ccn);
}

int
cmon_send_json(cconn_t *ccn, nvlist_t *nvl)
{
	int r;
	cmon_t *cmon = cconn_data(ccn);

	custr_reset(scratch);
	if (cmon_nvlist_to_json(nvl, scratch) != 0 ||
	    custr_appendc(scratch, '\n') != 0) {
		return (-1);
	}

	if ((r = cconn_send(ccn, scratch)) != 0) {
		return (r);
	}

	cmon->cmon_last_send = gethrtime();
	return (0);
}

void
cmon_on_json(cconn_t *ccn, cmon_t *cmon, nvlist_t *nvl)
{
	fprintf(stderr, "[%p]<%3d> JSON:\n", ccn, cmon->cmon_id);
	nvlist_print(stderr, nvl);
}

void
cmon_on_line(cconn_t *ccn, int event)
{
	cmon_t *cmon = cconn_data(ccn);

	cmon->cmon_last_recv = gethrtime();

	VERIFY(event == CCONN_CB_LINE_AVAILABLE);

	custr_t *cu = cconn_line(ccn);
	if (cu == NULL || custr_len(cu) < 1) {
		cconn_next(ccn);
		return;
	}

	fprintf(stderr, "[%p]<%3d> input: %s\n", ccn, cmon->cmon_id,
	    custr_cstr(cu));

	custr_reset(scratch);

	if (custr_cstr(cu)[0] == '{') {
		nvlist_parse_json_error_t nje = { 0 };
		nvlist_t *nvl;

		/*
		 * This is a JSON input line.
		 */
		if (nvlist_parse_json(custr_cstr(cu), custr_len(cu),
		    &nvl, NVJSON_FORCE_INTEGER, &nje) != 0) {
			fprintf(stderr, "[%p]<%3d> JSON error: %s\n", ccn,
			    cmon->cmon_id, nje.nje_message);
			cconn_abort(ccn);
			return;
		}

		cmon_on_json(ccn, cmon, nvl);

	} else if (strcmp(custr_cstr(cu), "json") == 0) {
		nvlist_t *nvl = NULL;

		if (nvlist_alloc(&nvl, NV_UNIQUE_NAME, 0) != 0) {
			cconn_abort(ccn);
			return;
		}

		nvlist_add_string(nvl, "awesome", "value!");
		nvlist_add_int32(nvl, "number", 64);

		cmon_send_json(ccn, nvl);
		nvlist_free(nvl);
	} else {
		custr_append(scratch, "my responses are limited, you must ask "
		    "the right questions\n");
		custr_append_printf(scratch, "unknown: %s\n", custr_cstr(cu));
		if (cconn_send(ccn, scratch) == 0) {
			cmon->cmon_last_send = gethrtime();
		}
	}

	cconn_next(ccn);
}

void
cmon_on_incoming(cserver_t *csrv, int event)
{
	VERIFY(event == CSERVER_CB_INCOMING);

	for (;;) {
		cconn_t *ccn;

		if (cserver_accept(csrv, &ccn) != 0) {
			if (errno != EAGAIN) {
				warn("cserver_accept");
			}
			return;
		}

		cmon_t *cmon = calloc(1, sizeof (*cmon));
		if (cmon == NULL) {
			cconn_abort(ccn);
			continue;
		}
		cmon->cmon_id = cmon_next_id++;
		list_insert_tail(&cmon_list, cmon);
		cmon->cmon_conn = ccn;
		cconn_data_set(ccn, cmon);
		cmon->cmon_last_send = cmon->cmon_last_recv = gethrtime();

		cconn_on(ccn, CCONN_CB_LINE_AVAILABLE, cmon_on_line);
		cconn_on(ccn, CCONN_CB_CLOSE, cmon_on_close);
		cconn_on(ccn, CCONN_CB_END, cmon_on_end);

		fprintf(stderr, "[%p]<%3d> accepted: %s\n", ccn, cmon->cmon_id,
		    cconn_remote_addr_str(ccn));
	}
}

void
cmon_on_timer(cloop_ent_t *clent, int event)
{
	VERIFY(event == CLOOP_CB_TIMER);

	static hrtime_t recv_timeout = 24 * 1000000000LL;
	static hrtime_t send_hb_interval = 5 * 1000000000LL;

	hrtime_t now = gethrtime();
	nvlist_add_uint64(nvl_hbmsg, "hrtime", now);
	nvlist_add_int64(nvl_hbmsg, "time", time(NULL));

	cmon_t *cmon, *cmon_next;
	for (cmon = list_head(&cmon_list); cmon != NULL; cmon = cmon_next) {
		cmon_next = list_next(&cmon_list, cmon);

		if ((now - cmon->cmon_last_recv) > recv_timeout) {
			cconn_abort(cmon->cmon_conn);
			continue;
		}

		if ((now - cmon->cmon_last_send) > send_hb_interval) {
			cmon_send_json(cmon->cmon_conn, nvl_hbmsg);
		}
	}
}


int
main(int argc, char *argv[])
{
	cloop_t *cloop;
	cloop_ent_t *cltimer;

	if (nvlist_alloc(&nvl_hbmsg, NV_UNIQUE_NAME, 0) != 0 ||
	    nvlist_add_string(nvl_hbmsg, "type", "heartbeat") != 0) {
		err(1, "nvlist");
	}

	list_create(&cmon_list, sizeof (cmon_t), offsetof(cmon_t, cmon_link));

	if (cloop_alloc(&cloop) != 0) {
		err(1, "cloop_alloc");
	}

	if (cloop_ent_alloc(&cltimer) != 0) {
		err(1, "cloop_ent_alloc");
	}
	if (cloop_attach_ent_timer(cloop, cltimer, 1) != 0){
		err(1, "cloop_attach_ent_timer");
	}
	cloop_ent_on(cltimer, CLOOP_CB_TIMER, cmon_on_timer);

	if (custr_alloc(&scratch) != 0) {
		err(1, "custr_alloc");
	}

	if (cserver_alloc(&csrv) != 0) {
		err(1, "cserver_alloc");
	}
	cserver_on(csrv, CSERVER_CB_INCOMING, cmon_on_incoming);

	if (cserver_listen_tcp(csrv, cloop, "0.0.0.0", LISTEN_PORT) != 0) {
		err(1, "cserver_listen");
	}
	fprintf(stderr, "LISTENING ON PORT %s\n", LISTEN_PORT);

	for (;;) {
		unsigned int again = 0;

		if (cloop_run(cloop, &again) != 0) {
			err(1, "cloop_run");
		}

		if (!again) {
			fprintf(stderr, "LOOP END\n");
			break;
		}
	}

	cserver_free(csrv);
	cloop_free(cloop);
	custr_free(scratch);
	nvlist_free(nvl_hbmsg);
	if (getenv("ABORT_ON_EXIT") != NULL) {
		fprintf(stderr, "aborting for findleaks\n");
		abort();
	}
	return (0);
}
