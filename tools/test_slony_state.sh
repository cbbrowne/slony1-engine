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
done