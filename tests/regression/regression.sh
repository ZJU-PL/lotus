executable=$1
bc_dir=$2
benchmarks_bin_dir=$3

echo "[INFO] ----------------------------------------------------"
echo "[INFO] Regression begins (spec2006)"
echo "[INFO] ----------------------------------------------------"

rm -f $benchmarks_bin_dir/*.log
rm -f $benchmarks_bin_dir/*.err

for bc in $bc_dir/*.bc;
do
  proj=`basename $bc`
  printf "Running %20s" "$proj"

  start_time=$(date +%s)
  $executable $bc -nworkers=5 >>$benchmarks_bin_dir/$proj.log 2>$benchmarks_bin_dir/$proj.err
  ret=$?
  end_time=$(date +%s)
  elapsed=$((end_time - start_time))
  printf "\t takes %5s seconds. " "$elapsed"

  if [ $ret -ne 0 ]; then
    printf "\tFail!\n"
  else
    printf "\tPass!\n"
  fi
done

echo "[INFO] ----------------------------------------------------"
echo "[INFO] Regression completes (spec2006)"
echo "[INFO] ----------------------------------------------------"