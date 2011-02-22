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

static void stack_init (void);
static bool stack_pop (SlonState *current);
static void stack_dump ();

/* ---------- 
 * Global variables 
 * ----------
 */
#define EMPTY_STACK -1
static SlonState *mstack;
static int stack_size = EMPTY_STACK;
static int stack_maxlength = 1;
static pthread_mutex_t stack_lock = PTHREAD_MUTEX_INITIALIZER;
int monitor_interval;

/* ---------- 
 * slon_localMonitorThread
 *
 * Monitoring thread that periodically flushes stackd-up monitoring requests to database
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
  ScheduleStatus rc;

  slon_log(SLON_INFO,
	   "monitorThread: thread starts\n");

  stack_init();

  /*
   * Connect to the local database
   */
  if ((conn = slon_connectdb(rtcfg_conninfo, "local_monitor")) == NULL) {
    slon_retry();
  } else {
    dbconn = conn->dbconn;
    slon_log(SLON_DEBUG2, "monitorThread: setup DB conn\n");

    monitor_state("local_monitor", 0, (pid_t) conn->conn_pid, "thread main loop", 0, "n/a");
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

    while ((rc = (ScheduleStatus) sched_wait_time(conn, SCHED_WAIT_SOCK_READ, monitor_interval) == SCHED_STATUS_OK))
      {
	int qlen;
	pthread_mutex_lock(&stack_lock);   /* lock access to stack size */
	qlen = stack_size;
	pthread_mutex_unlock(&stack_lock);
	if (qlen >= 0) {
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
	    PQclear(res);
	  /* Now, iterate through stack contents, and dump them all to the database */
	  while (stack_pop(&state)) {  /* This implicitly locks stack - unlocks immediately */
	    dstring_init(&monquery);
	    slon_mkquery(&monquery,
			 "select %s.component_state('%s', %d, %d,", 
			 rtcfg_namespace, state.actor, state.pid, state.node);
	    if (state.conn_pid > 0) {
	      slon_appendquery(&monquery, "%d, ", state.conn_pid);
	    } else {
	      slon_appendquery(&monquery, "NULL::integer, ");
	    }
	    if ((state.activity != 0) && strlen(state.activity) > 0) {
	      slon_appendquery(&monquery, "'%s', ", state.activity);
	    } else {
	      slon_appendquery(&monquery, "NULL::text, ");
	    }
	    (void) strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S%z", localtime(&(state.start_time)));
	    slon_appendquery(&monquery, "'%s', ", timebuf);
	    if (state.event > 0) {
	      slon_appendquery(&monquery, "%L, ", state.event);
	    } else {
	      slon_appendquery(&monquery, "NULL::bigint, ");
	    }
	    if ((state.event_type != 0) && strlen(state.event_type) > 0) {
	      slon_appendquery(&monquery, "'%s');", state.event_type);
	    } else {
	      slon_appendquery(&monquery, "NULL::text);");
	    }
	    if (state.actor != NULL)
	      free(state.actor);
	    if (state.activity != NULL)
	      free(state.activity);
	    if (state.event_type != NULL)
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
	   * Delete obsolete component tuples - where the connection PID is dead
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
					
	}
	if ((rc = (ScheduleStatus) sched_msleep(NULL, monitor_interval)) != SCHED_STATUS_OK) {
	  break;
	}
      }
      monitor_state("local_monitor", 0, (pid_t) conn->conn_pid, "just running", 0, "n/a");
  }
  slon_log(SLON_CONFIG, "monitorThread: exit main loop\n");

  dstring_free(&beginquery);
  dstring_free(&delquery);
  dstring_free(&monquery);
  slon_disconnectdb(conn);

  slon_log(SLON_INFO, "monitorThread: thread done\n");
  pthread_exit(NULL);
  return (void *) 0;
}

static void stack_init (void)
{
  stack_maxlength = 20;
  mstack = malloc(sizeof(SlonState) * (stack_maxlength + 1));
  if (mstack == NULL) {
      slon_log(SLON_ERROR, "stack_init() - malloc() failure could not allocate %d stack slots\n", stack_maxlength);
      slon_retry();
  } else {
    slon_log(SLON_DEBUG2, "stack_init() - initialize stack to size %d\n", stack_maxlength);
  }
  stack_size = EMPTY_STACK;
}

