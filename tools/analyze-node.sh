#!/bin/bash
# analyze-slony-node
# Script to analyze cluster based on specified node

TEXT=""
function help () {
    echo "analyze-slony-node [options]"
    echo "
  --text                  - Do not generate any graphics or HTML
  --help                  - Request help
  --cluster=clustername   - Optional specification of cluster to be used
  --output-directory=/tmp/somewhere  Indicates destination for graphics/HTML output"
    echo "
Additionally, uses libpq environment variables
(PGHOST/PGPORT/PGDATABASE/...) to indicate the database to check

WARNINTERVAL used to indicate intervals of event confirmation delay that indicate WARNING
DANGERINTERVAL used to indicate intervals of event confirmation delay that indicate DANGER
"

}
for arg in $*; do
    case $arg in
	--text)
	    TEXT="true"
	    ;;
	--cluster=*)
	    PGCLUSTER=`echo $arg | sed 's/^--cluster=//'`
	    ;;
	--output-directory=*)
	    GENDIR=`echo $arg | sed 's/^--output-directory=//'`
	    ;;
	--help)
	    help
	    exit
	    ;;
	*)
	    echo "Command line parameter [${arg}] not understood"
	    echo ""
	    help
	    exit
	    ;;
    esac
done

# Indirectly controlled by environment variables:
#  PGCLUSTER - indicates cluster
#  GENDIR    - indicates directory

PGCLUSTER=${PGCLUSTER:-"slony_regress1"}
CS=\"_${PGCLUSTER}\"
GENDIR=${GENDIR:-"/tmp/slony-cluster-analysis"}
WARNINTERVAL=${WARNINTERVAL:-"30 seconds"}
DANGERINTERVAL=${DANGERINTERVAL:-"5 minutes"}

echo "# analyze-cluster.sh running"
if [[ "x${TEXT}" -eq "xtrue" ]]; then
    echo "# Text output only, to STDOUT"
