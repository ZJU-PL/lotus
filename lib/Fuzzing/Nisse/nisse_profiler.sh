#!/usr/bin/env bash


LLVM_INSTALL_DIR=/usr/local
SOURCE_DIR=~/Codes/Nisse
BUILD_DIR=$SOURCE_DIR/build

LLVM_OPT=$LLVM_INSTALL_DIR/bin/opt
LLVM_CLANG=$LLVM_INSTALL_DIR/bin/clang
PROFILER_IMPL=$SOURCE_DIR/lib/prof.c
MY_LLVM_LIB=$BUILD_DIR/lib/libNisse.so
PROP_BIN=$BUILD_DIR/bin/propagation

# Move to folder where the file is:
#
DR_NAME="$(dirname $1)"
cd $DR_NAME

# File names:
#
FL_NAME=$(basename $1)
BS_NAME=$(basename $FL_NAME .c)
PROF_DIR=$FL_NAME.profiling
LL_NAME="$BS_NAME.ll"
PF_NAME="$BS_NAME.profiled.ll"
INFO_PROF="info.prof"
MAIN_PROF="main.prof"
PROF_SUF=".prof.full"


# Create the profiling folder:
#
if [ -d "$PROF_DIR" ]; then
  rm -r "$PROF_DIR"
fi
mkdir $PROF_DIR
cd $PROF_DIR

CLANG_FLAGS="-Xclang -disable-O0-optnone -std=c99 -c -S -emit-llvm"

# Generating the bytecode in SSA form:
#
$LLVM_CLANG $CLANG_FLAGS ../$FL_NAME -o $LL_NAME
if [[ $ret_code -ne 0 ]]; then
  echo "Compilation failed"
  cd -
  echo $FL_NAME >> err.txt
  exit $ret_code
fi
$LLVM_OPT -S -passes="mem2reg,instnamer" $LL_NAME -o $LL_NAME

# Running the pass:
#
$LLVM_OPT -S -load-pass-plugin $MY_LLVM_LIB -passes="ks" -stats \
    $LL_NAME -o $PF_NAME

# Print the instrumented CFG
#
$LLVM_OPT -S -passes="dot-cfg" -stats \
    $PF_NAME -o $PF_NAME

# Compile the newly instrumented program, and link it against the profiler.
#
$LLVM_OPT -S -passes="loop-simplify,break-crit-edges" $LL_NAME -o $LL_NAME
$LLVM_CLANG -Wall -std=c99 $PF_NAME $PROFILER_IMPL -o $BS_NAME
ret_code=$?
if [[ $ret_code -ne 0 ]]; then
  echo "Compilation failed"
  cd -
  echo $FL_NAME >> err.txt
  exit $ret_code
fi

# Run the instrumented binary:
#
if [ $# -ge 2 ]
then
  ./$BS_NAME $2
else
  ./$BS_NAME 0
fi
ret_code=$?
if [ ! -f $INFO_PROF ] || [ ! -f $MAIN_PROF ]
then
  echo "Execution failed"
  cd -
  echo $FL_NAME >> err.txt
  exit $ret_code
fi

# Propagate the weights for each function:
#
$PROP_BIN $INFO_PROF $MAIN_PROF -o ".prof.full"

# Prepare the result folders
#
mkdir graphs profiles compiled partial_profiles dot

# Move the files to apropriate folders
#
for i in .*.dot; do
  mv $i dot/
done

for i in *.graph; do
  mv $i graphs/
done

for i in *.ll; do
  mv $i compiled/
done

for i in *.prof.full*; do
  mv $i profiles/
done

for i in *.prof; do
  mv $i partial_profiles/
done

mv $BS_NAME compiled/

# Go back to the folder where you were before:
#
cd -