void monitor_state (char *actor, int node, pid_t conn_pid, /*@null@*/char *activity, int64 event, /*@null@*/char *event_type) 
{
  size_t len;
  SlonState *tos;
  SlonState *nstack;
  char *ns;
  pid_t mypid;

  mypid = getpid();

  /* slon_log(SLON_DEBUG2, "monitor_state (%s,%d,%d,%d,%s,%ld,%s)\n",  */
  /* 	   actor, pid, node, conn_pid, activity, event, event_type); */

  pthread_mutex_lock(&stack_lock);

  if (mstack == NULL) {
    stack_init();
  }
  if (stack_size >= stack_maxlength) {
    /* Need to reallocate stack */
    slon_log(SLON_DEBUG2, "monitorThread: resize stack - size %d\n", stack_size);
    stack_dump();
    stack_maxlength *= 2;
    slon_log(SLON_DEBUG2, "monitorThread: resize stack - new length %d\n", stack_maxlength);
    nstack=realloc(mstack, (size_t) stack_maxlength + 1);
    if (nstack == NULL) {
      slon_log(SLON_ERROR, "stack_init() - realloc() failure could not allocate %d stack slots\n", stack_maxlength);
      slon_retry();
    } else {
      mstack = nstack;
    }
    slon_log(SLON_DEBUG2, "monitorThread: stack reallocation done\n", stack_maxlength);
    stack_dump();
  }

  /* if actor matches, then we can do an in-place update */
  len = strlen(actor);
  if (stack_size != EMPTY_STACK) {
    tos = mstack+stack_size;
    if (strncmp(actor, tos->actor, len) == 0) {
      if (tos->actor != NULL) {
	free(tos->actor);
      }
      if (tos->activity != NULL) {
	free(tos->activity);
      }
      if (tos->event_type != NULL) {
	free(tos->event_type);
      }
    } else {
      stack_size++;
    } 
  } else {
    stack_size++;
  }
  tos=mstack+stack_size;
  tos->pid = mypid;
  tos->node = node;
  tos->conn_pid = conn_pid;
  tos->event = event;
  tos->start_time = time(NULL);
  if (actor != NULL) {
    len = strlen(actor);
    ns = malloc(sizeof(char)*len+1);
    if (ns) {
      strncpy(ns, actor, len);
      ns[len] = (char) 0;
      tos->actor = ns;
    } else {
      slon_log(SLON_ERROR, "monitor_state - unable to allocate memory for actor (len %d)\n", len);
      slon_retry();
    }
  } else {
    tos->actor = NULL;
  }
  if (activity != NULL) {
    len = strlen(activity);
    ns = malloc(sizeof(char)*len+1);
    if (ns) {
      strncpy(ns, activity, len);
      ns[len] = (char) 0;
      tos->activity = ns;
    } else {
      slon_log(SLON_ERROR, "monitor_state - unable to allocate memory for activity (len %d)\n", len);
      slon_retry();
    }
  } else {
    tos->activity = NULL;
  }
  if (event_type != NULL) {
    len = strlen(event_type);
    ns =  malloc(sizeof(char)*len+1);
    if (ns) {
      strncpy(ns, event_type, len);
      ns[len] = (char) 0;
      tos->event_type = ns;
    } else {
      slon_log(SLON_ERROR, "monitor_state - unable to allocate memory for event_type (len %d)\n", len);
      slon_retry();
    }
  } else {
    tos->event_type = NULL;
  }
  pthread_mutex_unlock(&stack_lock);
}

/* Note that it is the caller's responsibility to free() the contents
   of strings ->actor, ->activity, ->event_type */
static bool stack_pop (/*@out@*/ SlonState *qentry)
{
  SlonState *ce = NULL;
  pthread_mutex_lock(&stack_lock);
  if (stack_size == EMPTY_STACK) {
    pthread_mutex_unlock(&stack_lock);
    return FALSE;
  } else {
    ce = mstack+stack_size;
    qentry->actor = ce->actor;
    qentry->pid = ce->pid;
    qentry->node = ce->node;
    qentry->conn_pid = ce->conn_pid;
    qentry->activity = ce->activity;
    qentry->event = ce->event;
    qentry->event_type = ce->event_type;
    qentry->start_time = ce->start_time;
    /* slon_log(SLON_DEBUG2, "pop (%s,%d,%d,%d,%s,%ld,%s)\n",  */
    /* 	     qentry->actor, qentry->pid, qentry->node, qentry->conn_pid, qentry->activity, qentry->event, qentry->event_type); */

    stack_size--;
    pthread_mutex_unlock(&stack_lock);
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

static void stack_dump () {
  int i;
  SlonState *tos;
  slon_log(SLON_DEBUG2, "monitorThread: stack_dump()\n");
  for (i = 0; i < stack_size; i++) {
    tos = mstack + i;
    slon_log(SLON_DEBUG2, "stack[%d]=%d\n", 
	     i, tos);
    slon_log(SLON_DEBUG2, "pid:%d node:%d connpid:%d event:%lld\n", 
	     tos->pid, tos->node, tos->conn_pid, tos->event);
    slon_log(SLON_DEBUG2, "actor[%s] activity[%s] event_type[%s]\n",
	     tos->actor, tos->activity, tos->event_type); 
  }
  slon_log(SLON_DEBUG2, "monitorThread: stack_dump done\n");
}
