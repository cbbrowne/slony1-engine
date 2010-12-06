#!/bin/bash
# Script to analyze cluster based on specified node
PGCLUSTER=${PGCLUSTER:-"slony_regress1"}
if [[ $GENDIR == "" ]]; then
    TMPDIR=${TMPDIR:-"/tmp"}    # Consider either a default value, or /tmp
    if [ -d ${TMPDIR} ]; then
        T_TMPDIR=${TMPDIR}
    else
        for i in /tmp /usr/tmp /var/tmp; do
            if [ -d ${i} ]; then
                TMPDIR=$i
                break
            fi
        done
    fi

    case `uname` in
        SunOS|AIX|MINGW32*)
            if [ -z $TMPDIR ]; then
                err 3 "unable to create temp dir"
                exit
            fi
            ;;
        Linux)
            mktmp=`mktemp -d -t slony-analyze.XXXXXX`
            if [ $MY_MKTEMP_IS_DECREPIT ] ; then
                mktmp=`mktemp -d ${TMPDIR}/slony-analyze.XXXXXX`
            fi
            if [ ! -d $mktmp ]; then
                err 3 "mktemp failed"
            fi
            ;;
        *)
            mktmp=`mktemp -d -t slony-analyze`
            if [ ! -d $mktmp ]; then
                err 3 "mktemp failed"
            fi
            ;;
    esac
fi

function RUNQUERY () {
    local QUERY=$1
    RESULTSET=`psql -qtA -F ":" -R " " -c "${QUERY}"`
    echo ${RESULTSET}
}
function argn () {
    local V=$1
    local n=$2
    local res=`echo ${V} | cut -d : -f ${n}`
    echo $res
}

echo "# analyze-cluster.sh - output going into [${GENDIR}]"
NODES=`RUNQUERY "select no_id from \"_${PGCLUSTER}\".sl_node;"`
MYNODE=`RUNQUERY "select last_value from \"_${PGCLUSTER}\".sl_local_node_id;"`
NOW=`date`
function mklabel () {
    local purpose=$1
    echo "label=\"${purpose} - generated per node ${MYNODE} on ${NOW}\";"
}


function mknodes () {
    local file=$1
    for node in `echo $NODES`; do
        desc=`RUNQUERY "select no_comment from \"_${PGCLUSTER}\".sl_node where no_id =${node};"`
        if [[ $node -eq $MYNODE ]]; then
            echo "node${node} [shape=record, style=filled, label=\"node ${node}|${desc}\"];" >> ${file}
        else
            echo "node${node} [shape=record, label=\"node ${node}|${desc}\"];" >> ${file}
        fi
    done
}

PF=${GENDIR}/paths-overview.dot
echo "digraph pathoverview {" > ${PF}
mklabel "Slon CONNINFO PATH view" >> ${PF}
mknodes "${PF}"

PATHS=`RUNQUERY "select pa_server,pa_client from \"_${PGCLUSTER}\".sl_path;"`
for p in `echo $PATHS`; do
    server=`argn ${p} 1`
    client=`argn ${p} 2`
    conninfo=`RUNQUERY "select pa_conninfo from \"_${PGCLUSTER}\".sl_path where pa_server=${server} and pa_client=${client};"`
    echo "  node${client} -> node${server} [label=\"${conninfo}\"];" >> ${PF}
done
echo "}" >> ${PF}
dot -O -Tpng ${PF}


LISTEN=${GENDIR}/listen-overview.dot
echo "digraph listenoverview {" > ${LISTEN}
mklabel "Listen Path view" >> ${LISTEN}

mknodes "${LISTEN}"

PATHS=`RUNQUERY "select li_origin,li_provider,li_receiver from \"_${PGCLUSTER}\".sl_listen;"`
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

SUBS=`RUNQUERY "select sub_set,sub_provider,sub_receiver,sub_forward,sub_active from \"_${PGCLUSTER}\".sl_subscribe;"`
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

SETS=`RUNQUERY "select set_id, set_origin from \"_${PGCLUSTER}\".sl_set;"`
for sq in `echo $SETS`; do
    sset=`argn ${sq} 1`
    origin=`argn ${sq} 2`
    comment=`RUNQUERY "select set_comment from \"_${PGCLUSTER}\".sl_set where set_id=${sset};"`
    echo "set${sset} [label=\"set ${sset}|${comment}\"];">> ${SOV}
    echo "set${sset} -> node${origin} [label=\"set ${sset} originating on node ${origin}\"];">> ${SOV}
done
echo "}" >> ${SOV}

dot -O -Tpng ${SOV}
