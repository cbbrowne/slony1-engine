/*-------------------------------------------------------------------------
 * cleanup_thread.c
 *
 *	Periodic cleanup of confirm-, event- and log-data.
 *
 *	Copyright (c) 2003-2009, PostgreSQL Global Development Group
 *	Author: Jan Wieck, Afilias USA INC.
 *
 *
 *-------------------------------------------------------------------------
 */


#include <pthread.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#ifndef WIN32
#include <sys/time.h>
#include <unistd.h>
#endif

#include "slon.h"


/* ----------
 * Global data
 * ----------
 */
int			vac_frequency = SLON_VACUUM_FREQUENCY;
char	   *cleanup_interval;

static int	vac_bias = 0;
static unsigned long earliest_xid = 0;
static unsigned long get_earliest_xid(PGconn *dbconn);

/* ----------
 * cleanupThread_main
 *
 * Periodically calls the stored procedure to remove old events and log data and
 * vacuums those tables.
 * ----------
 */
extern SlonNode *rtcfg_node_list_head;

void *
cleanupThread_main( /* @unused@ */ void *dummy)
{
	SlonConn   *conn;
	SlonDString query_baseclean;
	SlonDString query2;
	SlonDString query_pertbl;
	SlonDString query_nodeinfo;

	PGconn	   *dbconn;
	PGresult   *res;
	PGresult   *res2;
	struct timeval tv_start;
	struct timeval tv_end;
	int			t;
	int			vac_count = 0;
	int			vac_enable = SLON_VACUUM_FREQUENCY;
	char	   *vacuum_action;
	int			ntuples;
	bool        p_mem_eq_db;
	SlonNode   *node;
	int         node_id;
	int         tupno;

	slon_log(SLON_CONFIG, "cleanupThread: thread starts\n");

	/*
	 * Want the vacuum time bias to be between 0 and 100 seconds, hence
	 * between 0 and 100000
	 */
	if (vac_bias == 0)
	{
		vac_bias = rand() % (SLON_CLEANUP_SLEEP * 166);
	}
	slon_log(SLON_CONFIG, "cleanupThread: bias = %d\n", vac_bias);

	/*
	 * Connect to the local database
	 */
	if ((conn = slon_connectdb(rtcfg_conninfo, "local_cleanup")) == NULL)
	{
#ifndef WIN32
		(void) kill(getpid(), SIGTERM);
		pthread_exit(NULL);
#else
		exit(0);
#endif
		/* slon_retry(); */
	}

	dbconn = conn->dbconn;

	monitor_state("local_cleanup", 0, conn->conn_pid, "thread main loop", 0, "n/a");

	/*
	 * Build the query string for calling the cleanupEvent() stored procedure
	 */
	dstring_init(&query_baseclean);
	slon_mkquery(&query_baseclean,
				 "begin;"
				 "lock table %s.sl_config_lock;"
				 "select %s.cleanupEvent('%s'::interval);"
				 "commit;",
				 rtcfg_namespace,
				 rtcfg_namespace,
				 cleanup_interval
		);
	dstring_init(&query2);

	/*
	 * Loop until shutdown time arrived
	 *
	 * Note the introduction of vac_bias and an up-to-100s random "fuzz"; this
	 * reduces the likelihood that having multiple slons hitting the same
	 * cluster will run into conflicts due to trying to vacuum common tables *
	 * such as pg_listener concurrently
	 */
	while (sched_wait_time(conn, SCHED_WAIT_SOCK_READ, SLON_CLEANUP_SLEEP * 1000 + vac_bias + (rand() % (SLON_CLEANUP_SLEEP * 166))) == SCHED_STATUS_OK)
	{
		/*
		 * Call the stored procedure cleanupEvent()
		 */
		monitor_state("local_cleanup", 0, conn->conn_pid, "cleanupEvent", 0, "n/a");
		gettimeofday(&tv_start, NULL);
		res = PQexec(dbconn, dstring_data(&query_baseclean));
		if (PQresultStatus(res) != PGRES_COMMAND_OK)
		{
			slon_log(SLON_FATAL,
					 "cleanupThread: \"%s\" - %s",
				  dstring_data(&query_baseclean), PQresultErrorMessage(res));
			PQclear(res);
			slon_retry();
			break;
		}
		PQclear(res);
		gettimeofday(&tv_end, NULL);
		slon_log(SLON_INFO,
				 "cleanupThread: %8.3f seconds for cleanupEvent()\n",
				 TIMEVAL_DIFF(&tv_start, &tv_end));

		/*
		 * Detain the usual suspects (vacuum event and log data)
		 */
		if (vac_frequency != 0)
		{
			vac_enable = vac_frequency;
		}
		if (++vac_count >= vac_enable)
		{
			unsigned long latest_xid;

			vac_count = 0;

			latest_xid = get_earliest_xid(dbconn);
			vacuum_action = "";
			if (earliest_xid == latest_xid)
			{

				slon_log(SLON_INFO,
					"cleanupThread: xid %d still active - analyze instead\n",
						 earliest_xid);
			}
			else
			{
				if (vac_enable == vac_frequency)
				{
					vacuum_action = "vacuum ";
				}
			}
			earliest_xid = latest_xid;

			/*
			 * Build the query string for vacuuming replication runtime data
			 * and event tables
			 */
			gettimeofday(&tv_start, NULL);

			slon_mkquery(&query2, "select nspname, relname from %s.TablesToVacuum();", rtcfg_namespace);
			res = PQexec(dbconn, dstring_data(&query2));

			/*
			 * for each table...  and we should set up the query to return not
			 * only the table name, but also a boolean to support what's in
			 * the SELECT below; that'll nicely simplify this process...
			 */

			if (PQresultStatus(res) != PGRES_TUPLES_OK) /* query error */
			{
				slon_log(SLON_ERROR,
						 "cleanupThread: \"%s\" - %s",
						 dstring_data(&query2), PQresultErrorMessage(res));
			}
			ntuples = PQntuples(res);
			slon_log(SLON_DEBUG1, "cleanupThread: number of tables to clean: %d\n", ntuples);

			monitor_state("local_cleanup", 0, conn->conn_pid, "vacuumTables", 0, "n/a");
			for (t = 0; t < ntuples; t++)
			{
				char	   *tab_nspname = PQgetvalue(res, t, 0);
				char	   *tab_relname = PQgetvalue(res, t, 1);
				ExecStatusType vrc;

				slon_log(SLON_DEBUG1, "cleanupThread: %s analyze \"%s\".%s;\n",
						 vacuum_action, tab_nspname, tab_relname);
				dstring_init(&query_pertbl);
				slon_mkquery(&query_pertbl, "%s analyze \"%s\".%s;",
							 vacuum_action, tab_nspname, tab_relname);
				res2 = PQexec(dbconn, dstring_data(&query_pertbl));
				vrc = PQresultStatus(res2);
				if (vrc == PGRES_FATAL_ERROR)
				{
					slon_log(SLON_ERROR,
							 "cleanupThread: \"%s\" - %s\n",
					dstring_data(&query_pertbl), PQresultErrorMessage(res2));

					/*
					 * slon_retry(); break;
					 */
				}
				else
				{
					if (vrc == PGRES_NONFATAL_ERROR)
					{
						slon_log(SLON_WARN,
								 "cleanupThread: \"%s\" - %s\n",
								 dstring_data(&query_pertbl), PQresultErrorMessage(res2));

					}
				}
				PQclear(res2);
				dstring_reset(&query_pertbl);
			}
			gettimeofday(&tv_end, NULL);
			slon_log(SLON_INFO,
					 "cleanupThread: %8.3f seconds for vacuuming\n",
					 TIMEVAL_DIFF(&tv_start, &tv_end));

			/* Health check: Verify that in-memory structures match with node
			 * database */

			/* 1.  check that sl_node agrees with node list */
			p_mem_eq_db = true;
			dstring_init(&query_nodeinfo);

			/* Walk thru node list on disk, simply looking for existence */
			slon_mkquery(&query_nodeinfo, 
						 "select no_id from %s.sl_node;",
						 rtcfg_namespace, node_id);
			res = PQexec(dbconn, dstring_data(&query_nodeinfo));
			if (PQresultStatus(res) != PGRES_TUPLES_OK) {
				slon_log(SLON_ERROR,
						 "cleanupThread: \"%s\" - %s",
						 dstring_data(&query_nodeinfo), PQresultErrorMessage(res));
			}
			for (tupno = 0; tupno < ntuples; tupno++) 
			{
				node_id = atoi(PQgetvalue(res, tupno, 0));
				if (rtcfg_findNode(node_id) == NULL) {
					slon_log(SLON_WARN, "cleanupThread: node %d in sl_node not found in in-memory node list\n",
							 node_id);
					p_mem_eq_db = false;
				}
			}
			PQclear(res);
			
			/* Walk thru node list in memory... */
			for (node = rtcfg_node_list_head; node ; node = node->next) {
				node_id = node->no_id;
				slon_mkquery(&query_nodeinfo, 
							 "select no_comment from %s.sl_node where no_id = %d;",
							 rtcfg_namespace, node_id);
				res = PQexec(dbconn, dstring_data(&query_nodeinfo));
				if (PQresultStatus(res) != PGRES_TUPLES_OK) {
					slon_log(SLON_ERROR,
							 "cleanupThread: \"%s\" - %s",
							 dstring_data(&query_nodeinfo), PQresultErrorMessage(res));
				}
				if (PQntuples(res) != 1) {
					slon_log(SLON_WARN, "cleanupThread: in-memory node %d not found in sl_node\n",
							 node_id);
					p_mem_eq_db = false;
				} else {
					if (strcmp(node->no_comment, PQgetvalue(res, 0, 0))) {
						slon_log(SLON_WARN, "cleanupThread: in-memory node %d comment [%s] does not match sl_node comment [%s]\n",
								 node->no_comment, PQgetvalue(res, 0, 0));
						p_mem_eq_db = false;
					}
				}
				PQclear(res);
				dstring_reset(&query_nodeinfo);							 
			}
			dstring_reset(&query_nodeinfo);

/*   Induce logging if you like by setting p_mem_eq_db to "false" */
			/* p_mem_eq_db = false; */

			if (p_mem_eq_db) {
				/* All's OK with the node list */
			} else {
				slon_log(SLON_WARN, "cleanupThread: differences between in-memory node list and sl_node\n");
				slon_log(SLON_WARN, "cleanupThread: dumping sl_node:\n");
				slon_mkquery(&query_nodeinfo, 
							 "select no_id, no_active, no_comment, pa_conninfo, pa_connretry from %s.sl_node, %s.sl_path where pa_server = no_id and pa_client = %d;",
							 rtcfg_namespace, rtcfg_namespace, rtcfg_namespace, rtcfg_nodeid);
				res = PQexec(dbconn, dstring_data(&query_nodeinfo));
				if (PQresultStatus(res) != PGRES_TUPLES_OK) {
					slon_log(SLON_ERROR,
							 "cleanupThread: \"%s\" - %s",
							 dstring_data(&query_nodeinfo), PQresultErrorMessage(res));
				}
				ntuples = PQntuples(res);
				for (tupno = 0; tupno < ntuples; tupno++) 
				{
					slon_log(SLON_WARN,
							 "cleanupThread: sl_node:[%s] active:[%s] comment:[%s] conninfo:[%s] retry interval:[%s]\n",
							 PQgetvalue(res, tupno, 0), 
							 PQgetvalue(res, tupno, 1), 
							 PQgetvalue(res, tupno, 2),
							 PQgetvalue(res, tupno, 3),
							 PQgetvalue(res, tupno, 4));
				}
				PQclear(res);
				for (node = rtcfg_node_list_head; node; node = node->next) {
					if (node != NULL) {
						slon_log(SLON_WARN, 
								 "cleanupThread: in-memory node:[%d] active state:[%d] conn retry interval:[%d] last event received:[%lld] "
								 " comment:[%s] conninfo:[%s] last snapshot:[%s]\n",
								 node->no_id, node->no_active, node->pa_connretry, node->last_event,
								 (node->no_comment==NULL)?"":node->no_comment, 
								 (node->pa_conninfo == NULL)?"":node->pa_conninfo, 
								 (node->last_snapshot == NULL)?"":node->last_snapshot);
					}
				}
			}
			/*
			 * Free Resources
			 */
			dstring_free(&query_nodeinfo);
			dstring_free(&query_pertbl);
			monitor_state("local_cleanup", 0, conn->conn_pid, "thread main loop", 0, "n/a");
		}
	}

	/*
	 * Free Resources
	 */
	dstring_free(&query_baseclean);
	dstring_free(&query2);

	/*
	 * Disconnect from the database
	 */
	slon_disconnectdb(conn);

	/*
	 * Terminate this thread
	 */
	slon_log(SLON_DEBUG1, "cleanupThread: thread done\n");
	pthread_exit(NULL);
}


/* ----------
 * get_earliest_xid()
 *
 * reads the earliest XID that is still active.
 *
 * The idea is that if, between cleanupThread iterations, this XID has
 * not changed, then an old transaction is still in progress,
 * PostgreSQL is holding onto the tuples, and there is no value in
 * doing VACUUMs of the various Slony-I tables.
 * ----------
 */
static unsigned long
get_earliest_xid(PGconn *dbconn)
{
	int64		xid;
	PGresult   *res;
	SlonDString query;

	dstring_init(&query);
	(void) slon_mkquery(&query, "select pg_catalog.txid_snapshot_xmin(pg_catalog.txid_current_snapshot());");
	res = PQexec(dbconn, dstring_data(&query));
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		slon_log(SLON_FATAL, "cleanupThread: could not txid_snapshot_xmin()!\n");
		PQclear(res);
		slon_retry();
		return (unsigned long) -1;
	}
	xid = strtoll(PQgetvalue(res, 0, 0), NULL, 10);
	slon_log(SLON_DEBUG1, "cleanupThread: minxid: %d\n", xid);
	PQclear(res);
	dstring_free(&query);
	return (unsigned long) xid;
}
