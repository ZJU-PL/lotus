#!/bin/bash

PID=$1
DURATION=1
DELIM="\\t"

if [ $# != 1 ]
then 
	echo "Usage: mem_usage.sh PID"
	exit 1
fi

title="data"$DELIM"total"$DELIM"rss"$DELIM"dirty"
echo -e $title

while [ 1 = 1 ]
do
	temp=`pmap -x $PID | grep total | grep -oP "[0-9]+.*"| sed "s/\s\+/ /g"`
	total=`echo $temp | cut -d ' ' -f1`
	rss=`echo $temp | cut -d ' ' -f2`
	dirty=`echo $temp | cut -d ' ' -f3`
	date=`date +20%y-%m-%d-%H-%M-%S`
	echo -e "$date$DELIM$total$DELIM$rss$DELIM$dirty"
	sleep $DURATION

	NUM=$( ps aux | awk '{print $2}' | grep $PID | wc -l )
	if [ $NUM -eq 0 ];
	then 
		exit
	fi
done
