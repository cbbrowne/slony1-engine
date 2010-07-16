/*-------------------------------------------------------------------------
 * slon.c
 *
 *	The control framework for the node daemon.
 *
 *	Copyright (c) 2003-2004, PostgreSQL Global Development Group
 *	Author: Jan Wieck, Afilias USA INC.
 *
 *	
 *-------------------------------------------------------------------------
 */


#include <pthread.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/types.h>

#include "libpq-fe.h"
#include "c.h"

#include "slon.h"


/* ----------
 * Global data
 * ----------
 */
int slon_restart_request = false;

pthread_mutex_t		slon_wait_listen_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t		slon_wait_listen_cond = PTHREAD_COND_INITIALIZER;


/* ----------
 * Local data
 * ----------
 */
static pthread_t        local_event_thread;
static pthread_t        local_cleanup_thread;
static pthread_t        local_sync_thread;


static pthread_t		main_thread;
static char *const	   *main_argv;
static void				sigalrmhandler(int signo);


/* ----------
 * main
 * ----------
 */
int
main (int argc, char *const argv[])
{
	char	   *cp1;
	char	   *cp2;
	SlonDString	query;
	PGresult   *res;
	int			i, n;
	PGconn	   *startup_conn;
	int			c;
	int			errors = 0;
	extern int	optind;
	extern char *optarg;
	int			group_size_set = 0;

	while ((c = getopt(argc, argv, "d:s:t:g:hv")) != EOF)
	{
		switch(c)
		{
			case 'd':	slon_log_level = strtol(optarg, NULL, 10);
						if (slon_log_level < 0 || slon_log_level > 4)
						{
							fprintf(stderr, "illegal debug level %d\n",
									slon_log_level);
							errors++;
						}
						slon_log_level += SLON_INFO;
						break;

			case 's':	sync_interval = strtol(optarg, NULL, 10);
						if (sync_interval < 100 || sync_interval > 60000)
						{
							fprintf(stderr, "sync interval must be between 100 and 60000 ms\n");
							errors++;
						}
						else if (!group_size_set)
						{
							sync_group_maxsize = 60000 / sync_interval;
							if (sync_group_maxsize > 100)
								sync_group_maxsize = 100;
						}

						break;

			case 't':	sync_interval_timeout = strtol(optarg, NULL, 10);
						if (sync_interval < 0)
						{
							fprintf(stderr, "sync interval must be >= 0\n");
							errors++;
						}

						break;

			case 'g':	sync_group_maxsize = strtol(optarg, NULL, 10);
						if (sync_group_maxsize < 1 || sync_group_maxsize > 100)
						{
							fprintf(stderr, "sync group size must be between 1 and 100 ms\n");
							errors++;
						}
						group_size_set = 1;
						break;

			case 'h':	errors++;
						break;

			case 'v':	printf("slon version %s\n", SLONY_I_VERSION_STRING);
						exit(0);
						break;

			default:	fprintf(stderr, "unknown option '%c'\n", c);
						errors++;
						break;
		}
	}

	if (argc - optind != 2)
		errors++;

	if (errors != 0)
	{
		fprintf(stderr, "usage: %s [options] clustername conninfo\n", argv[0]);
		fprintf(stderr, "\n");
		fprintf(stderr, "Options:\n");
		fprintf(stderr, "    -d <debuglevel>       verbosity of logging (1..4)\n");
		fprintf(stderr, "    -s <milliseconds>     SYNC check interval (default 10000)\n");
		fprintf(stderr, "    -t <milliseconds>     SYNC interval timeout (default 60000)\n");
		fprintf(stderr, "    -g <num>              maximum SYNC group size (default 6)\n");
		return 1;
	}

	/*
	 * Make sure the sync interval isn't too small.
	 */
	if (sync_interval_timeout != 0 && sync_interval_timeout <= sync_interval)
		sync_interval_timeout = sync_interval * 2;

	/*
	 * Remember the cluster name and build the properly quoted 
	 * namespace identifier
	 */
	slon_pid = getpid();
	rtcfg_cluster_name	= (char *)argv[optind];
	rtcfg_namespace		= malloc(strlen(argv[optind]) * 2 + 4);
	cp2 = rtcfg_namespace;
	*cp2++ = '"';
	*cp2++ = '_';
	for (cp1 = (char *)argv[optind]; *cp1; cp1++)
	{
		if (*cp1 == '"')
			*cp2++ = '"';
		*cp2++ = *cp1;
	}
	*cp2++ = '"';
	*cp2 = '\0';

	slon_log(SLON_CONFIG, "main: slon version %s starting up\n",
			SLONY_I_VERSION_STRING);

	/*
	 * Remember the connection information for the local node.
	 */
	rtcfg_conninfo = (char *)argv[++optind];

	/*
	 * Connect to the local database for reading the initial configuration
	 */
	startup_conn = PQconnectdb(rtcfg_conninfo);
	if (startup_conn == NULL)
	{
		slon_log(SLON_FATAL, "main: PQconnectdb() failed\n");
		slon_exit(-1);
	}
	if (PQstatus(startup_conn) != CONNECTION_OK)
	{
		slon_log(SLON_FATAL, "main: Cannot connect to local database - %s",
				PQerrorMessage(startup_conn));
		PQfinish(startup_conn);
		slon_exit(-1);
	}

	/*
	 * Get our local node ID
	 */
	rtcfg_nodeid = db_getLocalNodeId(startup_conn);
	if (rtcfg_nodeid < 0)
	{
		slon_log(SLON_FATAL, "main: Node is not initialized properly\n");
		slon_exit(-1);
	}
	if (db_checkSchemaVersion(startup_conn) < 0)
	{
		slon_log(SLON_FATAL, "main: Node has wrong Slony-I schema or module version loaded\n");
		slon_exit(-1);
	}
	slon_log(SLON_CONFIG, "main: local node id = %d\n", rtcfg_nodeid);

	/*
	 * Start the event scheduling system
	 */
	if (sched_start_mainloop() < 0)
		slon_exit(-1);

	slon_log(SLON_CONFIG, "main: loading current cluster configuration\n");

	/*
	 * Begin a transaction
	 */
	res = PQexec(startup_conn, 
			"start transaction; "
			"set transaction isolation level serializable;");
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		slon_log(SLON_FATAL, "Cannot start transaction - %s",
				PQresultErrorMessage(res));
		PQclear(res);
		slon_exit(-1);
	}
	PQclear(res);

	/*
	 * Read configuration table sl_node
	 */
	dstring_init(&query);
	slon_mkquery(&query, 
			"select no_id, no_active, no_comment, "
			"    (select coalesce(max(con_seqno),0) from %s.sl_confirm "
			"        where con_origin = no_id and con_received = %d) "
			"        as last_event "
			"from %s.sl_node "
			"order by no_id; ",
			rtcfg_namespace, rtcfg_nodeid, rtcfg_namespace);
	res = PQexec(startup_conn, dstring_data(&query));
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		slon_log(SLON_FATAL, "main: Cannot get node list - %s",
				PQresultErrorMessage(res));
		PQclear(res);
		dstring_free(&query);
		slon_exit(-1);
	}
	for (i = 0, n = PQntuples(res); i < n; i++)
	{
		int		no_id		= (int) strtol(PQgetvalue(res, i, 0), NULL, 10);
		int		no_active	= (*PQgetvalue(res, i, 1) == 't') ? 1 : 0;
		char   *no_comment	= PQgetvalue(res, i, 2);
		int64	last_event;

		if (no_id == rtcfg_nodeid)
		{
			/*
			 * Complete our own local node entry
			 */
			rtcfg_nodeactive  = no_active;
			rtcfg_nodecomment = strdup(no_comment);
		}
		else
		{
			/*
			 * Add a remote node
			 */
			slon_scanint64(PQgetvalue(res, i, 3), &last_event);
			rtcfg_storeNode(no_id, no_comment);
			rtcfg_setNodeLastEvent(no_id, last_event);

			/*
			 * If it is active, remember for activation just before
			 * we start processing events.
			 */
			if (no_active)
				rtcfg_needActivate(no_id);
		}
	}
	PQclear(res);

	/*
	 * Read configuration table sl_path - the interesting pieces
	 */
	slon_mkquery(&query, 
			"select pa_server, pa_conninfo, pa_connretry "
			"from %s.sl_path where pa_client = %d",
			rtcfg_namespace, rtcfg_nodeid);
	res = PQexec(startup_conn, dstring_data(&query));
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		slon_log(SLON_FATAL, "main: Cannot get path config - %s",
				PQresultErrorMessage(res));
		PQclear(res);
		dstring_free(&query);
		slon_exit(-1);
	}
	for (i = 0, n = PQntuples(res); i < n; i++)
	{
		int		pa_server		= (int) strtol(PQgetvalue(res, i, 0), NULL, 10);
		char   *pa_conninfo		= PQgetvalue(res, i, 1);
		int		pa_connretry	= (int) strtol(PQgetvalue(res, i, 2), NULL, 10);

		rtcfg_storePath(pa_server, pa_conninfo, pa_connretry);
	}
	PQclear(res);

	/*
	 * Read configuration table sl_listen - the interesting pieces
	 */
	slon_mkquery(&query, 
			"select li_origin, li_provider "
			"from %s.sl_listen where li_receiver = %d",
			rtcfg_namespace, rtcfg_nodeid);
	res = PQexec(startup_conn, dstring_data(&query));
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		slon_log(SLON_FATAL, "main: Cannot get listen config - %s",
				PQresultErrorMessage(res));
		PQclear(res);
		dstring_free(&query);
		slon_exit(-1);
	}
	for (i = 0, n = PQntuples(res); i < n; i++)
	{
		int		li_origin	= (int) strtol(PQgetvalue(res, i, 0), NULL, 10);
		int		li_provider	= (int) strtol(PQgetvalue(res, i, 1), NULL, 10);

		rtcfg_storeListen(li_origin, li_provider);
	}
	PQclear(res);

	/*
	 * Read configuration table sl_set
	 */
	slon_mkquery(&query, 
			"select set_id, set_origin, set_comment "
			"from %s.sl_set",
			rtcfg_namespace);
	res = PQexec(startup_conn, dstring_data(&query));
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		slon_log(SLON_FATAL, "main: Cannot get set config - %s",
				PQresultErrorMessage(res));
		PQclear(res);
		dstring_free(&query);
		slon_exit(-1);
	}
	for (i = 0, n = PQntuples(res); i < n; i++)
	{
		int		set_id		= (int) strtol(PQgetvalue(res, i, 0), NULL, 10);
		int		set_origin	= (int) strtol(PQgetvalue(res, i, 1), NULL, 10);
		char   *set_comment = PQgetvalue(res, i, 2);

		rtcfg_storeSet(set_id, set_origin, set_comment);
	}
	PQclear(res);

	/*
	 * Read configuration table sl_subscribe - our subscriptions only
	 */
	slon_mkquery(&query, 
			"select sub_set, sub_provider, sub_forward, sub_active "
			"from %s.sl_subscribe "
			"where sub_receiver = %d",
			rtcfg_namespace, rtcfg_nodeid);
	res = PQexec(startup_conn, dstring_data(&query));
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		slon_log(SLON_FATAL, "main: Cannot get subscription config - %s",
				PQresultErrorMessage(res));
		PQclear(res);
		dstring_free(&query);
		slon_exit(-1);
	}
	for (i = 0, n = PQntuples(res); i < n; i++)
	{
		int		sub_set			= (int) strtol(PQgetvalue(res, i, 0), NULL, 10);
		int		sub_provider	= (int) strtol(PQgetvalue(res, i, 1), NULL, 10);
		char   *sub_forward		= PQgetvalue(res, i, 2);
		char   *sub_active		= PQgetvalue(res, i, 3);

		rtcfg_storeSubscribe(sub_set, sub_provider, sub_forward);
		if (*sub_active == 't')
			rtcfg_enableSubscription(sub_set, sub_provider, sub_forward);
	}
	PQclear(res);

	/*
	 * Remember the last known local event sequence
	 */
	slon_mkquery(&query,
			"select coalesce(max(ev_seqno), -1) from %s.sl_event "
			"where ev_origin = '%d'",
			rtcfg_namespace, rtcfg_nodeid);
	res = PQexec(startup_conn, dstring_data(&query));
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		slon_log(SLON_FATAL, "main: Cannot get last local eventid - %s",
				PQresultErrorMessage(res));
		PQclear(res);
		dstring_free(&query);
		slon_exit(-1);
	}
	if (PQntuples(res) == 0)
		strcpy(rtcfg_lastevent, "-1");
	else
		if (PQgetisnull(res, 0, 0))
			strcpy(rtcfg_lastevent, "-1");
		else
			strcpy(rtcfg_lastevent, PQgetvalue(res, 0, 0));
	PQclear(res);
	dstring_free(&query);
	slon_log(SLON_DEBUG2, 
			"main: last local event sequence = %s\n", 
			rtcfg_lastevent);

	/*
	 * Rollback the transaction we used to get the config snapshot
	 */
	res = PQexec(startup_conn, "rollback transaction;");
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		slon_log(SLON_FATAL, "main: Cannot rollback transaction - %s",
				PQresultErrorMessage(res));
		PQclear(res);
		slon_exit(-1);
	}
	PQclear(res);

	/*
	 * Done with the startup, don't need the local connection any more.
	 */
	PQfinish(startup_conn);

	slon_log(SLON_CONFIG, "main: configuration complete - starting threads\n");

	/*
	 * Create the local event thread that is monitoring
	 * the local node for administrative events to adjust the
	 * configuration at runtime.
	 * We wait here until the local listen thread has checked that
	 * there is no other slon daemon running.
	 */
	pthread_mutex_lock(&slon_wait_listen_lock);
	if (pthread_create(&local_event_thread, NULL, localListenThread_main, NULL) < 0)
	{
		slon_log(SLON_FATAL, "main: cannot create localListenThread - %s\n",
				strerror(errno));
		slon_abort();
	}
	pthread_cond_wait(&slon_wait_listen_cond, &slon_wait_listen_lock);
	pthread_mutex_unlock(&slon_wait_listen_lock);

	/*
	 * Enable all nodes that are active
	 */
	rtcfg_doActivate();

	/*
	 * Create the local cleanup thread that will remove old
	 * events and log data.
	 */
	if (pthread_create(&local_cleanup_thread, NULL, cleanupThread_main, NULL) < 0)
	{
		slon_log(SLON_FATAL, "main: cannot create cleanupThread - %s\n",
				strerror(errno));
		slon_abort();
	}

	/*
	 * Create the local sync thread that will generate SYNC
	 * events if we had local database updates.
	 */
	if (pthread_create(&local_sync_thread, NULL, syncThread_main, NULL) < 0)
	{
		slon_log(SLON_FATAL, "main: cannot create syncThread - %s\n",
				strerror(errno));
		slon_abort();
	}

	/*
	 * Wait until the scheduler has shut down all remote connections
	 */
	slon_log(SLON_DEBUG1, "main: running scheduler mainloop\n");
	if (sched_wait_mainloop() < 0)
	{
		slon_log(SLON_FATAL, "main: scheduler returned with error\n");
		slon_abort();
	}
	slon_log(SLON_DEBUG1, "main: scheduler mainloop returned\n");

	/*
	 * Wait for all remote threads to finish
	 */
	main_thread = pthread_self();
	main_argv = argv;
	signal(SIGALRM, sigalrmhandler);
	alarm(20);

	rtcfg_joinAllRemoteThreads();

	alarm(0);

	/*
	 * Wait for the local threads to finish
	 */
	if (pthread_join(local_event_thread, NULL) < 0)
		slon_log(SLON_ERROR, "main: cannot join localListenThread - %s\n",
				strerror(errno));

	if (pthread_join(local_cleanup_thread, NULL) < 0)
		slon_log(SLON_ERROR, "main: cannot join cleanupThread - %s\n",
				strerror(errno));

	if (pthread_join(local_sync_thread, NULL) < 0)
		slon_log(SLON_ERROR, "main: cannot join syncThread - %s\n",
				strerror(errno));

	if (slon_restart_request)
	{
		slon_log(SLON_DEBUG1, "main: restart requested\n");
		execvp(argv[0], argv);
		slon_log(SLON_FATAL,
				"main: cannot restart via execvp(): %s\n", strerror(errno));
		exit(-1);
	}

	/*
	 * That's it.
	 */
	slon_log(SLON_DEBUG1, "main: done\n");
	return 0;
}


void
slon_exit(int code)
{
	exit(code);
}


static void
sigalrmhandler(int signo)
{
	if (main_thread == pthread_self())
	{
		alarm(0);

		slon_log(SLON_WARN, "main: shutdown timeout\n");
		if (slon_restart_request)
		{
			execvp(main_argv[0], main_argv);
			slon_log(SLON_FATAL,
				"main: cannot restart via execvp(): %s\n", strerror(errno));
		}
		exit(-1);
	}

	pthread_kill(main_thread, SIGALRM);
}


/*
 * Local Variables:
 *  tab-width: 4
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 */


