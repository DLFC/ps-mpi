#!/bin/bash
# set -x
if [ $# -lt 3 ]; then
    echo "usage: $0 num_servers num_workers bin [args..]"
    exit -1;
fi

export DMLC_NUM_SERVER=$1
shift
export DMLC_NUM_WORKER=$1
shift
bin=$1
shift
arg="$@"

# start the scheduler
export DMLC_PS_ROOT_URI=192.168.1.107
export DMLC_PS_ROOT_PORT=8000
export DMLC_ROLE=scheduler
${bin} ${arg} &


# start servers
export DMLC_ROLE=server

ssh -o StrictHostKeyChecking=no 192.168.1.101 'export DMLC_NUM_SERVER=1;export DMLC_NUM_WORKER=3;export DMLC_PS_ROOT_URI=192.168.1.107;export DMLC_PS_ROOT_PORT=8000;export DMLC_ROLE=server;export HEAPPROFILE=./S0;~/ps-lite/tests/test_connection' &


# start workers
export DMLC_ROLE=worker

ssh -o StrictHostKeyChecking=no 192.168.1.101 'export DMLC_NUM_SERVER=1;export DMLC_NUM_WORKER=3;export DMLC_PS_ROOT_URI=192.168.1.107;export DMLC_PS_ROOT_PORT=8000;export DMLC_ROLE=worker;export HEAPPROFILE=./W0;~/ps-lite/tests/test_connection' &
ssh -o StrictHostKeyChecking=no 192.168.1.107 'export DMLC_NUM_SERVER=1;export DMLC_NUM_WORKER=3;export DMLC_PS_ROOT_URI=192.168.1.107;export DMLC_PS_ROOT_PORT=8000;export DMLC_ROLE=worker;export HEAPPROFILE=./W1;~/ps-lite/tests/test_connection' &
ssh -o StrictHostKeyChecking=no 192.168.1.101 'export DMLC_NUM_SERVER=1;export DMLC_NUM_WORKER=3;export DMLC_PS_ROOT_URI=192.168.1.107;export DMLC_PS_ROOT_PORT=8000;export DMLC_ROLE=worker;export HEAPPROFILE=./W2;~/ps-lite/tests/test_connection' &


wait
