# DemandAPA

This is the independent demand-driven APA implementation imported from the
OOPSA 23 paper, *Exploiting the Sparseness of Control-flow and Call Graphs
for Efficient and On-demand Algebraic Program Analysis*. It is separate from
Lotus's call-string APA solver in `Dataflow/APA`.

The implementation lives in `include/Dataflow/DemandAPA/`; its command-line
entry point is `tools/dataflow/DemandAPA/Main.cpp`. The public-facing build
target is `DemandAPA` (an interface library) and the executable is
`lotus-demand-apa`. Headers include their own dependencies. `Algorithm.h`
provides the algorithm declaration and all template implementations; callers do
not need to include implementation fragments in a particular order.

Non-template header definitions use C++17 inline linkage, allowing multiple
translation units to include the interface. The imported algebra caches and
projection contexts remain shared global state, owned by `Algebra.h` and
`Project.h`; this change does not make independent analyses reentrant. Drivers
use those definitions rather than redeclaring them. The baseline comparison
timeout is per algorithm instance and is passed as the optional fourth argument
of `Algorithm::work` (default 20 seconds).

The `dataflow_tests` suite (cases `DemandAPAHeadersTest.*`) compiles every header
in a separate translation unit, links them together with a reverse-include-order
consumer, and tests shared projection and algebra state. Select it with
`LOTUS_TEST_SUBSYSTEMS=dataflow/demandapa` when DemandAPA and unit tests are
enabled.

Enable it with `-DLOTUS_ENABLE_DEMAND_APA=ON`. BuDDy 2.4 is built from the
vendored source in `third-party/buddy-2.4`. OpenMP is used when available;
otherwise the same code runs serially. The original PACE 2017 treewidth and
PACE 2020 treedepth solvers can be built with
`-DLOTUS_ENABLE_FLOW_CUTTER=ON`.

For a raw Boolean program, enable `LOTUS_ENABLE_BOOLEAN_PROGRAM_TOOLS` too,
then prepare the input and decompositions with:

```sh
python3 scripts/prepare_demand_apa_boolean.py input.bp /path/to/dataset \
  --treewidth-seconds 60 --treedepth-seconds 30
```

Run from any directory:

```sh
build/bin/lotus-demand-apa 600 100 0 /path/to
```

The arguments are baseline timeout in seconds, maximum instances per analysis,
reset flag (`1` truncates `results.csv`), and dataset root. The dataset root
must contain `prep_output`, `treewidth_solver_output`, and
`treedepth_solver_output`. Results are written to its `results.csv`.
The optional environment variables `LOTUS_DEMAND_APA_MAX_QUERIES`,
`LOTUS_DEMAND_APA_BDD_NODES`, and `LOTUS_DEMAND_APA_BDD_CACHE` allow smaller
smoke runs. Defaults match the imported experiment: one million queries and
BuDDy capacities of 100 million nodes and 10 million cache entries.

The upstream bundle contains no license notice. Provenance is retained here; check redistribution terms before
publishing this imported code outside the workspace. FlowCutter and BuDDy
retain their own upstream notices in `third-party`.
