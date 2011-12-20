/* ----------------------------------------------------------------------
 * slony1_funcs.c
 *
 *	  The C functions and triggers portion of Slony-I.
 *
 *	Copyright (c) 2003-2009, PostgreSQL Global Development Group
 *	Author: Jan Wieck, Afilias USA INC.
 *
 *	
 * ----------------------------------------------------------------------
 */

#include "postgres.h"
#ifdef MSVC
#include "config_msvc.h"
#else
#include "config.h"
#endif


#include "avl_tree.c"

#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "parser/keywords.h"
#include "parser/parse_type.h"
#include "executor/spi.h"
#include "libpq/md5.h"
#include "commands/trigger.h"
#include "commands/async.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_type.h"
#include "catalog/namespace.h"
#include "access/xact.h"
#include "access/transam.h"
#include "utils/builtins.h"
#include "utils/elog.h"
#include "utils/guc.h"
#include "utils/rel.h"
#include "utils/relcache.h"
#include "utils/lsyscache.h"
#include "utils/hsearch.h"
#ifdef HAVE_GETACTIVESNAPSHOT
#include "utils/snapmgr.h"
#endif
#ifdef HAVE_TYPCACHE
#include "utils/typcache.h"
#else
#include "parser/parse_oper.h"
#endif
#include "mb/pg_wchar.h"

#include <signal.h>
#include <errno.h>
/*@+matchanyintegral@*/
/*@-compmempass@*/
/*@-immediatetrans@*/

#ifdef PG_MODULE_MAGIC
PG_MODULE_MAGIC;
#endif

PG_FUNCTION_INFO_V1(_Slony_I_createEvent);
PG_FUNCTION_INFO_V1(_Slony_I_getLocalNodeId);
PG_FUNCTION_INFO_V1(_Slony_I_getModuleVersion);

PG_FUNCTION_INFO_V1(_Slony_I_logTrigger);
PG_FUNCTION_INFO_V1(_Slony_I_denyAccess);
PG_FUNCTION_INFO_V1(_Slony_I_logApply);
PG_FUNCTION_INFO_V1(_Slony_I_lockedSet);
PG_FUNCTION_INFO_V1(_Slony_I_killBackend);
PG_FUNCTION_INFO_V1(_Slony_I_seqtrack);

PG_FUNCTION_INFO_V1(_slon_quote_ident);
PG_FUNCTION_INFO_V1(_Slony_I_resetSession);
PG_FUNCTION_INFO_V1(_slon_decode_tgargs);

Datum		_Slony_I_createEvent(PG_FUNCTION_ARGS);
Datum		_Slony_I_getLocalNodeId(PG_FUNCTION_ARGS);
Datum		_Slony_I_getModuleVersion(PG_FUNCTION_ARGS);

Datum		_Slony_I_logTrigger(PG_FUNCTION_ARGS);
Datum		_Slony_I_denyAccess(PG_FUNCTION_ARGS);
Datum		_Slony_I_logApply(PG_FUNCTION_ARGS);
Datum		_Slony_I_lockedSet(PG_FUNCTION_ARGS);
Datum		_Slony_I_killBackend(PG_FUNCTION_ARGS);
Datum		_Slony_I_seqtrack(PG_FUNCTION_ARGS);

Datum		_slon_quote_ident(PG_FUNCTION_ARGS);
Datum		_slon_decode_tgargs(PG_FUNCTION_ARGS);

Datum		_Slony_I_resetSession(PG_FUNCTION_ARGS);

#ifdef CYGWIN
extern DLLIMPORT Node *newNodeMacroHolder;
#endif

#define PLAN_NONE			0
#define PLAN_INSERT_EVENT	(1 << 1)
#define PLAN_INSERT_LOG_STATUS (1 << 2)
#define PLAN_APPLY_QUERIES	(1 << 3)


/* ----
 * Slony_I_ClusterStatus -
 *
 *	The per-cluster data to hold for functions and triggers.
 * ----
 */
typedef struct slony_I_cluster_status
{
	NameData	clustername;
	char	   *clusterident;
	int32		localNodeId;
	TransactionId currentXid;
	void	   *plan_active_log;

	int			have_plan;
	void	   *plan_insert_event;
	void	   *plan_insert_log_1;
	void	   *plan_insert_log_2;
	void	   *plan_insert_log_script;
	void	   *plan_record_sequences;
	void	   *plan_get_logstatus;
	void	   *plan_table_info;

	text	   *cmdtype_I;
	text	   *cmdtype_U;
	text	   *cmdtype_D;

	struct slony_I_cluster_status *next;
} Slony_I_ClusterStatus;


typedef struct apply_cache_entry
{
	char		key[16];

	void	   *plan;
	bool		forward;
	struct apply_cache_entry *prev;
	struct apply_cache_entry *next;
	struct apply_cache_entry *self;
} ApplyCacheEntry;


static HTAB				   *applyCacheHash = NULL;
static ApplyCacheEntry	   *applyCacheHead = NULL;
static ApplyCacheEntry	   *applyCacheTail = NULL;
static int					applyCacheSize = 100;
static int					applyCacheUsed = 0;




/*@null@*/
static Slony_I_ClusterStatus *clusterStatusList = NULL;
static Slony_I_ClusterStatus *
getClusterStatus(Name cluster_name,
				 int need_plan_mask);
static const char *slon_quote_identifier(const char *ident);
static int prepareLogPlan(Slony_I_ClusterStatus * cs,
					   int log_status);

Datum
_Slony_I_createEvent(PG_FUNCTION_ARGS)
{
	TransactionId newXid = GetTopTransactionId();
	Slony_I_ClusterStatus *cs;
	char	   *ev_type_c;
	Datum		argv[9];
	char		nulls[10];
	size_t		buf_size;
	int			rc;
	int			i;
	int64		retval;
	bool		isnull;

#ifdef HAVE_GETACTIVESNAPSHOT
	if (GetActiveSnapshot() == NULL)
		elog(ERROR, "Slony-I: ActiveSnapshot is NULL in createEvent()");
#else
	if (SerializableSnapshot == NULL)
		elog(ERROR, "Slony-I: SerializableSnapshot is NULL in createEvent()");
#endif

	if ((rc = SPI_connect()) < 0)
		elog(ERROR, "Slony-I: SPI_connect() failed in createEvent()");

	/*
	 * Get or create the cluster status information and make sure it has the
	 * SPI plans that we need here.
	 */
	cs = getClusterStatus(PG_GETARG_NAME(0),
						  PLAN_INSERT_EVENT);

	buf_size = 8192;

	/*
	 * Do the following only once per transaction.
	 */
	if (!TransactionIdEquals(cs->currentXid, newXid))
	{
		cs->currentXid = newXid;
	}

	/*
	 * Call the saved INSERT plan
	 */
	for (i = 1; i < 10; i++)
	{
		if (i >= PG_NARGS() || PG_ARGISNULL(i))
		{
			argv[i - 1] = (Datum) 0;
			nulls[i - 1] = 'n';
		}
		else
		{
			argv[i - 1] = PG_GETARG_DATUM(i);
			nulls[i - 1] = ' ';
		}
	}
	nulls[9] = '\0';

	if ((rc = SPI_execp(cs->plan_insert_event, argv, nulls, 0)) < 0)
		elog(ERROR, "Slony-I: SPI_execp() failed for \"INSERT INTO sl_event ...\"");

	/*
	 * The INSERT plan also contains a SELECT currval('sl_event_seq'), use the
	 * new sequence number as return value.
	 */
	if (SPI_processed != 1)
		elog(ERROR, "Slony-I: INSERT plan did not return 1 result row");
	retval = DatumGetInt64(SPI_getbinval(SPI_tuptable->vals[0],
										 SPI_tuptable->tupdesc, 1, &isnull));

	/*
	 * For SYNC and ENABLE_SUBSCRIPTION events, we also remember all current
	 * sequence values.
	 */
	if (PG_NARGS() > 1 && !PG_ARGISNULL(1))
	{
		ev_type_c = DatumGetPointer(DirectFunctionCall1(
											   textout, PG_GETARG_DATUM(1)));
		if (strcmp(ev_type_c, "SYNC") == 0 ||
			strcmp(ev_type_c, "ENABLE_SUBSCRIPTION") == 0)
		{
/*@-nullpass@*/
			if ((rc = SPI_execp(cs->plan_record_sequences, NULL, NULL, 0)) < 0)
				elog(ERROR, "Slony-I: SPI_execp() failed for \"INSERT INTO sl_seqlog ...\"");
/*@+nullpass@*/
		}
	}

	(void) SPI_finish();
/*@-mustfreefresh@*/
	PG_RETURN_INT64(retval);
}

/*@+mustfreefresh@*/