else
    echo "Generating graphical output in [${GENDIR}]"
    if [[ -d ${GENDIR} ]]; then
	echo "Trimming out old output from ${GENDIR}"
	for suffix in dot png html; do
	    rm -f ${GENDIR}/*.${suffix}
	done
    else
	mkdir ${GENDIR}
    fi
fi


function RUNQUERY () {
    local QUERY=$1
    RESULTSET=`psql -qtAX -F ":" -R " " -c "${QUERY}"`
    echo ${RESULTSET}
}

function RQ () {
    local QUERY=$1
    psql -qnX -c "${QUERY}"
}
function argn () {
    local V=$1
    local n=$2
    local res=`echo ${V} | cut -d : -f ${n}`
    echo $res
}

NODES=`RUNQUERY "select no_id from ${CS}.sl_node;"`
# Find local node ID
MYNODE=`RUNQUERY "select ${CS}.getlocalnodeid('_${PGCLUSTER}');"`
NOW=`date`
function mklabel () {
    local purpose=$1
    echo "label=\"${purpose} - generated per node ${MYNODE} on ${NOW}\";"
}

function mknodes () {
    local file=$1
    for node in `echo $NODES`; do
        desc=`RUNQUERY "select no_comment from ${CS}.sl_node where no_id =${node};"`
        if [[ $node -eq $MYNODE ]]; then
            echo "node${node} [shape=record, style=filled, label=\"node ${node}|${desc}\"];" >> ${file}
        else
            echo "node${node} [shape=record, label=\"node ${node}|${desc}\"];" >> ${file}
        fi
    done
}


if [[ "x${TEXT}" -eq "xtrue" ]]; then
    echo "Drawing Slony state according to node [${MYNODE}]"
    date
    echo "Nodes in cluster"
    RQ "
 select n.no_id as node, no_comment as description, 
    coalesce((select st_lag_time from ${CS}.sl_status s where s.st_received = n.no_id), '0s'::interval) as event_lag, 
    case when (select st_lag_time from ${CS}.sl_status s where s.st_received = n.no_id order by st_lag_time desc) between '${WARNINTERVAL}'::interval and '${DANGERINTERVAL}'::interval 
      then 'Behind:Warning' 
          when (select st_lag_time from ${CS}.sl_status s where s.st_received = n.no_id order by st_lag_time desc) > '${DANGERINTERVAL}'::interval
      then 'Behind:Danger!' 
      else 'Up To Date' end as \"Timeliness\" from
 ${CS}.sl_node n order by no_id; 
"

    echo "If nodes have Timeliness marked as Behind:Warning events have not propagated in > ${WARNINTERVAL}, and status for the node may not be completely up to date."
    echo "If nodes have Timeliness marked as Behind:Danger events have not propagated in > ${DANGERINTERVAL}, and status for the node is considered dangerously out of date"
    echo ""
    
    echo "Connections used by slon processes to manage inter-node communications"
    RQ "select pa_server as \"From Server\", pa_client as \"To Client\", pa_conninfo as conninfo, pa_connretry as \"Retry Time\" from ${CS}.sl_path order by pa_server, pa_client;"
    echo ""

    echo "Replication Sets"
    RQ "select set_id as \"Set ID\", set_origin as \"Origin Node\", set_comment as \"Description\",  (select count(*) from ${CS}.sl_table where tab_set = set_id) as \"Tables\", (select count(*) from ${CS}.sl_sequence where seq_set = set_id) as \"Sequences\" from ${CS}.sl_set order by set_id;"
    
    echo "Subscriptions that node ${MYNODE} is aware of"
    RQ "select sub_set as \"Set\", sub_receiver as \"Receiver\", sub_provider as \"Provider\", sub_forward as \"Does Receiver Forward?\", sub_active as \"Considered Active?\",
        exists (select 1 from ${CS}.sl_set where set_id = sub_set and set_origin = sub_provider) as \"Provider is Origin?\",
        (select now() - con_timestamp from ${CS}.sl_confirm where con_origin = sub_receiver and con_received = (select set_origin from ${CS}.sl_set where set_id = sub_set) order by con_timestamp desc limit 1) as \"Origin Confirmation Aging\"
        from ${CS}.sl_subscribe;"
    echo "Origin Confirmation Aging approximates how far behind subscriptions may be, according to this node."

    echo "Activity going on in node ${MYNODE}'s database"
    RQ "select co_actor as \"Thread\", co_pid as \"Slon PID\", co_node as \"Node Serviced\", co_connection_pid as \"DB Connection PID\", co_activity as \"Thread Activity\", co_event as \"Event\", co_eventtype as \"Event Type\", co_starttime as \"Start of Activity\"
     from ${CS}.sl_components order by co_node, co_actor;"
    echo "Note: 
   local_monitor only reports in once when slon starts up
   local_cleanup only reports in when it does a cleanup
"

     echo "Event summary"
     RQ "select ev_origin as \"Origin Node\" , ev_type as \"Event Type\", count(*) as \"Count\", max(ev_seqno) as \"Max Event #\", max(ev_timestamp) as \"Latest Occurrence\", now() - max(ev_timestamp) as \"Aging\" from ${CS}.sl_event  group by 1, 2  order by 1,2;"

else
    PF=${GENDIR}/paths-overview.dot
    echo "digraph pathoverview {" > ${PF}
    mklabel "Slon CONNINFO PATH view" >> ${PF}
    mknodes "${PF}"

    PATHS=`RUNQUERY "select pa_server,pa_client from ${CS}.sl_path;"`
    for p in `echo $PATHS`; do
	server=`argn ${p} 1`
	client=`argn ${p} 2`
	conninfo=`RUNQUERY "select pa_conninfo from ${CS}.sl_path where pa_server=${server} and pa_client=${client};"`
	echo "  node${client} -> node${server} [label=\"${conninfo}\"];" >> ${PF}
    done
    echo "}" >> ${PF}
    dot -O -Tpng ${PF}


    LISTEN=${GENDIR}/listen-overview.dot
    echo "digraph listenoverview {" > ${LISTEN}
    mklabel "Listen Path view" >> ${LISTEN}
    
    mknodes "${LISTEN}"
    
    PATHS=`RUNQUERY "select li_origin,li_provider,li_receiver from ${CS}.sl_listen;"`
    for p in `echo $PATHS`; do
	origin=`argn ${p} 1`
	provider=`argn ${p} 2`
	receiver=`argn ${p} 3`
	echo "  node${receiver} -> node${provider} [label=\"${origin}\"];" >> ${LISTEN}
    done
    echo "}" >> ${LISTEN}
    dot -O -Tpng ${LISTEN}

    SOV=${GENDIR}/subscription-overview.dot
    echo "digraph subscriptionview {" > ${SOV}
    mklabel "Subscription view" >> ${SOV}

    mknodes "${SOV}"

    SUBS=`RUNQUERY "select sub_set,sub_provider,sub_receiver,sub_forward,sub_active from ${CS}.sl_subscribe;"`
    for i in `echo $SUBS`; do
	sset=`argn "${i}" 1`
	provider=`argn "${i}" 2`
	receiver=`argn "${i}" 3`
	forwarding=`argn "${i}" 4`
	active=`argn "${i}" 5`
	
	echo "node${provider} -> node${receiver} [ " >> ${SOV}
	if [[ $forwarding -eq "t" ]]; then
            style="bold"
	else
            style="solid"
	fi
	
	if [[ $active -eq "t" ]]; then
            style="${style}"
	else
            style="dotted"
	fi
	echo "style=${style}" >> ${SOV}
	
    echo "];" >> ${SOV}
    done
    
    SETS=`RUNQUERY "select set_id, set_origin from ${CS}.sl_set;"`
    for sq in `echo $SETS`; do
	sset=`argn ${sq} 1`
	origin=`argn ${sq} 2`
	comment=`RUNQUERY "select set_comment from ${CS}.sl_set where set_id=${sset};"`
	echo "set${sset} [label=\"set ${sset}|${comment}\"];">> ${SOV}
	echo "set${sset} -> node${origin} [label=\"set ${sset} originating on node ${origin}\"];">> ${SOV}
    done
    echo "}" >> ${SOV}
    
    dot -O -Tpng ${SOV}
fi
