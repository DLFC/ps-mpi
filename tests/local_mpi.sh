#!/bin/bash
# set -x

export DMLC_NUM_SERVER=2
export DMLC_NUM_WORKER=4

# start the scheduler
export DMLC_PS_ROOT_URI='127.0.0.1'
export DMLC_PS_ROOT_PORT=8000
export PS_VERBOSE=0
mpiexec -n 7 ./test_kv_app &

wait