/*
 * _Slony_I_getLocalNodeId -
 *
 *	  SQL callable wrapper for calling getLocalNodeId() in order
 *	  to get the current setting of sequence sl_local_node_id with
 *	  configuration check.
 *
 */
Datum
_Slony_I_getLocalNodeId(PG_FUNCTION_ARGS)
{
	Slony_I_ClusterStatus *cs;
	int			rc;

	if ((rc = SPI_connect()) < 0)
		elog(ERROR, "Slony-I: SPI_connect() failed in getLocalNodeId()");

	cs = getClusterStatus(PG_GETARG_NAME(0), PLAN_NONE);

	SPI_finish();

	PG_RETURN_INT32(cs->localNodeId);
}


/*
 * _Slony_I_getModuleVersion -
 *
 *	  SQL callable function to determine the version number
 *	  of this shared object during the startup checks.
 *
 */
Datum
_Slony_I_getModuleVersion(PG_FUNCTION_ARGS)
{
	text	   *retval;
	int			len;

	len = strlen(SLONY_I_VERSION_STRING);
	retval = palloc(VARHDRSZ + len);

	SET_VARSIZE(retval, VARHDRSZ + len);
	memcpy(VARDATA(retval), SLONY_I_VERSION_STRING, len);

	PG_RETURN_TEXT_P(retval);
}


Datum
_Slony_I_logTrigger(PG_FUNCTION_ARGS)
{
	TransactionId newXid = GetTopTransactionId();
	Slony_I_ClusterStatus *cs;
	TriggerData *tg;
	Datum		argv[6];
	text	   *cmdtype = NULL;
	int32		cmdupdncols = 0;
	int			rc;
	Name		cluster_name;
	int32		tab_id;
	char	   *attkind;
	int			attkind_idx;

	char	   *olddatestyle = NULL;
	Datum	   *cmdargs = NULL;
	Datum	   *cmdargselem = NULL;
	bool	   *cmdnulls = NULL;
	bool	   *cmdnullselem = NULL;
	int			cmddims[1];
	int			cmdlbs[1];

	/*
	 * Don't do any logging if the current session role isn't Origin.
	 */
	if (SessionReplicationRole != SESSION_REPLICATION_ROLE_ORIGIN)
		return PointerGetDatum(NULL);

	/*
	 * Get the trigger call context
	 */
	if (!CALLED_AS_TRIGGER(fcinfo))
		elog(ERROR, "Slony-I: logTrigger() not called as trigger");
	tg = (TriggerData *) (fcinfo->context);

	/*
	 * Check all logTrigger() calling conventions
	 */
	if (!TRIGGER_FIRED_AFTER(tg->tg_event))
		elog(ERROR, "Slony-I: logTrigger() must be fired AFTER");
	if (!TRIGGER_FIRED_FOR_ROW(tg->tg_event))
		elog(ERROR, "Slony-I: logTrigger() must be fired FOR EACH ROW");
	if (tg->tg_trigger->tgnargs != 3)
		elog(ERROR, "Slony-I: logTrigger() must be defined with 3 args");

	/*
	 * Connect to the SPI manager
	 */
	if ((rc = SPI_connect()) < 0)
		elog(ERROR, "Slony-I: SPI_connect() failed in logTrigger()");

	/*
	 * Get all the trigger arguments
	 */
	cluster_name = DatumGetName(DirectFunctionCall1(namein,
								CStringGetDatum(tg->tg_trigger->tgargs[0])));
	tab_id = strtol(tg->tg_trigger->tgargs[1], NULL, 10);
	attkind = tg->tg_trigger->tgargs[2];

	/*
	 * Get or create the cluster status information and make sure it has the
	 * SPI plans that we need here.
	 */
	cs = getClusterStatus(cluster_name, PLAN_INSERT_LOG_STATUS);

	/*
	 * Do the following only once per transaction.
	 */
	if (!TransactionIdEquals(cs->currentXid, newXid))
	{
		int32		log_status;
		bool isnull;

		/*
		 * Determine the currently active log table
		 */
		if (SPI_execp(cs->plan_get_logstatus, NULL, NULL, 0) < 0)
			elog(ERROR, "Slony-I: cannot determine log status");
		if (SPI_processed != 1)
			elog(ERROR, "Slony-I: cannot determine log status");

		log_status = DatumGetInt32(SPI_getbinval(SPI_tuptable->vals[0],
											SPI_tuptable->tupdesc, 1, &isnull));
		SPI_freetuptable(SPI_tuptable);
		prepareLogPlan(cs,log_status);
		switch (log_status)
		{
			case 0:
			case 2:
				cs->plan_active_log = cs->plan_insert_log_1;
				break;

			case 1:
			case 3:
				cs->plan_active_log = cs->plan_insert_log_2;
				break;

			default:
				elog(ERROR, "Slony-I: illegal log status %d", log_status);
				break;
		}

		cs->currentXid = newXid;
	}

	/*
	 * Save the current datestyle setting and switch to ISO (if not already)
	 */
	olddatestyle = GetConfigOptionByName("DateStyle", NULL);
	if (!strstr(olddatestyle, "ISO"))
		set_config_option("DateStyle", "ISO", PGC_USERSET, PGC_S_SESSION, 
				true, true);


	/*
	 * Determine cmdtype and cmdargs depending on the command type
	 */
	if (TRIGGER_FIRED_BY_INSERT(tg->tg_event))
	{
		HeapTuple	new_row = tg->tg_trigtuple;
		TupleDesc	tupdesc = tg->tg_relation->rd_att;
		char	   *col_value;

		int			i;

		/*
		 * INSERT
		 *
		 * cmdtype = 'I' cmdargs = colname, newval [, ...]
		 */
		cmdtype = cs->cmdtype_I;

		cmdargselem = cmdargs = (Datum *)palloc(sizeof(Datum) * 
				((tg->tg_relation->rd_att->natts * 2) + 2));
		cmdnullselem = cmdnulls = (bool *)palloc(sizeof(bool) *
				((tg->tg_relation->rd_att->natts * 2) + 2));

		/*
		 * Specify all the columns
		 */
		for (i = 0; i < tg->tg_relation->rd_att->natts; i++)
		{
			/*
			 * Skip dropped columns
			 */
			if (tupdesc->attrs[i]->attisdropped)
				continue;

			/*
			 * Add the column name
			 */
			*cmdargselem++ = DirectFunctionCall1(textin,
					CStringGetDatum(SPI_fname(tupdesc, i + 1)));
			*cmdnullselem++ = false;

			/*
			 * Add the column value
			 */
			if ((col_value = SPI_getvalue(new_row, tupdesc, i + 1)) == NULL)
			{
				*cmdnullselem++ = true;
				cmdargselem++;
			}
			else
			{
				*cmdargselem++ = DirectFunctionCall1(textin, 
						CStringGetDatum(col_value));
				*cmdnullselem++ = false;
			}
		}

	}
	else if (TRIGGER_FIRED_BY_UPDATE(tg->tg_event))
	{
		HeapTuple	old_row = tg->tg_trigtuple;
		HeapTuple	new_row = tg->tg_newtuple;
		TupleDesc	tupdesc = tg->tg_relation->rd_att;
		Datum		old_value;
		Datum		new_value;
		bool		old_isnull;
		bool		new_isnull;

		char	   *col_ident;
		char	   *col_value;
		int			i;

		/*
		 * UPDATE
		 *
		 * cmdtype = 'U' cmdargs = pkcolname, oldval [, ...]
		 *						   colname, newval [, ...]
		 */
		cmdtype = cs->cmdtype_U;

		cmdargselem = cmdargs = (Datum *)palloc(sizeof(Datum) * 
				((tg->tg_relation->rd_att->natts * 4) + 3));
		cmdnullselem = cmdnulls = (bool *)palloc(sizeof(bool) *
				((tg->tg_relation->rd_att->natts * 4) + 3));

		/*
		 * For all changed columns, add name+value pairs and count them.
		 */
		for (i = 0; i < tg->tg_relation->rd_att->natts; i++)
		{
			/*
			 * Ignore dropped columns
			 */
			if (tupdesc->attrs[i]->attisdropped)
				continue;

			old_value = SPI_getbinval(old_row, tupdesc, i + 1, &old_isnull);
			new_value = SPI_getbinval(new_row, tupdesc, i + 1, &new_isnull);

			/*
			 * If old and new value are NULL, the column is unchanged
			 */
			if (old_isnull && new_isnull)
				continue;

			/*
			 * If both are NOT NULL, we need to compare the values and skip
			 * setting the column if equal
			 */
			if (!old_isnull && !new_isnull)
			{
				Oid			opr_oid;
				FmgrInfo   *opr_finfo_p;

				/*
				 * Lookup the equal operators function call info using the
				 * typecache if available
				 */
#ifdef HAVE_TYPCACHE
				TypeCacheEntry *type_cache;

				type_cache = lookup_type_cache(
											   SPI_gettypeid(tupdesc, i + 1),
								  TYPECACHE_EQ_OPR | TYPECACHE_EQ_OPR_FINFO);
				opr_oid = type_cache->eq_opr;
				if (opr_oid == ARRAY_EQ_OP)
					opr_oid = InvalidOid;
				else
					opr_finfo_p = &(type_cache->eq_opr_finfo);
#else
				FmgrInfo	opr_finfo;

				opr_oid = compatible_oper_funcid(makeList1(makeString("=")),
											   SPI_gettypeid(tupdesc, i + 1),
										SPI_gettypeid(tupdesc, i + 1), true);
				if (OidIsValid(opr_oid))
				{
					fmgr_info(opr_oid, &opr_finfo);
					opr_finfo_p = &opr_finfo;
				}
#endif

				/*
				 * If we have an equal operator, use that to do binary
				 * comparision. Else get the string representation of both
				 * attributes and do string comparision.
				 */
				if (OidIsValid(opr_oid))
				{
					if (DatumGetBool(FunctionCall2(opr_finfo_p,
												   old_value, new_value)))
						continue;
				}
				else
				{
					char	   *old_strval = SPI_getvalue(old_row, tupdesc, i + 1);
					char	   *new_strval = SPI_getvalue(new_row, tupdesc, i + 1);

					if (strcmp(old_strval, new_strval) == 0)
						continue;
				}
			}

			*cmdargselem++ = DirectFunctionCall1(textin,
					CStringGetDatum(SPI_fname(tupdesc, i + 1)));
			*cmdnullselem++ = false;
			if (new_isnull) 
			{
				*cmdnullselem++ = true;
				cmdargselem++;
			}
			else
			{
				*cmdargselem++ = DirectFunctionCall1(textin,
						CStringGetDatum(SPI_getvalue(new_row, tupdesc, i + 1)));
				*cmdnullselem++ = false;
			}
			cmdupdncols++;
		}

		/*
		 * Add pairs of PK column names and values
		 */
		for (i = 0, attkind_idx = -1; i < tg->tg_relation->rd_att->natts; i++)
		{
			/*
			 * Ignore dropped columns
			 */
			if (tupdesc->attrs[i]->attisdropped)
				continue;

			attkind_idx++;
			if (!attkind[attkind_idx])
				break;
			if (attkind[attkind_idx] != 'k')
				continue;
			col_ident = SPI_fname(tupdesc, i + 1);
			col_value = SPI_getvalue(old_row, tupdesc, i + 1);
			if (col_value == NULL)
				elog(ERROR, "Slony-I: old key column %s.%s IS NULL on UPDATE",
					 NameStr(tg->tg_relation->rd_rel->relname), col_ident);

			*cmdargselem++ = DirectFunctionCall1(textin,
					CStringGetDatum(col_ident));
			*cmdnullselem++ = false;

			*cmdargselem++ = DirectFunctionCall1(textin,
					CStringGetDatum(col_value));
			*cmdnullselem++ = false;
		}

	}
	else if (TRIGGER_FIRED_BY_DELETE(tg->tg_event))
	{
		HeapTuple	old_row = tg->tg_trigtuple;
		TupleDesc	tupdesc = tg->tg_relation->rd_att;
		char	   *col_ident;
		char	   *col_value;
		int			i;

		/*
		 * DELETE
		 *
		 * cmdtype = 'D' cmdargs = pkcolname, oldval [, ...]
		 */
		cmdtype = cs->cmdtype_D;

		cmdargselem = cmdargs = (Datum *)palloc(sizeof(Datum) * 
				((tg->tg_relation->rd_att->natts * 2) + 2));
		cmdnullselem = cmdnulls = (bool *)palloc(sizeof(bool) *
				((tg->tg_relation->rd_att->natts * 2) + 2));

		/*
		 * Add the PK columns
		 */
		for (i = 0, attkind_idx = -1; i < tg->tg_relation->rd_att->natts; i++)
		{
			if (tupdesc->attrs[i]->attisdropped)
				continue;

			attkind_idx++;
			if (!attkind[attkind_idx])
				break;
			if (attkind[attkind_idx] != 'k')
				continue;

			*cmdargselem++ = DirectFunctionCall1(textin,
					CStringGetDatum(col_ident = SPI_fname(tupdesc, i + 1)));
			*cmdnullselem++ = false;

			col_value = SPI_getvalue(old_row, tupdesc, i + 1);
			if (col_value == NULL)
				elog(ERROR, "Slony-I: old key column %s.%s IS NULL on DELETE",
					 NameStr(tg->tg_relation->rd_rel->relname), col_ident);
			*cmdargselem++ = DirectFunctionCall1(textin,
					CStringGetDatum(col_value));
			*cmdnullselem++ = false;
		}
	}
	else
		elog(ERROR, "Slony-I: logTrigger() fired for unhandled event");

	/*
	 * Restore the datestyle
	 */
	if (!strstr(olddatestyle, "ISO"))
		set_config_option("DateStyle", olddatestyle, 
				PGC_USERSET, PGC_S_SESSION, true, true);

	/*
	 * Construct the parameter array and insert the log row.
	 */
	cmddims[0] = cmdargselem - cmdargs;
	cmdlbs[0] = 1;

	argv[0] = Int32GetDatum(tab_id);
	argv[1] = DirectFunctionCall1(textin, 
				CStringGetDatum(get_namespace_name(
					RelationGetNamespace(tg->tg_relation))));
	argv[2] = DirectFunctionCall1(textin,
				CStringGetDatum(RelationGetRelationName(tg->tg_relation)));
	argv[3] = PointerGetDatum(cmdtype);
	argv[4] = Int32GetDatum(cmdupdncols);
	argv[5] = PointerGetDatum(construct_md_array(cmdargs, cmdnulls, 1,
			cmddims, cmdlbs, TEXTOID, -1, false, 'i'));

	SPI_execp(cs->plan_active_log, argv, NULL, 0);

	SPI_finish();
	return PointerGetDatum(NULL);
}


