/*
 * Copyright (c) 2019 Martijn van Duren <martijn@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
#include <sys/tree.h>
#include <sys/types.h>
#include <sys/socket.h>

#include <err.h>
#include <errno.h>
#include <event.h>
#include <netdb.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <asr.h>

#include "smtp_proc.h"

struct dnsbl_session;

struct dnsbl_query {
	struct asr_query *query;
	struct event_asr *event;
	struct event timeout;
	int resolved;
	int blacklist;
	struct dnsbl_session *session;
};

struct dnsbl_session {
	uint64_t reqid;
	uint64_t token;
	int listed;
	struct dnsbl_query *query;
	RB_ENTRY(dnsbl_session) entry;
};

RB_HEAD(dnsbl_sessions, dnsbl_session) dnsbl_sessions = RB_INITIALIZER(NULL);
RB_PROTOTYPE(dnsbl_sessions, dnsbl_session, entry, dnsbl_session_cmp);

static char **blacklists = NULL;
static size_t nblacklists = 0;
static int markspam = 0;

void usage(void);
void dnsbl_connect(char *, int, struct timespec *, char *, char *, uint64_t,
    uint64_t, char *, struct inx_addr *);
void dnsbl_dataline(char *, int, struct timespec *, char *, char *, uint64_t,
    uint64_t, char *);
void dnsbl_disconnect(char *, int, struct timespec *, char *, char *, uint64_t);
void dnsbl_resolve(struct asr_result *, void *);
void dnsbl_timeout(int, short, void *);
void dnsbl_session_query_done(struct dnsbl_session *);
void dnsbl_session_free(struct dnsbl_session *);
int dnsbl_session_cmp(struct dnsbl_session *, struct dnsbl_session *);

int
main(int argc, char *argv[])
{
	int ch;
	int i;

	if (pledge("stdio dns", NULL) == -1)
		err(1, "pledge");

	while ((ch = getopt(argc, argv, "m")) != -1) {
		switch (ch) {
		case 'm':
			markspam = 1;
			break;
		default:
			usage();
		}
	}

	nblacklists = argc - optind;

	if ((blacklists = calloc(nblacklists, sizeof(*blacklists))) == NULL)
		err(1, NULL);
	for (i = 0; i < nblacklists; i++)
		blacklists[i] = argv[optind + i];

	if (nblacklists == 0)
		errx(1, "No blacklist specified");

	smtp_register_filter_connect(dnsbl_connect);
	if (markspam)
		smtp_register_filter_dataline(dnsbl_dataline);
	smtp_in_register_report_disconnect(dnsbl_disconnect);
	smtp_run();

	return 0;
}

void
dnsbl_connect(char *type, int version, struct timespec *tm, char *direction,
    char *phase, uint64_t reqid, uint64_t token, char *hostname,
    struct inx_addr *xaddr)
{
	struct dnsbl_session *session;
	struct timeval timeout = {1, 0};
	char query[255];
	u_char *addr;
	int i, try;

	if ((session = calloc(1, sizeof(*session))) == NULL)
		err(1, NULL);
	if ((session->query = calloc(nblacklists, sizeof(*(session->query))))
	    == NULL)
		err(1, NULL);
	session->reqid = reqid;
	session->token = token;
	session->listed = -1;
	RB_INSERT(dnsbl_sessions, &dnsbl_sessions, session);

	if (xaddr->af == AF_INET)
		addr = (u_char *)&(xaddr->addr);
	else
		addr = (u_char *)&(xaddr->addr6);
	for (i = 0; i < nblacklists; i++) {
		if (xaddr->af == AF_INET) {
			if (snprintf(query, sizeof(query), "%u.%u.%u.%u.%s",
			    addr[3], addr[2], addr[1], addr[0],
			    blacklists[i]) >= sizeof(query))
				errx(1, "Can't create query, domain too long");
		} else if (xaddr->af == AF_INET6) {
			if (snprintf(query, sizeof(query), "%hhx.%hhx.%hhx.%hhx"
			    ".%hhx.%hhx.%hhx.%hhx.%hhx.%hhx.%hhx.%hhx.%hhx.%hhx"
			    ".%hhx.%hhx.%hhx.%hhx.%hhx.%hhx.%hhx.%hhx.%hhx.%hhx"
			    ".%hhx.%hhx.%hhx.%hhx.%hhx.%hhx.%hhx.%hhx.%s",
			    (u_char) (addr[15] & 0xf), (u_char) (addr[15] >> 4),
			    (u_char) (addr[14] & 0xf), (u_char) (addr[14] >> 4),
			    (u_char) (addr[13] & 0xf), (u_char) (addr[13] >> 4),
			    (u_char) (addr[12] & 0xf), (u_char) (addr[12] >> 4),
			    (u_char) (addr[11] & 0xf), (u_char) (addr[11] >> 4),
			    (u_char) (addr[10] & 0xf), (u_char) (addr[10] >> 4),
			    (u_char) (addr[9] & 0xf), (u_char) (addr[9] >> 4),
			    (u_char) (addr[8] & 0xf), (u_char) (addr[8] >> 4),
			    (u_char) (addr[7] & 0xf), (u_char) (addr[8] >> 4),
			    (u_char) (addr[6] & 0xf), (u_char) (addr[7] >> 4),
			    (u_char) (addr[5] & 0xf), (u_char) (addr[5] >> 4),
			    (u_char) (addr[4] & 0xf), (u_char) (addr[4] >> 4),
			    (u_char) (addr[3] & 0xf), (u_char) (addr[3] >> 4),
			    (u_char) (addr[2] & 0xf), (u_char) (addr[2] >> 4),
			    (u_char) (addr[1] & 0xf), (u_char) (addr[1] >> 4),
			    (u_char) (addr[0] & 0xf), (u_char) (addr[0] >> 4),
			    blacklists[i]) >= sizeof(query))
				errx(1, "Can't create query, domain too long");
		} else
			errx(1, "Invalid address family received");

		session->query[i].query = gethostbyname_async(query, NULL);
		session->query[i].event = event_asr_run(session->query[i].query,
		    dnsbl_resolve, &(session->query[i]));
		session->query[i].blacklist = i;
		session->query[i].session = session;
		evtimer_set(&(session->query[i].timeout), dnsbl_timeout,
		    &(session->query[i]));
		evtimer_add(&(session->query[i].timeout), &timeout);
	}
}

void
dnsbl_resolve(struct asr_result *result, void *arg)
{
	struct dnsbl_query *query = arg;
	struct dnsbl_session *session = query->session;
	int i, blacklist;

	query->resolved = 1;
	query->event = NULL;
	query->query = NULL;
	evtimer_del(&(query->timeout));
	if (result->ar_hostent != NULL) {
		if (!markspam) {
			smtp_filter_disconnect(session->reqid, session->token,
			    "Host listed at %s", blacklists[query->blacklist]);
			dnsbl_session_free(session);
		} else {
			dnsbl_session_query_done(session);
			session->listed = query->blacklist;
			smtp_filter_proceed(session->reqid, session->token);
		}
		return;
	}
	if (result->ar_h_errno != HOST_NOT_FOUND) {
		smtp_filter_disconnect(session->reqid, session->token,
		    "DNS error on %s", blacklists[query->blacklist]);
		dnsbl_session_free(session);
		return;
	}

	for (i = 0; i < nblacklists; i++) {
		if (!session->query[i].resolved)
			return;
	}
	smtp_filter_proceed(session->reqid, session->token);
	if (!markspam)
		dnsbl_session_free(session);
}

void
dnsbl_timeout(int fd, short event, void *arg)
{
	struct dnsbl_query *query = arg;
	struct dnsbl_session *session = query->session;

	smtp_filter_disconnect(session->reqid, session->token,
	    "DNS timeout on %s", blacklists[query->blacklist]);
	dnsbl_session_free(session);
}

void
dnsbl_disconnect(char *type, int version, struct timespec *tm, char *direction,
    char *phase, uint64_t reqid)
{
	struct dnsbl_session *session, search;

	search.reqid = reqid;
	if ((session = RB_FIND(dnsbl_sessions, &dnsbl_sessions, &search)) != NULL)
		dnsbl_session_free(session);
}

void
dnsbl_dataline(char *type, int version, struct timespec *tm, char *direction,
    char *phase, uint64_t reqid, uint64_t token, char *line)
{
	struct dnsbl_session *session, search;

	search.reqid = reqid;
	session = RB_FIND(dnsbl_sessions, &dnsbl_sessions, &search);

	if (session->listed != -1) {
		smtp_filter_dataline(reqid, token, "X-Spam: yes");
		smtp_filter_dataline(reqid, token, "X-Spam-DNSBL: Listed at %s",
		    blacklists[session->listed]);
		session->listed = -1;
	}
	smtp_filter_dataline(reqid, token, "%s", line);
}

void
dnsbl_session_query_done(struct dnsbl_session *session)
{
	int i;

	for (i = 0; i < nblacklists; i++) {
		if (!session->query[i].resolved) {
			event_asr_abort(session->query[i].event);
			evtimer_del(&(session->query[i].timeout));
		}
	}
}

void
dnsbl_session_free(struct dnsbl_session *session)
{
	RB_REMOVE(dnsbl_sessions, &dnsbl_sessions, session);
	dnsbl_session_query_done(session);
	free(session->query);
	free(session);
}

int
dnsbl_session_cmp(struct dnsbl_session *s1, struct dnsbl_session *s2)
{
	return (s1->reqid < s2->reqid ? -1 : s1->reqid > s2->reqid);
}

__dead void
usage(void)
{
	fprintf(stderr, "usage: %s [-m] blacklist [...]\n",
	    getprogname());
	exit(1);
}

RB_GENERATE(dnsbl_sessions, dnsbl_session, entry, dnsbl_session_cmp);
