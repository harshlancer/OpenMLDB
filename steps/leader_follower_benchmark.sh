#! /bin/sh
#
# cluster_run.sh

ROOT=`pwd`

clear_debug() {
    ps xf | grep rtidb | grep -v grep | awk '{print $1}' | while read line; do kill -9 $line; done
}

clear_debug
DIR_PREFIX=$ROOT
test -d $DIR_PREFIX/binlog1 && rm -rf $DIR_PREFIX/binlog1
test -d $DIR_PREFIX/snapshot1 && rm -rf $DIR_PREFIX/snapshot1
test -d $DIR_PREFIX/binlog2 && rm -rf $DIR_PREFIX/binlog2
test -d $DIR_PREFIX/snapshot2 && rm -rf $DIR_PREFIX/snapshot2

./build/bin/rtidb --binlog_root_path=$DIR_PREFIX/binlog1 --snapshot_root_path=$DIR_PREFIX/snapshot1 --binlog_single_file_max_size=8 --log_level=debug --gc_safe_offset=0 --gc_interval=1 --endpoint=0.0.0.0:19527 --role=tablet >log0 2>&1 &
./build/bin/rtidb --binlog_root_path=$DIR_PREFIX/binlog2 --snapshot_root_path=$DIR_PREFIX/snapshot2 --binlog_single_file_max_size=8 --log_level=debug --gc_safe_offset=0 --gc_interval=1 --endpoint=0.0.0.0:19528 --role=tablet >log1 2>&1 &
sleep 1

./build/bin/rtidb --cmd="create t1 2 1 0 false 127.0.0.1:19528" --role=client --endpoint=127.0.0.1:19528 --interactive=false 
./build/bin/rtidb --cmd="create t1 2 1 0 true 127.0.0.1:19528" --role=client --endpoint=127.0.0.1:19527 --interactive=false 
./build/bin/rtidb --cmd="create t1 1 1 0 true" --role=client --endpoint=127.0.0.1:19527 --interactive=false 
sleep 1
./build/bin/rtidb --cmd="benchmark" --role=client --endpoint=127.0.0.1:19527 --interactive=false  | grep Percentile

clear_debug