Datum
_Slony_I_denyAccess(PG_FUNCTION_ARGS)
{
	TriggerData *tg;

	/*
	 * Get the trigger call context
	 */
	if (!CALLED_AS_TRIGGER(fcinfo))
		elog(ERROR, "Slony-I: denyAccess() not called as trigger");
	tg = (TriggerData *) (fcinfo->context);

	/*
	 * Check all logTrigger() calling conventions
	 */
	if (!TRIGGER_FIRED_BEFORE(tg->tg_event))
		elog(ERROR, "Slony-I: denyAccess() must be fired BEFORE");
	if (!TRIGGER_FIRED_FOR_ROW(tg->tg_event))
		elog(ERROR, "Slony-I: denyAccess() must be fired FOR EACH ROW");
	if (tg->tg_trigger->tgnargs != 1)
		elog(ERROR, "Slony-I: denyAccess() must be defined with 1 arg");

	/*
	 * Connect to the SPI manager
	 */
	if (SPI_connect() < 0)
		elog(ERROR, "Slony-I: SPI_connect() failed in denyAccess()");

	/*
	 * If the replication role is: ORIGIN - default role --> FAIL REPLICA -
	 * this trigger will not fire in --> N/A LOCAL - role when running "local
	 * updates" --> PERMIT UPDATE
	 */
	if (SessionReplicationRole == SESSION_REPLICATION_ROLE_ORIGIN)
		elog(ERROR,
			 "Slony-I: Table %s is replicated and cannot be "
			 "modified on a subscriber node - role=%d",
		  NameStr(tg->tg_relation->rd_rel->relname), SessionReplicationRole);

	SPI_finish();
	if (TRIGGER_FIRED_BY_UPDATE(tg->tg_event))
		return PointerGetDatum(tg->tg_newtuple);
	else
		return PointerGetDatum(tg->tg_trigtuple);
}


