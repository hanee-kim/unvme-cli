#!/bin/bash

# --admin_cmd= example: split id_ctrl/id_ns over the admin queue
#
# Issues admin commands (not Read/Write) through the libunvmed ioengine via a
# 60:40 ratio of Identify Controller / Identify Namespace.  The engine forces
# --sqid=0 for --admin_cmd jobs, so numjobs=2 (multiple fio threads) is used to
# verify that admin commands are all funneled onto the single admin SQ (sqid=0)
# concurrently rather than per-thread I/O queues.

set -e

BDF=""
RUNTIME=10
CMD_TIMEOUT=""

usage() {
	echo "Usage: $0 -b <bdf> [-t <runtime>] [-T <timeout>]"
	echo "  -b <bdf>      : PCI device address (e.g., '0000:01:00.0')"
	echo "  -t <runtime>  : Test duration in seconds (default: 10)"
	echo "  -T <timeout>  : Command timeout in seconds (optional)"
	echo ""
	echo "Run a fio job issuing Identify admin commands in a 60:40 ratio of"
	echo "id_ctrl:id_ns through the admin queue (--sqid=0) with numjobs=2."
	echo ""
	echo "Note that it assumes fio.so is installed in somewhere in the current system."
	exit 1
}

while getopts "b:t:T:" opt; do
	case ${opt} in
		b)
			BDF=$OPTARG
			;;
		t)
			RUNTIME=$OPTARG
			;;
		T)
			CMD_TIMEOUT=$OPTARG
			;;
		\?)
			usage
			;;
	esac
done

if [ -z "$BDF" ]; then
	usage
fi

bdf="$BDF"
bdf_dot=$(echo $bdf | sed 's/:/./g')  # Convert ':' to '.' for fio

if pgrep -x "unvmed" > /dev/null; then (
	set -x
	unvme stop
) fi

(
set -x
unvme start
unvme add $bdf --nr-ioqs=1

unvme create-adminq $bdf -s 32 -c 32
if [ -z "$CMD_TIMEOUT" ]; then
	unvme enable $bdf
else
	unvme enable $bdf -t $CMD_TIMEOUT
fi
unvme id-ctrl $bdf > /dev/null
unvme id-ns $bdf -n 1 > /dev/null
unvme status $bdf

# id_ctrl/60:id_ns/40 -> 60% Identify Controller, 40% Identify Namespace.
# bs=4k satisfies the NVME_IDENTIFY_DATA_SIZE requirement for data-bearing
# admin commands.  The engine forces --sqid=0 for --admin_cmd jobs, so both
# numjobs=2 threads share the single admin SQ.
unvme fio \
	--ioengine=libunvmed \
	--filename=$bdf_dot \
	--nsid=1 \
	--direct=1 \
	--thread=1 \
	--numjobs=2 \
	--group_reporting \
	--time_based \
	--runtime=${RUNTIME}s \
	--name=admin_cmd_split \
	--bs=4k \
	--iodepth=1 \
	--rw=read \
	--admin_cmd=id_ctrl/60:id_ns/40
)
