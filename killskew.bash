#!/bin/bash
#set -x
FIND=`ps aux | awk '{print $2" "$11}' | grep '.skew'`
if [ "$FIND" == "" ]; then
  echo skew process not found
  exit
fi
PID=`ps aux | awk '{print $2" "$11}' | grep '.skew' | awk '{print $1}'`
echo Killing...
ps aux | grep $PID | grep 'skew'
kill $PID
exit