Datum
_Slony_I_logApply(PG_FUNCTION_ARGS)
{
	static char *query = NULL;
	static int	query_alloc = 0;

	TransactionId newXid = GetTopTransactionId();
	Slony_I_ClusterStatus *cs;
	TriggerData *tg;
	HeapTuple	new_row;
	TupleDesc	tupdesc;
	Name		cluster_name;
	int			rc;
	bool		isnull;
	Relation	target_rel;

	char		*query_pos;
	Datum		dat;
	char		cmdtype;
	char		*nspname;
	char		*relname;
	int32		cmdupdncols;
	Datum		*cmdargs;
	bool		*cmdargsnulls;
	int			cmdargsn;
	int			querynvals = 0;
	Datum		*queryvals = NULL;
	Oid			*querytypes = NULL;
	char		*querynulls = NULL;
	char		**querycolnames = NULL;
	int			i;
	int			spi_rc;

	ApplyCacheEntry	   *cacheEnt;
	char				cacheKey[16];
	bool				found;

	/*
	 * Get the trigger call context
	 */
	if (!CALLED_AS_TRIGGER(fcinfo))
		elog(ERROR, "Slony-I: logApply() not called as trigger");
	tg = (TriggerData *) (fcinfo->context);

	/*
	 * Don't do any applying if the current session role isn't Replica.
	 */
	if (SessionReplicationRole != SESSION_REPLICATION_ROLE_REPLICA)
		return PointerGetDatum(tg->tg_trigtuple);

	/*
	 * Check all logApply() calling conventions
	 */
	if (!TRIGGER_FIRED_BEFORE(tg->tg_event))
		elog(ERROR, "Slony-I: logApply() must be fired BEFORE");
	if (!TRIGGER_FIRED_FOR_ROW(tg->tg_event))
		elog(ERROR, "Slony-I: logApply() must be fired FOR EACH ROW");
	if (tg->tg_trigger->tgnargs != 1)
		elog(ERROR, "Slony-I: logApply() must be defined with 1 arg");

	/*
	 * Connect to the SPI manager
	 */
	if ((rc = SPI_connect()) < 0)
		elog(ERROR, "Slony-I: SPI_connect() failed in logApply()");

	/*
	 * Get or create the cluster status information and make sure it has the
	 * SPI plans that we need here.
	 */
	cluster_name = DatumGetName(DirectFunctionCall1(namein,
								CStringGetDatum(tg->tg_trigger->tgargs[0])));
	cs = getClusterStatus(cluster_name, PLAN_APPLY_QUERIES);

	/*
	 * Do the following only once per transaction.
	 */
	if (!TransactionIdEquals(cs->currentXid, newXid))
	{
		HASHCTL		hctl;

		for (cacheEnt = applyCacheHead; cacheEnt; cacheEnt = cacheEnt->next)
		{
			if (cacheEnt->plan != NULL)
				SPI_freeplan(cacheEnt->plan);
			cacheEnt->plan = NULL;
		}
		applyCacheHead = NULL;
		applyCacheTail = NULL;
		applyCacheUsed = 0;

		if (applyCacheHash != NULL)
			hash_destroy(applyCacheHash);
		memset(&hctl, 0, sizeof(hctl));
		hctl.keysize = 16;
		hctl.entrysize = sizeof(ApplyCacheEntry);
		applyCacheHash = hash_create("Slony-I apply cache",
					50, &hctl, HASH_ELEM);

		cs->currentXid = newXid;
	}

	/*
	 * Get the cmdtype first.
	 */
	new_row = tg->tg_trigtuple;
	tupdesc = tg->tg_relation->rd_att;

	dat = SPI_getbinval(new_row, tupdesc, 
			SPI_fnumber(tupdesc, "log_cmdtype"), &isnull);
	if (isnull)
		elog(ERROR, "Slony-I: log_cmdtype is NULL");
	cmdtype = DatumGetChar(dat);

	/*
	 * Rows coming from sl_log_script are handled different from
	 * regular data log rows since they don't have all the columns.
	 */
	if (cmdtype == 'S')
	{
		char	   *ddl_script;
		bool		localNodeFound = true;
		Datum		script_insert_args[4];

		dat = SPI_getbinval(new_row, tupdesc, 
				SPI_fnumber(tupdesc, "log_cmdargs"), &isnull);
		if (isnull)
			elog(ERROR, "Slony-I: log_cmdargs is NULL");

		/*
		 * Turn the log_cmdargs into a plain array of Text Datums.
		 */
		deconstruct_array(DatumGetArrayTypeP(dat), 
				TEXTOID, -1, false, 'i', 
				&cmdargs, &cmdargsnulls, &cmdargsn);
		
		/*
		 * The first element is the DDL statement itself.
		 */
		ddl_script = DatumGetCString(DirectFunctionCall1(
						textout, cmdargs[0]));
		
		/*
		 * If there is an optional node ID list, check that we are in it.
		 */
		if (cmdargsn > 1) {
			localNodeFound = false;
			for (i = 1; i < cmdargsn; i++)
			{
				int32	nodeId = DatumGetInt32(
									DirectFunctionCall1(int4in,
									DirectFunctionCall1(textout, cmdargs[i])));
				if (nodeId == cs->localNodeId)
				{
					localNodeFound = true;
					break;
				}
			}
		}

		/*
		 * Execute the DDL statement if the node list is empty or our
		 * local node ID appears in it.
		 */
		if (localNodeFound)
		{
			if (SPI_exec(ddl_script, 0) < 0)
			{
				elog(ERROR, "SPI_exec() failed for DDL statement '%s'",
					ddl_script);
			}

			/*
			 * Set the currentXid to invalid to flush the apply
			 * query cache.
			 */
			cs->currentXid = InvalidTransactionId;
		}

		/*
		 * Build the parameters for the insert into sl_log_script
		 * and execute the query.
		 */
		script_insert_args[0] = SPI_getbinval(new_row, tupdesc, 
				SPI_fnumber(tupdesc, "log_origin"), &isnull);
		script_insert_args[1] = SPI_getbinval(new_row, tupdesc, 
				SPI_fnumber(tupdesc, "log_txid"), &isnull);
		script_insert_args[2] = SPI_getbinval(new_row, tupdesc, 
				SPI_fnumber(tupdesc, "log_actionseq"), &isnull);
		script_insert_args[3] = SPI_getbinval(new_row, tupdesc, 
				SPI_fnumber(tupdesc, "log_cmdargs"), &isnull);
		if (SPI_execp(cs->plan_insert_log_script, script_insert_args, NULL, 0) < 0)
			elog(ERROR, "Execution of sl_log_script insert plan failed");

		/*
		 * Return NULL to suppress the insert into the original sl_log_N.
		 */
		SPI_finish();
		return PointerGetDatum(NULL);
	}

	/*
	 * Normal data log row. Get all the relevant data from the log row.
	 */
	nspname = SPI_getvalue(new_row, tupdesc, 
			SPI_fnumber(tupdesc, "log_tablenspname"));

	relname = SPI_getvalue(new_row, tupdesc,
			SPI_fnumber(tupdesc, "log_tablerelname"));

	dat = SPI_getbinval(new_row, tupdesc, 
			SPI_fnumber(tupdesc, "log_cmdupdncols"), &isnull);
	if (isnull && cmdtype == 'U')
		elog(ERROR, "Slony-I: log_cmdupdncols is NULL on UPDATE");
	cmdupdncols = DatumGetInt32(dat);

	dat = SPI_getbinval(new_row, tupdesc, 
			SPI_fnumber(tupdesc, "log_cmdargs"), &isnull);
	if (isnull)
		elog(ERROR, "Slony-I: log_cmdargs is NULL");

	/*
	 * Turn the log_cmdargs into a plain array of Text Datums.
	 */
	deconstruct_array(DatumGetArrayTypeP(dat), 
			TEXTOID, -1, false, 'i', 
			&cmdargs, &cmdargsnulls, &cmdargsn);

	/*
	 * Find the target relation in the system cache. We need this to
	 * find the data types of the target columns for casting.
	 */
	target_rel = RelationIdGetRelation(
			get_relname_relid(relname, LookupExplicitNamespace(nspname)));
	if (target_rel == NULL)
		elog(ERROR, "Slony-I: cannot find table %s.%s in logApply()",
				slon_quote_identifier(nspname),
				slon_quote_identifier(relname));

	/*
	 * On first call, allocate the query string buffer. 
	 */
    if (query == NULL)
	{
		if ((query = malloc(query_alloc = 8192)) == NULL)
		{
			elog(ERROR, "Slony-I: out of memory in logApply()");
		}
	}
	query_pos = query;

	/*
	 * Handle the log row according to its log_cmdtype
	 */
	switch (cmdtype) 
	{
		case 'I':
			/*
			 * INSERT
			 */
			querycolnames = (char **)palloc(sizeof(char *) * cmdargsn / 2);
			queryvals = (Datum *)palloc(sizeof(Datum) * cmdargsn / 2);
			querytypes = (Oid *)palloc(sizeof(Oid) * cmdargsn / 2);
			querynulls = (char *)palloc(cmdargsn / 2 + 1);

			sprintf(query_pos, "INSERT INTO %s.%s (",
					slon_quote_identifier(nspname),
					slon_quote_identifier(relname));
			query_pos += strlen(query_pos);

			/*
			 * Construct the list of quoted column names.
			 */
			for (i = 0; i < cmdargsn; i += 2)
			{
				char	*colname;

				/*
				 * Double the query buffer if we are running low.
				 */
				if (query_pos - query > query_alloc - 256)
				{
					int		have = query_pos - query;

					query_alloc *= 2;
					query = realloc(query, query_alloc);
					query_pos = query + have;
				}

				if (i > 0)
				{
					strcpy(query_pos, ", ");
					query_pos += 2;
				}

				if (cmdargsnulls[i])
					elog(ERROR, "Slony-I: column name in log_cmdargs is NULL");
				querycolnames[i / 2] = DatumGetCString(DirectFunctionCall1(
								textout, cmdargs[i]));
				colname = (char *)slon_quote_identifier(querycolnames[i / 2]);
				strcpy(query_pos, colname);
				query_pos += strlen(query_pos);
			}

			/* 
			 * Add ") VALUES ("
			 */
			strcpy(query_pos, ") VALUES (");
			query_pos += strlen(query_pos);

			/*
			 * Add $n::<coltype> placeholders for all the values. 
			 * At the same time assemble the Datum array, nulls string
			 * and typeoid array for query planning and execution.
			 */
			for (i = 0; i < cmdargsn; i += 2)
			{
				char *coltype;

				/*
				 * Double the query buffer if we are running low.
				 */
				if (query_pos - query > query_alloc - 256)
				{
					int		have = query_pos - query;

					query_alloc *= 2;
					query = realloc(query, query_alloc);
					query_pos = query + have;
				}

				/*
				 * Lookup the column data type in the target relation.
				 */
				coltype = SPI_gettype(target_rel->rd_att, 
						SPI_fnumber(target_rel->rd_att, querycolnames[i / 2]));
				if (coltype == NULL)
					elog(ERROR, "Slony-I: type lookup for column %s failed in logApply()",
							querycolnames[i / 2]);

				/*
				 * Add the parameter to the query string and the
				 * datum to the query parameter array.
				 */
				sprintf(query_pos, "%s$%d::%s", (i == 0) ? "" : ", ", 
						i / 2 + 1, coltype);
				query_pos += strlen(query_pos);

				queryvals[i / 2] = cmdargs[i + 1];
				if (cmdargsnulls[i + 1])
					querynulls[i / 2] = 'n';
				else
					querynulls[i / 2] = ' ';
				querytypes[i / 2] = TEXTOID;
			}

			/*
			 * Finish the query string and terminate the nulls vector.
			 */
			strcpy(query_pos, ");");
			query_pos += 2;
			querynulls[cmdargsn / 2] = '\0';
			querynvals = cmdargsn / 2;

			break;

		case 'U':
			/*
			 * UPDATE
			 */
			querycolnames = (char **)palloc(sizeof(char *) * cmdargsn / 2);
			queryvals = (Datum *)palloc(sizeof(Datum) * cmdargsn / 2);
			querytypes = (Oid *)palloc(sizeof(Oid) * cmdargsn / 2);
			querynulls = (char *)palloc(cmdargsn / 2 + 1);

			sprintf(query_pos, "UPDATE ONLY %s.%s SET ",
					slon_quote_identifier(nspname),
					slon_quote_identifier(relname));
			query_pos += strlen(query_pos);

			/*
			 * This can all be done in one pass over the cmdargs array.
			 * We just have to switch the behavior slightly between
			 * the SET clause and the WHERE clause.
			 */
			for (i = 0; i < cmdargsn; i += 2)
			{
				char *colname;
				char *coltype;

				/*
				 * Double the query buffer if we are running low.
				 */
				if (query_pos - query > query_alloc - 256)
				{
					int		have = query_pos - query;

					query_alloc *= 2;
					query = realloc(query, query_alloc);
					query_pos = query + have;
				}

				/*
				 * Get the column name and data type.
				 */
				if (cmdargsnulls[i])
					elog(ERROR, "Slony-I: column name in log_cmdargs is NULL");
				colname = DatumGetCString(DirectFunctionCall1(
								textout, cmdargs[i]));
				coltype = SPI_gettype(target_rel->rd_att, 
						SPI_fnumber(target_rel->rd_att, colname));
				if (coltype == NULL)
					elog(ERROR, "Slony-I: type lookup for column %s failed in logApply()",
							colname);

				/*
				 * Special case if there were no columns updated.
				 * We tell it to set the first PK column to itself.
				 */
				if (cmdupdncols == 0)
				{
					sprintf(query_pos, "%s = %s",
							slon_quote_identifier(colname),
							slon_quote_identifier(colname));
					query_pos += strlen(query_pos);
				}

				/*
				 * If we are at the transition point from SET to WHERE,
				 * add the WHERE keyword.
				 */
				if (i == cmdupdncols * 2)
				{
					strcpy(query_pos, " WHERE ");
					query_pos += 7;
				}

				if (i < cmdupdncols * 2)
				{
					/*
					 * This is inside the SET clause.
					 * Add the <colname> = $n::<coltype> separated by
					 * comma.
					 */
					sprintf(query_pos, "%s%s = $%d::%s",
							(i > 0) ? ", " : "",
							slon_quote_identifier(colname),
							i / 2 + 1, coltype);
				}
				else
				{
					/*
					 * This is in the WHERE clause. Same as above but
					 * separated by AND.
					 */
					sprintf(query_pos, "%s%s = $%d::%s", 
							(i > cmdupdncols * 2) ? " AND " : "",
							slon_quote_identifier(colname),
							i / 2 + 1, coltype);
				}
				query_pos += strlen(query_pos);

				queryvals[i / 2] = cmdargs[i + 1];
				if (cmdargsnulls[i + 1])
					querynulls[i / 2] = 'n';
				else
					querynulls[i / 2] = ' ';
				querytypes[i / 2] = TEXTOID;
			}

			strcpy(query_pos, ";");
			query_pos += 1;
			querynulls[cmdargsn / 2] = '\0';
			querynvals = cmdargsn / 2;

			break;

		case 'D':
			/*
			 * DELETE
			 */
			querycolnames = (char **)palloc(sizeof(char *) * cmdargsn / 2);
			queryvals = (Datum *)palloc(sizeof(Datum) * cmdargsn / 2);
			querytypes = (Oid *)palloc(sizeof(Oid) * cmdargsn / 2);
			querynulls = (char *)palloc(cmdargsn / 2 + 1);

			sprintf(query_pos, "DELETE FROM ONLY %s.%s WHERE ",
					slon_quote_identifier(nspname),
					slon_quote_identifier(relname));
			query_pos += strlen(query_pos);

			for (i = 0; i < cmdargsn; i += 2)
			{
				char *colname;
				char *coltype;

				/*
				 * Double the query buffer if we are running low.
				 */
				if (query_pos - query > query_alloc - 256)
				{
					int		have = query_pos - query;

					query_alloc *= 2;
					query = realloc(query, query_alloc);
					query_pos = query + have;
				}

				/*
				 * Add <colname> = $n::<coltype> separated by comma.
				 */
				if (cmdargsnulls[i])
					elog(ERROR, "Slony-I: column name in log_cmdargs is NULL");
				colname = DatumGetCString(DirectFunctionCall1(
								textout, cmdargs[i]));
				coltype = SPI_gettype(target_rel->rd_att, 
						SPI_fnumber(target_rel->rd_att, colname));
				if (coltype == NULL)
					elog(ERROR, "Slony-I: type lookup for column %s failed in logApply()",
							colname);
				sprintf(query_pos, "%s%s = $%d::%s", 
						(i > 0) ? " AND " : "",
						slon_quote_identifier(colname),
						i / 2 + 1, coltype);

				query_pos += strlen(query_pos);

				queryvals[i / 2] = cmdargs[i + 1];
				if (cmdargsnulls[i + 1])
					querynulls[i / 2] = 'n';
				else
					querynulls[i / 2] = ' ';
				querytypes[i / 2] = TEXTOID;
			}

			strcpy(query_pos, ";");
			query_pos += 1;

			querynulls[cmdargsn / 2] = '\0';
			querynvals = cmdargsn / 2;

			break;

		case 'T':
			/*
			 * TRUNCATE
			 */
			queryvals = (Datum *)palloc(sizeof(Datum) * 2);
			querytypes = (Oid *)palloc(sizeof(Oid) * 2);
			querynulls = (char *)palloc(3);

			sprintf(query_pos, "SELECT %s.TruncateOnlyTable("
					"%s.slon_quote_brute($1) || '.' || "
					"%s.slon_quote_brute($2));",
					slon_quote_identifier(NameStr(*cluster_name)),
					slon_quote_identifier(NameStr(*cluster_name)),
					slon_quote_identifier(NameStr(*cluster_name)));

			queryvals[0] = DirectFunctionCall1(textin, CStringGetDatum(nspname));
			queryvals[1] = DirectFunctionCall1(textin, CStringGetDatum(relname));
			querytypes[0] = TEXTOID;
			querytypes[1] = TEXTOID;
			querynulls[0] = ' ';
			querynulls[1] = ' ';
			querynulls[2] = '\0';
			querynvals = 2;

			break;

		default:
			elog(ERROR, "Slony-I: unhandled log cmdtype '%c' in logApply()",
					cmdtype);
			break;
	}

	/*
	 * Close the target relation.
	 */
	RelationClose(target_rel);

	/*
	 * Check the query cache if we have an entry.
	 */
	pg_md5_binary(query, strlen(query), &cacheKey);
	cacheEnt = hash_search(applyCacheHash, &cacheKey, HASH_ENTER, &found);
	if (found)
	{
		/*
		 * We are reusing an existing query plan. Just move it
		 * to the end of the list.
		 */
		if (cacheEnt->self != cacheEnt)
			elog(ERROR, "logApply(): cacheEnt != cacheEnt->self");
		if (cacheEnt != applyCacheTail)
		{
			/*
			 * Remove the entry from the list
			 */
			if (cacheEnt->prev == NULL)
				applyCacheHead = cacheEnt->next;
			else
				cacheEnt->prev->next = cacheEnt->next;
			if (cacheEnt->next == NULL)
				applyCacheTail = cacheEnt->prev;
			else
				cacheEnt->next->prev = cacheEnt->prev;

			/*
			 * Put the entry back at the end of the list.
			 */
			if (applyCacheHead == NULL)
			{
				cacheEnt->prev = NULL;
				cacheEnt->next = NULL;
				applyCacheHead = cacheEnt;
				applyCacheTail = cacheEnt;
			}
			else
			{
				cacheEnt->prev = applyCacheTail;
				cacheEnt->next = NULL;
				applyCacheTail->next = cacheEnt;
				applyCacheTail = cacheEnt;
			}
		}
	}
	else
	{
		Datum	query_args[2];

		/*
		 * Query plan not found in plan cache, need to SPI_prepare() it.
		 */
		cacheEnt->plan	= SPI_saveplan(
				SPI_prepare(query, querynvals, querytypes));
		if (cacheEnt->plan == NULL)
			elog(ERROR, "Slony-I: SPI_prepare() failed for query '%s'", query);

		/*
		 * Add the plan to the double linked LRU list
		 */
		if (applyCacheHead == NULL)
		{
			cacheEnt->prev = NULL;
			cacheEnt->next = NULL;
			applyCacheHead = cacheEnt;
			applyCacheTail = cacheEnt;
		}
		else
		{
			cacheEnt->prev = applyCacheTail;
			cacheEnt->next = NULL;
			applyCacheTail->next = cacheEnt;
			applyCacheTail = cacheEnt;
		}
		cacheEnt->self = cacheEnt;
		applyCacheUsed++;

		/*
		 * If that pushes us over the maximum allowed cached plans,
		 * evict the one that wasn't used the longest.
		 */
		if (applyCacheUsed > applyCacheSize)
		{
			ApplyCacheEntry *evict = applyCacheHead;

			SPI_freeplan(evict->plan);

			if (evict->prev == NULL)
				applyCacheHead = evict->next; 
			else
				evict->prev->next = evict->next;
			if (evict->next == NULL)
				applyCacheTail = evict->prev;
			else
				evict->next->prev = evict->prev;

			hash_search(applyCacheHash, &(evict->key), HASH_REMOVE, &found);
			if (!found)
				elog(ERROR, "Slony-I: cached queries hash entry not found "
						"on evict");
		}

		/*
		 * We also need to determine if this table belongs to a
		 * set, that we are a forwarder of.
		 */
		query_args[0] = SPI_getbinval(new_row, tupdesc, 
			SPI_fnumber(tupdesc, "log_tableid"), &isnull);
		query_args[1] = Int32GetDatum(cs->localNodeId);

		if (SPI_execp(cs->plan_table_info, query_args, NULL, 0) < 0)
			elog(ERROR, "SPI_execp() failed for table forward lookup");

		if (SPI_processed != 1)
			elog(ERROR, "forwarding lookup for table %d failed",
					DatumGetInt32(query_args[1]));

		cacheEnt->forward = DatumGetBool(
				SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc,
					SPI_fnumber(SPI_tuptable->tupdesc, "sub_forward"), &isnull));
	}

	/*
	 * Execute the query.
	 */
	if (cacheEnt->plan == NULL)
		elog(ERROR, "Slony-I: cacheEnt->plan is NULL");
	if ((spi_rc = SPI_execp(cacheEnt->plan, queryvals, querynulls, 0)) < 0)
		elog(ERROR, "Slony-I: SPI_execp() for query '%s' failed - rc=%d",
				query, spi_rc);


	SPI_finish();
	if (cacheEnt->forward)
		return PointerGetDatum(tg->tg_trigtuple);
	else
		return PointerGetDatum(NULL);
}


