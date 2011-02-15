/*-------------------------------------------------------------------------
 * monitor_thread.c
 *
 *	Implementation of the thread that manages monitoring
 *
 *	Copyright (c) 2011, PostgreSQL Global Development Group
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

static void queue_init ();
void monitor_state (char *actor, pid_t pid, int node, int conn_pid, char *activity, int64 event, char *event_type);
static bool queue_dequeue (SlonState *current);

/* ---------- 
 * Global variables 
 * ----------
 */
static SlonStateQueue *queue_tail, *queue_head;
static pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;
static int queue_size;
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
  SlonDString beginquery;
  SlonDString monquery;
  SlonDString delquery;

  PGconn	   *dbconn;
  PGresult   *res;
  SlonState   state;
  char        timebuf[256];
  bool rc;

  slon_log(SLON_INFO,
	   "monitorThread: thread starts\n");

  queue_init();

  /*
   * Connect to the local database
   */
  if ((conn = slon_connectdb(rtcfg_conninfo, "local_monitor")) == NULL) {
    slon_retry();
  } else {
  
    dbconn = conn->dbconn;

    slon_log(SLON_DEBUG2, "monitorThread: setup DB conn\n");
    monitor_state("local_monitor", getpid(), 0, conn->conn_pid, NULL, 0, NULL);

    /*
     * set up queries that are run in each iteration
     */
    dstring_init(&beginquery);
    slon_mkquery(&beginquery,
		 "start transaction;");

    dstring_init(&delquery);
    slon_mkquery(&delquery,
		 "delete from %s.sl_components where co_connection_pid not in (select procpid from pg_catalog.pg_stat_activity);",
		 rtcfg_namespace);

    slon_log(SLON_DEBUG2, "monitorThread: setup start query\n");

    while ((rc = sched_wait_time(conn, SCHED_WAIT_SOCK_READ, monitor_interval) == SCHED_STATUS_OK))
      {
	int qlen = queue_size;
	pthread_mutex_lock(&queue_lock);
	pthread_mutex_unlock(&queue_lock);
      
	if (qlen > 0) {
	  int i = 0;

#define BEGINQUERY "start transaction;"
	  res = PQexec(dbconn, BEGINQUERY);
	  if (PQresultStatus(res) != PGRES_COMMAND_OK) 
	    { 
	      slon_log(SLON_FATAL, 
		       "monitorThread: \"%s\" - %s", 
		       BEGINQUERY, PQresultErrorMessage(res)); 
	      PQclear(res); 
	      slon_retry(); 
	      break; 
	    } 
	  /* Now, iterate through queue contents, and dump them all to the database */
	  while (queue_dequeue(&state)) {
	    slon_log(SLON_DEBUG2, "monitorThread: dequeue %d of %d\n", ++i, qlen);
	    slon_log(SLON_DEBUG2, "queue populated - top = (%s,%d,%d,%d,%s,%ld,%s)\n", 
		     state.actor, state.pid, state.node, state.conn_pid, state.activity, state.event, state.event_type);
	    dstring_init(&monquery);
	    slon_mkquery(&monquery,
			 "select %s.component_state('%s', %d, %d,", 
			 rtcfg_namespace, state.actor, state.pid, state.node);
	    slon_log(SLON_DEBUG2, "monitorThread: attached actor [%s] - pid [%d], node [%d]\n", state.actor, state.pid, state.node);
	    if (state.conn_pid > 0) {
	      slon_appendquery(&monquery, "%d, ", state.conn_pid);
	    } else {
	      slon_appendquery(&monquery, "NULL::integer, ");
	    }
	    slon_log(SLON_DEBUG2, "monitorThread: attached conn_pid [%d]\n", state.conn_pid);
	    if ((state.activity != 0) && strlen(state.activity) > 0) {
	      slon_appendquery(&monquery, "'%s', ", state.activity);
	    } else {
	      slon_appendquery(&monquery, "NULL::text, ");
	    }
	    slon_log(SLON_DEBUG2, "monitorThread: attached activity [%s]\n", state.activity);
	    strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S%z", localtime(&(state.start_time)));
	    slon_appendquery(&monquery, "'%s', ", timebuf);
	    slon_log(SLON_DEBUG2, "monitorThread: attached time\n");
	    if (state.event > 0) {
	      slon_appendquery(&monquery, "%L, ", state.event);
	    } else {
	      slon_appendquery(&monquery, "NULL::bigint, ");
	    }
	    slon_log(SLON_DEBUG2, "monitorThread: attached event- %lld\n", state.event);
	    if ((state.event_type != 0) && strlen(state.event_type) > 0) {
	      slon_appendquery(&monquery, "'%s');", state.event_type);
	    } else {
	      slon_appendquery(&monquery, "NULL::text);");
	    }
	    slon_log(SLON_DEBUG2, "monitorThread: attached event type %s\n", state.event_type);
	    slon_log(SLON_DEBUG2,
		     "monitorThread: query: [%s]\n",
		     dstring_data(&monquery));
	    free(state.actor);
	    free(state.activity);
	    free(state.event_type); 
	    res = PQexec(dbconn, dstring_data(&monquery));
	    if (PQresultStatus(res) != PGRES_TUPLES_OK)
	      {
		slon_log(SLON_FATAL,
			 "monitorThread: \"%s\" - %s",
			 dstring_data(&monquery), PQresultErrorMessage(res));
		PQclear(res);
		slon_retry();
		break;
	      }
	    PQclear(res);
	  }
	  /*
	   * Delete obsolete component tuples
	   */
	  res = PQexec(dbconn, dstring_data(&delquery));
	  if (PQresultStatus(res) != PGRES_COMMAND_OK)
	    {
	      slon_log(SLON_FATAL,
		       "monitorThread: \"%s\" - %s",
		       dstring_data(&delquery), PQresultErrorMessage(res));
	      PQclear(res);
	      slon_retry();
	      break;
	    }
#define COMMITQUERY "commit;"
	  res = PQexec(dbconn, COMMITQUERY);
	  if (PQresultStatus(res) != PGRES_COMMAND_OK) 
	    { 
	      slon_log(SLON_FATAL, 
		       "monitorThread: %s - %s\n", 
		       COMMITQUERY,
		       PQresultErrorMessage(res)); 
	      PQclear(res); 
	      slon_retry(); 
	    } 
	  PQclear(res);
					
	} else {
	  slon_log(SLON_DEBUG2, "monitorThread: awoke - nothing in queue to process\n");
	}
	if ((rc = sched_msleep(0, monitor_interval)) != SCHED_STATUS_OK) {
	  break;
	}
      }

  }
  slon_log(SLON_CONFIG, "monitorThread: exit main loop\n");

  dstring_free(&beginquery);
  dstring_free(&delquery);
  dstring_free(&monquery);
  slon_disconnectdb(conn);

  slon_log(SLON_INFO, "monitorThread: thread done\n");
  pthread_exit(NULL);
}

