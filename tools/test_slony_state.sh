#!/bin/sh

# Christopher Browne
# Copyright 2010-
# PostgreSQL Global Development Group
CLUSTER=$1

function RUNQUERY () {
    local QUERY=$1
    RESULTSET=`psql -qtA -F ":" -R " " -c "${QUERY}"`
    echo ${RESULTSET}
}
function RUNQUERYNODE () {
	local DSN=$1
    local QUERY=$2
    RESULTSET=`psql -qtA -F ":" -R " " -c "${QUERY}" "${DSN}"`
    echo ${RESULTSET}
}
function argn () {
    local V=$1
    local n=$2
    local res=`echo ${V} | cut -d : -f ${n}`
    echo $res
}

function problem () {
	local node=$1
	local subject=$2
	local body=$2

	PROBLEMRECEIVER="cbbrowne@ca.afilias.info"
	echo "${body}" | mail -s "Slony State Test Warning - Cluster ${CLUSTER} - node ${node} - ${subject}" ${PROBLEMRECEIVER}
}

# Rummage for DSNs
NODEQUERY="select no_id from \"_${CLUSTER}\".sl_node;"
NODES=`RUNQUERY "${NODEQUERY}"`

for node in `echo ${NODES}`; do
	CONNQUERY="select pa_conninfo from \"_${CLUSTER}\".sl_path where pa_server = ${node} group by pa_conninfo limit 1;"
	conninfo=`RUNQUERY "${CONNQUERY}"`
	eval CONNINFO${node}=\$conninfo
	echo "Conninfo for node ${node} is: ${conninfo}"
done
	
for node in `echo ${NODES}`; do
	eval DSN=\$CONNINFO${node}
	echo "Tests for node ${node} - DSN=$DSN"
	echo "==============================================================="
	listener_query="select relpages, reltuples from pg_catalog.pg_class where relname = 'pg_listener';"
	LQ=`RUNQUERYNODE "${DSN}" "${listener_query}"`
	pages=`argn "${LQ}" 1`
	tuples=`argn "${LQ}" 2`
	echo "pg_listener info - pages=[$pages] tuples=[$tuples]"

	HILISTENPAGES=5000
	if [[ $pages > ${HILISTENPAGES} ]] ; then
		problem ${node} "pg_listener relpages high - $pages" "Number of pages in table pg_listener is $pages
This is higher than the warning level of ${HILISTENPAGES}

Perhaps a long running transaction is preventing pg_listener from
being vacuumed out?"
	fi

	HILISTENTUPLES=200000
	if [[ $pages > ${HILISTENTUPLES} ]] ; then
		problem ${node} "pg_listener reltuples high - $tuples" "Number of tuples in table pg_listener is $tuples
This is higher than the warning level of ${HILISTENTUPLES}

Perhaps a long running transaction is preventing pg_listener from
being vacuumed out?"
	fi

	HISLTUPLES=200000
	echo ""
	echo "Size tests"
	echo "======================================================"
	sq="select relname, relpages, reltuples from pg_catalog.pg_class where relname in ('sl_log_1', 'sl_log_2', 'sl_seqlog') order by relname;"
	sqr=`RUNQUERYNODE "${DSN}" "${sq}"`
	for SQ in `echo $sqr`; do
		table=`argn "${SQ}" 1`
		pages=`argn "${SQ}" 2`
		tuples=`argn "${SQ}" 3`
		echo "size...    [${table}] - pages=[${pages}] tuples=[${tuples}]"
		if [[ $tuples > ${HISLTUPLES} ]]; then
			problem ${node} "${table} tuples = ${tuples} > ${HISLTUPLES}" "Number of tuples in Slony-I table ${table} is ${tuples} which
exceeds ${HISLTUPLES}.