Datum
_Slony_I_lockedSet(PG_FUNCTION_ARGS)
{
	TriggerData *tg;

	/*
	 * Get the trigger call context
	 */
	if (!CALLED_AS_TRIGGER(fcinfo))
		elog(ERROR, "Slony-I: lockedSet() not called as trigger");
	tg = (TriggerData *) (fcinfo->context);

	/*
	 * Check all logTrigger() calling conventions
	 */
	if (!TRIGGER_FIRED_BEFORE(tg->tg_event))
		elog(ERROR, "Slony-I: denyAccess() must be fired BEFORE");
	if (!TRIGGER_FIRED_FOR_ROW(tg->tg_event))
		elog(ERROR, "Slony-I: denyAccess() must be fired FOR EACH ROW");
	if (tg->tg_trigger->tgnargs != 1)
		elog(ERROR, "Slony-I: denyAccess() must be defined with 1 arg");

	elog(ERROR,
		 "Slony-I: Table %s is currently locked against updates "
		 "because of MOVE_SET operation in progress",
		 NameStr(tg->tg_relation->rd_rel->relname));

	return (Datum) 0;
}


Datum
_Slony_I_killBackend(PG_FUNCTION_ARGS)
{
	int32		pid;
	int32		signo;
	text	   *signame;

	if (!superuser())
		elog(ERROR, "Slony-I: insufficient privilege for killBackend");

	pid = PG_GETARG_INT32(0);
	signame = PG_GETARG_TEXT_P(1);

	if (VARSIZE(signame) == VARHDRSZ + 4 &&
		memcmp(VARDATA(signame), "NULL", 0) == 0)
	{
		signo = 0;
	}
	else if (VARSIZE(signame) == VARHDRSZ + 4 &&
			 memcmp(VARDATA(signame), "TERM", 0) == 0)
	{
		signo = SIGTERM;
	}
	else
	{
		signo = 0;
		elog(ERROR, "Slony-I: unsupported signal");
	}

	if (kill(pid, signo) < 0)
		PG_RETURN_INT32(-1);

	PG_RETURN_INT32(0);
}


