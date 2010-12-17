/*-------------------------------------------------------------------------
 * monitor_thread.c
 *
 *	Implementation of the thread that manages monitoring
 *
 *	Copyright (c) 2003-2009, PostgreSQL Global Development Group
 *	Author: Christopher Browne, Afilias Canada
 *
 *	
 *-------------------------------------------------------------------------
 */


#include <pthread.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/types.h>

#include "slon.h"

int queue_init ();

/* ---------- 
 * Global variables 
 * ----------
 */
SlonStateQueue *queue_tail, *queue_head;
pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;
int monitor_interval;

/* ---------- 
 * slon_localMonitorThread
 *
 * Monitoring thread that periodically flushes queued-up monitoring requests to database
 * ----------
 */
void *
monitorThread_main(void *dummy)
{
	SlonConn   *conn;
	char		last_actseq_buf[64];
	SlonDString query1;
	SlonDString query2;
	PGconn	   *dbconn;
	PGresult   *res;
	int			timeout_count;
	bool        queue_populated;
	SlonState   state;
	struct tm  *loctime;
	char        timebuf[256];

	slon_log(SLON_INFO,
			 "monitorThread: thread starts\n");

	queue_init();

	/*
	 * Connect to the local database
	 */
	if ((conn = slon_connectdb(rtcfg_conninfo, "local_monitor")) == NULL)
		slon_retry();
	dbconn = conn->dbconn;

	/*
	 * Build the query that starts a transaction and retrieves the last value
	 * from the action sequence.
	 */
	dstring_init(&query1);
	slon_mkquery(&query1,
				 "start transaction;"
				 "set transaction isolation level serializable;");

	/*
	 * Build the query that calls createEvent() for the SYNC
	 */
	dstring_init(&query2);
	slon_mkquery(&query2,
				 "select %s.createEvent('_%s', 'SYNC', NULL);",
				 rtcfg_namespace, rtcfg_cluster_name);

	while (sched_wait_time(conn, SCHED_WAIT_SOCK_READ, monitor_interval) == SCHED_STATUS_OK)
	{
			pthread_mutex_lock(&queue_lock);
			if (queue_head != NULL) {
					pthread_mutex_unlock(&queue_lock);
					
					res = PQexec(dbconn, dstring_data(&query1));
					if (PQresultStatus(res) != PGRES_TUPLES_OK)
					{
							slon_log(SLON_FATAL,
									 "monitorThread: \"%s\" - %s",
									 dstring_data(&query1), PQresultErrorMessage(res));
							PQclear(res);
							slon_retry();
							break;
					}

					/* Now, iterate through queue contents, and dump them all to the database */
					do {
							pthread_mutex_lock(&queue_lock);
							queue_populated = queue_dequeue(&state);
							pthread_mutex_unlock(&queue_lock);

							if (queue_populated) {
									dstring_init(&query2);
									slon_mkquery(&query2,
												 "select %s.component_state('%s', %d, %d,", 
												 rtcfg_namespace,
												 state.actor, state.pid, state.node);
									if (state.conn_pid > 0) {
											slon_appendquery(&query2, "%d, ", state.conn_pid);
									} else {
											slon_appendquery(&query2, "NULL::integer, ");
									}
									if (strlen(state.activity) > 0) {
											slon_appendquery(&query2, "'%s', ", state.activity);
									} else {
											slon_appendquery(&query2, "NULL::text, ");
									}
									loctime = localtime(&(state.start_time));
									strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S%z", loctime);
									slon_appendquery(&query2, "'%s', ", timebuf);
									if (state.event > 0) {
											slon_appendquery(&query2, "%ld, ", state.event);
									} else {
											slon_appendquery(&query2, "NULL::bigint, ");
									}
									if (strlen(state.activity) > 0) {
											slon_appendquery(&query2, "'%s');", state.event_type);
									} else {
											slon_appendquery(&query2, "NULL::text);");
									}
									res = PQexec(dbconn, dstring_data(&query2));
									if (PQresultStatus(res) != PGRES_COMMAND_OK)
									{
											slon_log(SLON_FATAL,
													 "monitorThread: \"%s\" - %s",
													 dstring_data(&query2), PQresultErrorMessage(res));
											PQclear(res);
											slon_retry();
											break;
									}
									
							}
					} while (queue_populated);

					/*
					 * Commit the transaction
					 */
					res = PQexec(dbconn, "commit transaction;");
					if (PQresultStatus(res) != PGRES_COMMAND_OK)
					{
					slon_log(SLON_FATAL,
							 "monitorThread: \"commit transaction;\" - %s",
							 PQresultErrorMessage(res));
					PQclear(res);
					slon_retry();
					}
					PQclear(res);
					
			} else {
					slon_log(SLON_DEBUG2, "monitorThread: awoke - nothing in queue to process\n");
					pthread_mutex_unlock(&queue_lock);
			}
	}

	dstring_free(&query1);
	dstring_free(&query2);
	slon_disconnectdb(conn);

	slon_log(SLON_INFO, "monitorThread: thread done\n");
	pthread_exit(NULL);
}

int queue_init ()
{
		if (queue_tail != NULL) {
				slon_log(SLON_FATAL, "monitorThread: trying to initialize queue when non-empty!\n");
				pthread_exit(NULL);
		} else {
				slon_log(SLON_DEBUG1, "monitorThread: initializing monitoring queue\n");
				queue_tail = NULL;
				queue_head = NULL;
		}
		return 1;
}

int monitor_state (char *actor, int pid, int node, int conn_pid, char *activity, int64 event, char *event_type) 
{
		SlonStateQueue *queue_current;

		pthread_mutex_lock(&queue_lock);
		queue_current = (SlonStateQueue *) malloc(sizeof(SlonStateQueue));
		queue_current->entry->actor = actor;
		queue_current->entry->pid = pid;
		queue_current->entry->node = node;
		queue_current->entry->conn_pid = conn_pid;
		queue_current->entry->activity = activity;
		queue_current->entry->start_time = time(NULL);
		queue_current->entry->event = event;
		queue_current->entry->event_type = event_type;

		if (queue_tail == NULL) {
				/* Empty queue - becomes a one-entry queue! */
				slon_log(SLON_DEBUG1, "monitorThread: add entry to empty queue\n");
				queue_head = queue_current;
				queue_tail = queue_current;
		} else {
				queue_tail->next = queue_current;
				queue_tail = queue_current;
		}
		pthread_mutex_unlock(&queue_lock);
		return 0;
}

bool queue_dequeue (SlonState *current)
{
		SlonStateQueue *curr;
		pthread_mutex_lock(&queue_lock);
		if (queue_tail != NULL) {
				current = queue_head->entry;
				curr = queue_head;
				queue_head = queue_head->next;
				free(curr);
				pthread_mutex_unlock(&queue_lock);
				return TRUE;
		} else {
				pthread_mutex_unlock(&queue_lock);
				return FALSE;
		}
}

/*
 * Local Variables:
 *	tab-width: 4
 *	c-indent-level: 4
 *	c-basic-offset: 4
 * End:
 */