You may wish to investigate whether or not a node is down, or perhaps
if sl_confirm entries have not been propagating properly.
"
		fi
	done

	echo ""
	echo "Listen Path Analysis"
	echo "=================================="
	echo ""
	inadequate_paths="select li_origin, count(*) from \"_${CLUSTER}\".sl_listen group by li_origin having count(*) < (select count(*) - 1 from \"_${CLUSTER}\".sl_node );"
	IPN=`RUNQUERYNODE "${DSN}" "${inadequate_paths}"`
	for i in `echo ${IPN}`; do
		inode=`argn "${i}" 1`
		icount=`argn "${i}" 2`
		echo "Problem node ${inode} - listen path count for node: ${icount}"
	done

	missing_paths="
      select origin, receiver from (select n1.no_id as origin, n2.no_id as receiver
      from \"_${CLUSTER}\".sl_node n1, \"_${CLUSTER}\".sl_node n2 where n1.no_id != n2.no_id) as foo
      where not exists (select 1 from \"_${CLUSTER}\".sl_listen
                     where li_origin = origin and li_receiver = receiver);"
	MPATHS=`RUNQUERYNODE "${DSN}" "${missing_paths}"`
	allmissing=""

	for i in `echo ${MPATHS}`; do
		origin=`argn "${i}" 1`
		receiver=`argn "${i}" 2`
		output="(origin, receiver) where there is a direct path missing in sl_listen: (${origin},${receiver})
"
		echo ${output}
		allmissing="${allmissing}${output}"
	done
	if [[ $allmissing != "" ]]; then
		problem ${node} "missing sl_listen paths" "${allmissing}

Please check contents of table sl_listen; some STORE LISTEN requests may be
necessary.
"
	fi

	no_direct_path="
    select sub_set, sub_provider, sub_receiver from \"_${CLUSTER}\".sl_subscribe where not exists
        (select 1 from \"_${CLUSTER}\".sl_listen 
         where li_origin = sub_provider and li_receiver = sub_receiver and li_provider = sub_provider);"
	res=`RUNQUERYNODE "${DSN}" "${no_direct_path}"`
	for i in `echo ${res}`; do
		dset=`argn "${i}" 1`
		dprov=`argn "${i}" 2`
		drec=`argn "${i}" 3`
		output="No direct path found for set ${dset} from provider ${dprov} to receiver ${drec}"
		problem ${node} "missing path from ${dprov} to ${drec}" "missing sl_listen entry - ${output}

Please check contents of table sl_listen; some STORE LISTEN requests may be
necessary."
	done

	WANTAGE="00:30:00"
	echo "Summary of event info"
	echo "-------------------------------------------------------------------"
	EVSUM="
  select ev_origin as \"Origin\", min(ev_seqno) as \"Min Sync\", max(ev_seqno) as \"Max Sync\",
         date_trunc('minutes', min(now() - ev_timestamp)) as \"Min Sync Age\",
         date_trunc('minutes', max(now() - ev_timestamp)) as \"Max Sync Age\",
         min(now() - ev_timestamp) > '${WANTAGE}' as agehi
     from \"_${CLUSTER}\".sl_event group by ev_origin;"
	psql -c "${EVSUM}"

	EVPROBS="
  select ev_origin, min(now() - ev_timestamp) > '${WANTAGE}'
     from \"_${CLUSTER}\".sl_event 
     group by ev_origin
     having min(now() - ev_timestamp) > '${WANTAGE}';"
	psql -c "${EVPROBS}"

	EVPROBRES=`RUNQUERY "${EVPROBS}"`
	for n in `echo ${EVPROBRES}`; do
		problem ${n} "Events not propagating to node ${n}" "For origin node ${n}, events not propagating quickly.

Are slons running for both nodes?

Might listen paths be missing?"
	done
	
	WANTCONFIRM="00:30:00"
	CONFQ="
    select con_origin as \"Origin\", con_received as \"Receiver\", min(con_seqno) as \"Min SYNC\",
           max(con_seqno) as \"Max SYNC\", date_trunc('minutes', min(now()-con_timestamp)) as \"Age of latest SYNC\",
           date_trunc('minutes', max(now()-con_timestamp)) as \"Age of eldest SYNC\",
           min(now() - con_timestamp) > '${WANTCONFIRM}' as \"Too Old?\"
    from \"_${CLUSTER}\".sl_confirm
    group by con_origin, con_received
    order by con_origin, con_received;"

	psql -c "${CONFQ}"

	CONFQ="
    select con_origin as \"Origin\", con_received as \"Receiver\", min(con_seqno) as \"Min SYNC\"
    from \"_${CLUSTER}\".sl_confirm
    group by con_origin, con_received
    having min(now() - con_timestamp) > '${WANTCONFIRM}'
    order by con_origin, con_received;"

	CONFQ=`RUNQUERY "${EVPROBS}"`
	for i in `echo ${CONFQ}`; do
		origin=`argn "${i}" 1`
		receiver=`argn "${i}" 2`
		
		problem ${origin} "Events from ${origin} not being confirmed by ${receiver}" "For origin node ${origin}, events not being confirmed quickly in sl_confirm.

Are slons running for both nodes?

Might listen paths be missing?"
		
	done

	echo "Listing of old open connections for node ${node}"
	ELDERLYTXN="1:30:00"
	OLDQ="
     select datname as \"Database\", procpid as \"Conn PID\", usename as \"User\", date_trunc('minutes', now() - query_start) as \"Query Age\", substr(current_query,0,20) as \"Query\"
     from pg_stat_activity
     where  (now() - query_start) > '${ELDERLYTXN}'::interval and
            current_query <> '<IDLE>'
     order by query_start;"

	psql -c "${CONFQ}"
	
	OLDQ="select procpid from pg_stat_activity where  (now() - query_start) > '${ELDERLYTXN}'::interval and current_query <> '<IDLE>';"

	OLDR=`RUNQUERY "${OLDQ}"`
	for pid in `echo ${OLDR}`; do
	    problem ${node} "Old connection (age >${ELDERLYTXN}) found - PID=${pid}" "Elderly DB connection found, which may cause degradation of database and replication performance"
	done

	ELDERLY_COMPONENT="00:05:00"
	old_comp_query="
     select co_actor as actor, co_pid as slon_pid, co_node as node , co_connection_pid as conn_pid, co_activity as activity, co_starttime as start_time, (now() - co_starttime) as age, co_event as event_id, co_eventtype as event_type
     from \"_${CLUSTER}\".sl_components 
     where  (now() - co_starttime) > '${ELDERLY_COMPONENT}'::interval
     order by co_starttime;"
	
	psql -c "${old_comp_query}"

	old_comp_query="
     select co_actor as actor, co_pid as slon_pid, co_node as node, co_connection_pid as conn_pid
     from \"_${CLUSTER}\".sl_components 
     where  (now() - co_starttime) > '${ELDERLY_COMPONENT}'::interval
     order by co_starttime;"
	OCR=`RUNQUERY "${old_comp_query}"`

	for i in `echo ${OCR}`; do 
	    actor=`argn "${i}" 1`
	    pid=`argn "${i}" 2`
	    nodeid=`argn "${i}" 3`
	    copid=`argn "${i}" 4`
	    problem ${node} "Component ${actor} has not reported back in > ${ELDERLY_COMPONENT}" "sl_component entry growing old for ${actor}, UNIX pid ${pid}, 
relating to node ${nodeid}, with DB connection PID ${copid}.

This may indicate that a slon thread has gotten stuck"
	done
done