typedef struct
{
	int32		seqid;
	int64		seqval;
} SeqTrack_elem;

static int
seqtrack_cmp(void *seq1, void *seq2)
{
	return (((SeqTrack_elem *) seq1)->seqid - ((SeqTrack_elem *) seq2)->seqid);
}

static void
seqtrack_free(void *seq)
{
	free(seq);
}

Datum
_Slony_I_seqtrack(PG_FUNCTION_ARGS)
{
	static AVLtree seqmem = AVL_INITIALIZER(seqtrack_cmp, seqtrack_free);
	AVLnode    *node;
	SeqTrack_elem *elem;
	int32		seqid;
	int64		seqval;

	seqid = PG_GETARG_INT32(0);
	seqval = PG_GETARG_INT64(1);

	/*
	 * Try to insert the sequence id into the AVL tree.
	 */
	if ((node = avl_insert(&seqmem, &seqid)) == NULL)
		elog(ERROR, "Slony-I: unexpected NULL return from avl_insert()");

	if (AVL_DATA(node) == NULL)
	{
		/*
		 * This is a new (not seen before) sequence. Create the element,
		 * remember the current lastval and return it to the caller.
		 */
		elem = (SeqTrack_elem *) malloc(sizeof(SeqTrack_elem));
		elem->seqid = seqid;
		elem->seqval = seqval;
		AVL_SETDATA(node, elem);

		PG_RETURN_INT64(seqval);
	}

	/*
	 * This is a sequence seen before. If the value has changed remember and
	 * return it. If it did not, return NULL.
	 */
	elem = AVL_DATA(node);

	if (elem->seqval == seqval)
		PG_RETURN_NULL();
	else
		elem->seqval = seqval;

	PG_RETURN_INT64(seqval);
}


/*
 * slon_quote_identifier					 - Quote an identifier only if needed
 *
 * When quotes are needed, we palloc the required space; slightly
 * space-wasteful but well worth it for notational simplicity.
 *
 * Version: pgsql/src/backend/utils/adt/ruleutils.c,v 1.188 2005/01/13 17:19:10
 */