static void queue_init ()
{
  if (queue_tail != NULL) {
    /* slon_log(SLON_FATAL, "monitorThread: trying to initialize queue when non-empty!\n"); */
    /* pthread_exit(NULL); */
  } 
  slon_log(SLON_DEBUG1, "monitorThread: initializing monitoring queue\n");
  queue_tail = NULL;
  queue_head = NULL;
  queue_size = 0;
}

void monitor_state (char *actor, pid_t pid, int node, pid_t conn_pid, char *activity, int64 event, char *event_type) 
{
  SlonStateQueue *queue_current;
  SlonState *curr;
  int len;
  curr = (SlonState *) malloc(sizeof(SlonState));
  len = strlen(actor);
  curr->actor =  (char *) malloc(sizeof(char) * len);
  strncpy(curr->actor, actor, len);
  curr->actor[len]=0;
  curr->pid = pid;
  curr->node = node;
  curr->conn_pid = conn_pid;
  if (activity != NULL) {
    len = strlen(activity);
    curr->activity = malloc(sizeof(char) * len);
    strncpy(curr->activity, activity, len);
    curr->activity[len] = 0;
  } else {
    curr->activity = activity;
  }
  curr->start_time = time(NULL);
  curr->event = event;
  if (event_type != NULL) {
    len = strlen(event_type);
    curr->event_type = malloc(sizeof(char) * len);
    strncpy(curr->event_type, event_type, len);
    curr->event_type[len] = 0;
  } else {
    curr->event_type = event_type;
  }
  queue_current = (SlonStateQueue *) malloc(sizeof(SlonStateQueue));
  queue_current->entry = curr;
  queue_current->next = NULL;

  pthread_mutex_lock(&queue_lock);
  if (queue_head == NULL) {
    queue_head = queue_current;
  }
  if (queue_tail == NULL) {
    queue_tail = queue_current;
  } else {
    queue_tail->next = queue_current;
    queue_tail = queue_current;
  }
  queue_size++;
  pthread_mutex_unlock(&queue_lock);

  slon_log(SLON_DEBUG2, "monitor_state - size=%d (%s,%d,%d,%d,%s,%ld,%s)\n", 
	   queue_size,
	   curr->actor, curr->pid, curr->node, curr->conn_pid, curr->activity, curr->event, curr->event_type);
}

/* Note that it is the caller's responsibility to free() the contents
   of strings qentry->actor, qentry->activity, qentry->event_type */
bool queue_dequeue (SlonState *qentry)
{
  SlonStateQueue *cq = NULL, *cn;
  SlonState *ce = NULL;
  pthread_mutex_lock(&queue_lock);
  if (queue_head == NULL) {
    slon_log(SLON_DEBUG2, "queue_dequeue()  - NO entry to dequeue\n");
    pthread_mutex_unlock(&queue_lock);
    return FALSE;
  } else {
    ce = queue_head->entry;
    qentry->actor = ce->actor;
    qentry->pid = ce->pid;
    qentry->node = ce->node;
    qentry->conn_pid = ce->conn_pid;
    qentry->activity = ce->activity;
    qentry->event = ce->event;
    qentry->event_type = ce->event_type;
    qentry->start_time = ce->start_time;
    slon_log(SLON_DEBUG2, "queue_dequeue()  - assigned all components to qentry for return\n");
    slon_log(SLON_DEBUG2, "dequeue (%s,%d,%d,%d,%s,%ld,%s)\n", 
	     qentry->actor, qentry->pid, qentry->node, qentry->conn_pid, qentry->activity, qentry->event, qentry->event_type);

    cq = queue_head;
    cn = queue_head->next;
    queue_head = cn;
    free(ce); 
    free(cq); 
    queue_size--;
    pthread_mutex_unlock(&queue_lock);
    return (bool) TRUE;
  }
}

/*
 * Local Variables:
 *	tab-width: 4
 *	c-indent-level: 4
 *	c-basic-offset: 4
 * End:
 */
