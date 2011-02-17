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

/* ---------- 
 * Global variables 
 * ----------
 */
static SlonState *mstack = NULL;
static int stack_size = -1;
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
  bool rc;
  int i;

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
    for (i=0; i< 11; i++) {
      monitor_state("local_monitor", getpid(), 0, (pid_t) conn->conn_pid, (char *)NULL, 0L, (char *)NULL);
    }
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

    while ((rc = sched_wait_time(conn, SCHED_WAIT_SOCK_READ, monitor_interval) == SCHED_STATUS_OK))
      {
	pthread_mutex_lock(&stack_lock);   /* lock access to stack size */
	int qlen = stack_size;
	pthread_mutex_unlock(&stack_lock);
	if (qlen > 0) {
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

	  /* Now, iterate through stack contents, and dump them all to the database */
	  while (stack_pop(&state)) {  /* This implicitly locks stack - unlocks immediately */
	    /* slon_log(SLON_DEBUG2, "stack populated - top = (%s,%d,%d,%d,%s,%ld,%s)\n",
	       state.actor, state.pid, state.node, state.conn_pid, state.activity, state.event, state.event_type); */
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
	    /* slon_log(SLON_DEBUG2, "monitorThread: query: [%s]\n", dstring_data(&monquery)); */
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
					
	}
	if ((rc = sched_msleep(NULL, monitor_interval)) != SCHED_STATUS_OK) {
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

static void stack_init (void)
{
  stack_maxlength = 12;
  mstack = malloc(sizeof(SlonState) * stack_maxlength);
  if (mstack == 0) {
      slon_log(SLON_ERROR, "stack_init() - malloc() failure could not allocate %d stack slots\n", stack_maxlength);
      slon_retry();
  } else {
    slon_log(SLON_DEBUG2, "stack_init() - initialize stack to size %d\n", stack_maxlength);
  }
  stack_size = 0;
}

void monitor_state (char *actor, pid_t pid, int node, pid_t conn_pid, /* @null@ */ char *activity, int64 event,  /* @null@ */ char *event_type) 
{
  size_t i, len;
  SlonState *tstack;
  SlonState *tos;

  slon_log(SLON_DEBUG2, "monitor_state (%s,%d,%d,%d,%s,%ld,%s)\n", 
	   actor, pid, node, conn_pid, activity, event, event_type);

  pthread_mutex_lock(&stack_lock);

  if (mstack == NULL) {
    stack_init();
  }
  if (stack_size+1 > stack_maxlength) {
    /* Need to reallocate stack */
    stack_maxlength *= 2;
    slon_log(SLON_DEBUG2, "monitor_state() - resizing stack to %d\n", stack_maxlength);
    tstack = malloc(sizeof(SlonState) * stack_maxlength);
    if (tstack == 0) {
      slon_log(SLON_ERROR, "monitorThread: malloc() failure in monitor_state() - could not allocate %d stack slots\n", stack_maxlength);
      slon_retry();
    }
    /* Copy mstack into tstack */
    for (i = 0; i < stack_size; i++) {
      tstack[i] = mstack[i];
      tstack[i].actor = mstack[i].actor; 
      tstack[i].activity = mstack[i].activity; 
      tstack[i].event_type = mstack[i].event_type; 
    }
    free(mstack);
    mstack = tstack;   /* and switch */
  }

  /* if actor matches, then we can do an in-place update */
  len = strlen(actor);
  if (stack_size > 0) {
    tos = mstack+stack_size;
    if (strncmp(actor, tos->actor, len) == 0) {
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
  tos->pid = pid;
  tos->node = node;
  tos->conn_pid = conn_pid;
  tos->event = event;
  tos->start_time = time(NULL);
  len = strlen(actor);
  tos->actor = malloc(sizeof(char)*len+1);
  if (tos->actor) {
    strncpy(tos->actor, actor, len);
    tos->actor[len] = (char) 0;
  } else {
    slon_log(SLON_ERROR, "monitor_state - unable to allocate memory for actor (len %d)\n", len);
    slon_retry();
  }
  if (activity != NULL) {
    len = strlen(activity);
    tos->activity = malloc(sizeof(char)*len+1);
    if (tos->activity) {
      strncpy(tos->activity, activity, len);
      tos->activity[len] = (char) 0;
    } else {
      slon_log(SLON_ERROR, "monitor_state - unable to allocate memory for activity (len %d)\n", len);
      slon_retry();
    }
  } else {
    tos->activity = NULL;
  }
  if (event_type != NULL) {
    len = strlen(event_type);
    tos->event_type = malloc(sizeof(char)*len+1);
    if (tos->event_type) {
      strncpy(tos->event_type, event_type, len);
      tos->event_type[len] = (char) 0;
    } else {
      slon_log(SLON_ERROR, "monitor_state - unable to allocate memory for event_type (len %d)\n", len);
      slon_retry();
    }
  } else {
    tos->event_type = NULL;
  }
  slon_log(SLON_DEBUG2, "monitor_state - size=%d (%s,%d,%d,%d,%s,%ld,%s)\n", 
	   stack_size,
	   tos->actor, tos->pid, tos->node, tos->conn_pid, tos->activity, tos->event, tos->event_type);
  pthread_mutex_unlock(&stack_lock);
}

/* Note that it is the caller's responsibility to free() the contents
   of strings ->actor, ->activity, ->event_type */
static bool stack_pop (SlonState *qentry)
{
  SlonState *ce = NULL;
  pthread_mutex_lock(&stack_lock);
  if (stack_size == 0) {
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
    slon_log(SLON_DEBUG2, "pop (%s,%d,%d,%d,%s,%ld,%s)\n", 
	     qentry->actor, qentry->pid, qentry->node, qentry->conn_pid, qentry->activity, qentry->event, qentry->event_type);

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