static const char *
slon_quote_identifier(const char *ident)
{
	/*
	 * Can avoid quoting if ident starts with a lowercase letter or underscore
	 * and contains only lowercase letters, digits, and underscores, *and* is
	 * not any SQL keyword.  Otherwise, supply quotes.
	 */
	int			nquotes = 0;
	bool		safe;
	const char *ptr;
	char	   *result;
	char	   *optr;

	/*
	 * would like to use <ctype.h> macros here, but they might yield unwanted
	 * locale-specific results...
	 */
	safe = ((ident[0] >= 'a' && ident[0] <= 'z') || ident[0] == '_');

	for (ptr = ident; *ptr; ptr++)
	{
		char		ch = *ptr;

		if ((ch >= 'a' && ch <= 'z') ||
			(ch >= '0' && ch <= '9') ||
			(ch == '_'))
		{
			/* okay */
		}
		else
		{
			safe = false;
			if (ch == '"')
				nquotes++;
		}
	}

	if (safe)
	{
		/*
		 * Check for keyword.  This test is overly strong, since many of the
		 * "keywords" known to the parser are usable as column names, but the
		 * parser doesn't provide any easy way to test for whether an
		 * identifier is safe or not... so be safe not sorry.
		 *
		 * Note: ScanKeywordLookup() does case-insensitive comparison, but
		 * that's fine, since we already know we have all-lower-case.
		 */

#ifdef SCANKEYWORDLOOKUP_1
		if (ScanKeywordLookup(ident) != NULL)
#endif
#ifdef SCANKEYWORDLOOKUP_3		   
			if (ScanKeywordLookup(ident,ScanKeywords,NumScanKeywords) != NULL)
#endif
			safe = false;
	}

	if (safe)
		return ident;			/* no change needed */

	result = (char *) palloc(strlen(ident) + nquotes + 2 + 1);

	optr = result;
	*optr++ = '"';
	for (ptr = ident; *ptr; ptr++)
	{
		char		ch = *ptr;

		if (ch == '"')
			*optr++ = '"';
		*optr++ = ch;
	}
	*optr++ = '"';
	*optr = '\0';

	return result;
}



/*
 * _slon_quote_ident -
 *		  returns a properly quoted identifier
 *
 * Version: pgsql/src/backend/utils/adt/quote.c,v 1.14.4.1 2005/03/21 16:29:31
 */
Datum
_slon_quote_ident(PG_FUNCTION_ARGS)
{
	text	   *t = PG_GETARG_TEXT_P(0);
	text	   *result;
	const char *qstr;
	char	   *str;
	int			len;

	/* We have to convert to a C string to use quote_identifier */
	len = VARSIZE(t) - VARHDRSZ;
	str = (char *) palloc(len + 1);
	memcpy(str, VARDATA(t), len);
	str[len] = '\0';

	qstr = slon_quote_identifier(str);

	len = strlen(qstr);
	result = (text *) palloc(len + VARHDRSZ);
	SET_VARSIZE(result, len + VARHDRSZ);
	memcpy(VARDATA(result), qstr, len);

	PG_RETURN_TEXT_P(result);
}



static Slony_I_ClusterStatus *
getClusterStatus(Name cluster_name, int need_plan_mask)
{
	Slony_I_ClusterStatus *cs;
	int			rc;
	char		query[1024];
	bool		isnull;
	Oid			plan_types[9];
	TypeName   *txid_snapshot_typname;

	/*
	 * Find an existing cs row for this cluster
	 */
	for (cs = clusterStatusList; cs; cs = cs->next)
	{
		if ((bool) DirectFunctionCall2(nameeq,
									   NameGetDatum(&(cs->clustername)),
									   NameGetDatum(cluster_name)) == true)
		{
			/*
			 * Return it if all the requested SPI plans are prepared already.
			 */
			if ((cs->have_plan & need_plan_mask) == need_plan_mask)
				return cs;

			/*
			 * Create more SPI plans below.
			 */
			break;
		}
	}

	if (cs == NULL)
	{
		/*
		 * No existing cs found ... create a new one
		 */
		cs = (Slony_I_ClusterStatus *) malloc(sizeof(Slony_I_ClusterStatus));
		memset(cs, 0, sizeof(Slony_I_ClusterStatus));

		/*
		 * We remember the plain cluster name for fast lookup
		 */
		strncpy(NameStr(cs->clustername), NameStr(*cluster_name), NAMEDATALEN);

		/*
		 * ... and the quoted identifier of it for building queries
		 */
		cs->clusterident = strdup(DatumGetCString(DirectFunctionCall1(textout,
											 DirectFunctionCall1(quote_ident,
																 DirectFunctionCall1(textin, CStringGetDatum(NameStr(*cluster_name)))))));

		/*
		 * Get our local node ID
		 */
		snprintf(query, 1024, "select last_value::int4 from %s.sl_local_node_id",
				 cs->clusterident);
		rc = SPI_exec(query, 0);
		if (rc < 0 || SPI_processed != 1)
			elog(ERROR, "Slony-I: failed to read sl_local_node_id");
		cs->localNodeId = DatumGetInt32(
										SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull));
		SPI_freetuptable(SPI_tuptable);
		if (cs->localNodeId < 0)
			elog(ERROR, "Slony-I: Node is uninitialized - cluster %s", DatumGetCString(cluster_name));

		/*
		 * Initialize the currentXid to invalid
		 */
		cs->currentXid = InvalidTransactionId;

		/*
		 * Insert the new control block into the list
		 */
		cs->next = clusterStatusList;
		clusterStatusList = cs;
	}

	/*
	 * Prepare and save the PLAN_INSERT_EVENT
	 */
	if ((need_plan_mask & PLAN_INSERT_EVENT) != 0 &&
		(cs->have_plan & PLAN_INSERT_EVENT) == 0)
	{
		/*
		 * Lookup the oid of the txid_snapshot type
		 */
		txid_snapshot_typname = makeNode(TypeName);
		txid_snapshot_typname->names =
			lappend(lappend(NIL, makeString("pg_catalog")),
					makeString("txid_snapshot"));

		/*
		 * Create the saved plan. We lock the sl_event table in exclusive mode
		 * in order to ensure that all events are really assigned sequence
		 * numbers in the order they get committed.
		 */
		sprintf(query,
				"INSERT INTO %s.sl_event "
				"(ev_origin, ev_seqno, "
				"ev_timestamp, ev_snapshot, "
				"ev_type, ev_data1, ev_data2, ev_data3, ev_data4, "
				"ev_data5, ev_data6, ev_data7, ev_data8) "
				"VALUES ('%d', nextval('%s.sl_event_seq'), "
				"now(), \"pg_catalog\".txid_current_snapshot(), $1, $2, "
				"$3, $4, $5, $6, $7, $8, $9); "
				"SELECT currval('%s.sl_event_seq');",
				cs->clusterident, cs->localNodeId, cs->clusterident,
				cs->clusterident);
		plan_types[0] = TEXTOID;
		plan_types[1] = TEXTOID;
		plan_types[2] = TEXTOID;
		plan_types[3] = TEXTOID;
		plan_types[4] = TEXTOID;
		plan_types[5] = TEXTOID;
		plan_types[6] = TEXTOID;
		plan_types[7] = TEXTOID;
		plan_types[8] = TEXTOID;

		cs->plan_insert_event = SPI_saveplan(SPI_prepare(query, 9, plan_types));
		if (cs->plan_insert_event == NULL)
			elog(ERROR, "Slony-I: SPI_prepare() failed");

		/*
		 * Also prepare the plan to remember sequence numbers on certain
		 * events.
		 */
		sprintf(query,
				"insert into %s.sl_seqlog "
				"(seql_seqid, seql_origin, seql_ev_seqno, seql_last_value) "
				"select * from ("
			 "select seq_id, %d, currval('%s.sl_event_seq'), seq_last_value "
				"from %s.sl_seqlastvalue "
				"where seq_origin = '%d') as FOO "
				"where NOT %s.seqtrack(seq_id, seq_last_value) IS NULL; ",
				cs->clusterident,
				cs->localNodeId, cs->clusterident,
				cs->clusterident, cs->localNodeId,
				cs->clusterident);

		cs->plan_record_sequences = SPI_saveplan(SPI_prepare(query, 0, NULL));
		if (cs->plan_record_sequences == NULL)
			elog(ERROR, "Slony-I: SPI_prepare() failed");

		cs->have_plan |= PLAN_INSERT_EVENT;
	}

	/*
	 * Prepare and save the PLAN_INSERT_LOG_STATUS
	 */
	if ((need_plan_mask & PLAN_INSERT_LOG_STATUS) != 0 &&
		(cs->have_plan & PLAN_INSERT_LOG_STATUS) == 0)
	{
		/* @-nullderef@ */

		/*
		 * Also create the 3 rather static text values for the log_cmdtype
		 * parameter.
		 */
		cs->cmdtype_I = malloc(VARHDRSZ + 1);
		SET_VARSIZE(cs->cmdtype_I, VARHDRSZ + 1);
		*VARDATA(cs->cmdtype_I) = 'I';
		cs->cmdtype_U = malloc(VARHDRSZ + 1);
		SET_VARSIZE(cs->cmdtype_U, VARHDRSZ + 1);
		*VARDATA(cs->cmdtype_U) = 'U';
		cs->cmdtype_D = malloc(VARHDRSZ + 1);
		SET_VARSIZE(cs->cmdtype_D, VARHDRSZ + 1);
		*VARDATA(cs->cmdtype_D) = 'D';

		/*
		 * And the plan to read the current log_status.
		 */
		sprintf(query, "SELECT last_value::int4 FROM %s.sl_log_status",
				cs->clusterident);
		cs->plan_get_logstatus = SPI_saveplan(SPI_prepare(query, 0, NULL));
		if (cs->plan_get_logstatus == NULL)
			elog(ERROR, "Slony-I: SPI_prepare() failed");

		cs->have_plan |= PLAN_INSERT_LOG_STATUS;
	}

	/*
	 * Prepare and save the PLAN_APPLY_QUERIES
	 */
	if ((need_plan_mask & PLAN_APPLY_QUERIES) != 0 &&
		(cs->have_plan & PLAN_APPLY_QUERIES) == 0)
	{
		/* @-nullderef@ */

		/*
		 * The plan to insert into sl_log_script.
		 */
		sprintf(query, "insert into %s.sl_log_script "
				"(log_origin, log_txid, log_actionseq, log_cmdargs) "
				"values ($1, $2, $3, $4);",
				slon_quote_identifier(NameStr(*cluster_name)));
		plan_types[0] = INT4OID;
		plan_types[1] = INT8OID;
		plan_types[2] = INT8OID;
		plan_types[3] = TEXTARRAYOID;

		cs->plan_insert_log_script = SPI_saveplan(
				SPI_prepare(query, 4, plan_types));
		if (cs->plan_insert_log_script == NULL)
			elog(ERROR, "Slony-I: SPI_prepare() failed");

		/*
		 * The plan to lookup table forwarding info
		 */
		sprintf(query,
				"select sub_forward from "
				" %s.sl_subscribe, %s.sl_table "
				" where tab_id = $1 and tab_set = sub_set "
				" and sub_receiver = $2;",
				slon_quote_identifier(NameStr(*cluster_name)),
				slon_quote_identifier(NameStr(*cluster_name)));
		
		plan_types[0] = INT4OID;
		plan_types[1] = INT4OID;

		cs->plan_table_info = SPI_saveplan(
				SPI_prepare(query, 2, plan_types));
		if (cs->plan_table_info == NULL)
			elog(ERROR, "Slony-I: SPI_prepare() failed");

		cs->have_plan |= PLAN_APPLY_QUERIES;
	}

	return cs;
	/* @+nullderef@ */
}

