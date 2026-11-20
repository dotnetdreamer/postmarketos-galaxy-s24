#!/bin/sh
# remote.sh: (re)start s24-remote in the nested session; print its status once the address is there.
. /tmp/s24rt/t.sh
for p in $(pgrep -x s24-remote); do tr '\0' '\n' </proc/$p/environ 2>/dev/null | grep -q "^S24_REMOTE_CONFIG=$T/remote.conf" && kill $p; done
sleep 8
cat $T/cache/s24-remote/s24-remote.log >>$T/history.log 2>/dev/null; echo '==== restart' >>$T/history.log; : >$T/cache/s24-remote/s24-remote.log
cl remote env XDG_RUNTIME_DIR=$R S24_REMOTE_CONFIG=$T/remote.conf ${EXTRA:-} $BIN serve
for i in $(seq 1 90); do grep -q '"url":"http' $R/s24-remote/status.json 2>/dev/null && break; sleep 1; done
cat $R/s24-remote/status.json; echo
