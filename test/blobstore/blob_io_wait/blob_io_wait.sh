#!/usr/bin/env bash

SYSTEM=$(uname -s)
if [ $SYSTEM = "FreeBSD" ] ; then
    echo "blob_io_wait.sh cannot run on FreeBSD currently."
    exit 0
fi

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh
rpc_py="$rootdir/scripts/rpc.py"

timing_enter blob_bdev_io_wait

truncate -s 64M $testdir/aio.bdev

$rootdir/test/app/bdev_svc/bdev_svc &
bdev_svc_pid=$!

trap 'killprocess $bdev_svc_pid; exit 1' SIGINT SIGTERM EXIT

waitforlisten $bdev_svc_pid
$rpc_py bdev_aio_create $testdir/aio.bdev aio0 4096
$rpc_py bdev_lvol_create_lvstore aio0 lvs0
$rpc_py bdev_lvol_create -l lvs0 lvol0 32

killprocess $bdev_svc_pid

# Minimal number of bdev io pool (128) and cache (1)
echo "[Bdev]" > $testdir/bdevperf.conf
echo "BdevIoPoolSize 128" >> $testdir/bdevperf.conf
echo "BdevIoCacheSize 1" >> $testdir/bdevperf.conf
echo "[AIO]" >> $testdir/bdevperf.conf
echo "AIO $testdir/aio.bdev aio0 4096" >> $testdir/bdevperf.conf

$rootdir/test/bdev/bdevperf/bdevperf -c $testdir/bdevperf.conf -q 128 -o 4096 -w write -t 5 -r /var/tmp/spdk.sock &
bdev_perf_pid=$!
waitforlisten $bdev_perf_pid
$rpc_py enable_bdev_histogram aio0 -e
sleep 2
$rpc_py get_bdev_histogram aio0 | $rootdir/scripts/histogram.py
$rpc_py enable_bdev_histogram aio0 -d
wait $bdev_perf_pid

$rootdir/test/bdev/bdevperf/bdevperf -c $testdir/bdevperf.conf -q 128 -o 4096 -w read -t 5 -r /var/tmp/spdk.sock &
bdev_perf_pid=$!
waitforlisten $bdev_perf_pid
$rpc_py enable_bdev_histogram aio0 -e
sleep 2
$rpc_py get_bdev_histogram aio0 | $rootdir/scripts/histogram.py
$rpc_py enable_bdev_histogram aio0 -d
wait $bdev_perf_pid

$rootdir/test/bdev/bdevperf/bdevperf -c $testdir/bdevperf.conf -q 128 -o 4096 -w unmap -t 1

sync
rm -rf $testdir/bdevperf.conf
rm -rf $testdir/aio.bdev
trap - SIGINT SIGTERM EXIT

report_test_completion "blob_io_wait"
timing_exit bdev_io_wait