/**
 * prepare the plan for the curren sl_log_x insert query.
 *
 */

#ifndef TEXTARRAYOID
#define TEXTARRAYOID 1009
#endif

int prepareLogPlan(Slony_I_ClusterStatus * cs,
				int log_status)
{
	char		query[1024];
	Oid			plan_types[9];

	if( (log_status==0 ||
		 log_status==2) &&
		cs->plan_insert_log_1==NULL)
	{

		/*
		 * Create the saved plan's
		 */
		sprintf(query, "INSERT INTO %s.sl_log_1 "
				"(log_origin, log_txid, log_tableid, log_actionseq,"
				" log_tablenspname, log_tablerelname, "
				" log_cmdtype, log_cmdupdncols, log_cmdargs) "
				"VALUES (%d, \"pg_catalog\".txid_current(), $1, "
				"nextval('%s.sl_action_seq'), $2, $3, $4, $5, $6); ",
				cs->clusterident, cs->localNodeId, cs->clusterident);
		plan_types[0] = INT4OID;
		plan_types[1] = TEXTOID;
		plan_types[2] = TEXTOID;
		plan_types[3] = TEXTOID;
		plan_types[4] = INT4OID;
		plan_types[5] = TEXTARRAYOID;

		cs->plan_insert_log_1 = SPI_saveplan(SPI_prepare(query, 6, plan_types));
		if (cs->plan_insert_log_1 == NULL)
			elog(ERROR, "Slony-I: SPI_prepare() failed");
	}
	else if ( (log_status==1 ||
			   log_status==3) &&
			  cs->plan_insert_log_2==NULL)
	{
		sprintf(query, "INSERT INTO %s.sl_log_2 "
				"(log_origin, log_txid, log_tableid, log_actionseq,"
				" log_tablenspname, log_tablerelname, "
				" log_cmdtype, log_cmdupdncols, log_cmdargs) "
				"VALUES (%d, \"pg_catalog\".txid_current(), $1, "
				"nextval('%s.sl_action_seq'), $2, $3, $4, $5, $6); ",
				cs->clusterident, cs->localNodeId, cs->clusterident);
		plan_types[0] = INT4OID;
		plan_types[1] = TEXTOID;
		plan_types[2] = TEXTOID;
		plan_types[3] = TEXTOID;
		plan_types[4] = INT4OID;
		plan_types[5] = TEXTARRAYOID;

		cs->plan_insert_log_2 = SPI_saveplan(SPI_prepare(query, 6, plan_types));
		if (cs->plan_insert_log_2 == NULL)
			elog(ERROR, "Slony-I: SPI_prepare() failed");
	}

	return 0;
}
/* Provide a way to reset the per-session data structure that stores
   the cluster status in the C functions. 

 * This is used to rectify the case where CLONE NODE updates the node
 * ID, but calls to getLocalNodeId() could continue to return the old
 * value.
 */
Datum
_Slony_I_resetSession(PG_FUNCTION_ARGS)
{
  Slony_I_ClusterStatus *cs;
  
  cs = clusterStatusList; 
  while(cs != NULL)
  {
	  Slony_I_ClusterStatus *previous;
	  if(cs->cmdtype_I)
		  free(cs->cmdtype_I);
	  if(cs->cmdtype_D)
		  free(cs->cmdtype_D);
	  if(cs->cmdtype_U)
		  free(cs->cmdtype_D);
	  free(cs->clusterident);
	  if(cs->plan_insert_event)
		  SPI_freeplan(cs->plan_insert_event);
	  if(cs->plan_insert_log_1)
		  SPI_freeplan(cs->plan_insert_log_1);
	  if(cs->plan_insert_log_2)
		  SPI_freeplan(cs->plan_insert_log_2);
	  if(cs->plan_record_sequences)
		  SPI_freeplan(cs->plan_record_sequences);
	  if(cs->plan_get_logstatus)
		  SPI_freeplan(cs->plan_get_logstatus);
	  previous=cs;
	  cs=cs->next;
	  free(previous);


  }
  clusterStatusList=NULL;
  PG_RETURN_NULL();

}

/**
 * A function to decode the tgargs column of pg_trigger
 * and return an array of text objects with each trigger
 * argument.
 */
Datum
_slon_decode_tgargs(PG_FUNCTION_ARGS)
{
	const char * arg;
	size_t elem_size=0;
	ArrayType * out_array;
	int idx;
	bytea	   *t = PG_GETARG_BYTEA_P(0);

	int arg_size = VARSIZE(t)- VARHDRSZ;
	const char * in_args = VARDATA(t);
	int array_size = 0;
	out_array=construct_empty_array(TEXTOID);
	arg=in_args;

	for(idx = 0; idx < arg_size; idx++)
	{
		
		if(in_args[idx ]=='\0')
		{
			text * one_arg = palloc(elem_size+VARHDRSZ);
			SET_VARSIZE(one_arg,elem_size + VARHDRSZ);
			memcpy(VARDATA(one_arg),arg,elem_size);
			out_array = array_set(out_array,
								  1, &array_size,
								  PointerGetDatum(one_arg),
								  false,
								  -1,
								  -1,
								  false , /*typbyval for TEXT*/
								  'i' /*typalign for TEXT */
				);
			elem_size=0;
			array_size++;
			arg=&in_args[idx+1];
		}
		else
		{
			elem_size++;
		}
	}


	PG_RETURN_ARRAYTYPE_P(out_array);
}
	
	
	
/*
 * Local Variables:
 *	tab-width: 4
 *	c-indent-level: 4
 *	c-basic-offset: 4
 * End:
 */
