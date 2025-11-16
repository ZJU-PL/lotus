#!/bin/bash
#
# llvm-mutate -[n|l|g|G] -[c|i|s instructions] -l -[o path]
#  mutate llvm IR
#
# Options
#  -h,--help ------- print this help output
#  -I,--ids -------- print the number of instructions
#  -L,--list ------- list instructions with number and types
#  -n,--name ------- name each instruction w/number
#  -g,--cfg -------- graph the CFG
#  -G,--call-graph - graph the call graph
#  -t,--trace ------ instrument to trace instructions
#  -T,--trace-obj -- object file providing tracing function
#                    default is llvm_mutate_trace.o
#  -c,--cut -------- cut the given instruction
#  -r,--replace ---- replace the first inst. with the second
#  -i,--insert ----- copy the second inst. before the first
#  -s,--swap ------- swap the given instructions
#  -l,--link ------- link the result into an executable
#  -o,--out -------- write output to specified file
#
HELP_TEXT=$(cat "$0" \
    |sed '/^[^#]/q' \
    |head -n -1 \
    |tail -n +3 \
    |sed -e :a -e '/^\n*$/{$d;N;ba' -e '}' \
    |cut -c3-)
if [ -z "$LLVM" ];then
    LLVM="$(dirname $0)/.."
fi
OPT_FLAGS="-load ${LLVM}/lib/Mutate.so -f"
TRACE=""
TRACE_OBJ="llvm_mutate_trace.o"
LINK_FLAGS=""
LINK=""
OUT=""
FIRST=0
RAW=""

init(){ if [ $FIRST -eq 0 ];then FIRST=1; RAW=$(cat -);fi; }
run(){ init; RAW="$(echo "$RAW"|opt ${OPT_FLAGS} $@|llvm-dis)"; }
graph(){
    for graph in $(run $1 3>&1 >/dev/null 2>&3 \
        |sed "s/Writing '//;s/'...//");do
        if [ -f $graph ];then
            if which feh >/dev/null && which dot >/dev/null;then
                echo $graph
                cat $graph|dot -Tpng|feh - -.
            else
                cat $graph
            fi
            rm -f $graph
        fi
    done; }

getopt -T > /dev/null
if [ $? -eq 4 ]; then
    # GNU enhanced getopt is available
    eval set -- $(getopt \
        -o hILntT:gGc:r:i:s:lo:f: \
        -l help,ids,cfg,call-graph,list,name,trace,trace-obj:,cut:,replace:,insert:,swap:,link,out:,file: \
        -- "$@" || echo "$HELP_TEXT" && exit 1;)
else
    # Original getopt is available
    HELP_TEXT=$(echo "$HELP_TEXT"|sed 's/,-[^ ]* / ---/;s/\(........\)-*/\1/')
    eval set -- $(getopt hILntT:gGc:r:i:s:lo:f: "$@" || echo "$HELP_TEXT" && exit 1;) 2>/dev/null
fi

## Process Options
while [ $# -gt 0 ];do
    case $1 in
        -h|--help) echo "$HELP_TEXT" && exit 0;;
        -l|--link) LINK="yes";;
        -o|--out)  OUT="$2"; shift;;
        -f|--file) FILE="$2"; shift;;
        -I|--ids)   run -ids ;;
        -L|--list)  run -list ;;
        -n|--name)  run -name ;;
        -g|--cfg)   graph -dot-cfg ;;
        -G|--call-graph) graph -dot-callgraph ;;
        -t|--trace) TRACE="yes"; run -trace;;
        -T|--trace-obj) TRACE_OBJ="$2"; shift;;
        -c|--cut)   run -cut -inst1="$2"; shift;;
        -r|--replace)
            run -replace \
                -inst1="$(echo $2|cut -d, -f1)" \
                -inst2="$(echo $2|cut -d, -f2)"; shift;;
        -i|--insert)
            run -insert \
                -inst1="$(echo $2|cut -d, -f1)" \
                -inst2="$(echo $2|cut -d, -f2)"; shift;;
        -s|--swap)
            run -swap \
                -inst1="$(echo $2|cut -d, -f1)" \
                -inst2="$(echo $2|cut -d, -f2)"; shift;;
        (--) shift; break;;
        (-*) echo "$HELP_TEXT" && exit 1;;
        (*)  break;;
    esac
    shift
done

init

if [ ! -z "$LINK" ];then
    if [ -z "$OUT" ];then OUT=a.out; fi
    if [ ! -z "$TRACE" ];then LINK_FLAGS+=" $TRACE_OBJ"; fi
    echo "$RAW"|llc|clang $LINK_FLAGS -x assembler - -o $OUT
else
    if [ -z "$OUT" ];then OUT=/dev/stdout; fi
    echo "$RAW" > $OUT
fi