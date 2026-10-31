# MPI Static Analysis

This module provides static analysis for MPI programs in LLVM IR. It focuses on
communication structure in the SPMD model rather than shared-memory threading.

## Components

- `MPIProcessModel`: extracts MPI operations and records metadata such as
  communicator, rank, tag, request, and window handles. It normalizes raw
  calls into MPI facts; point-to-point and request semantics are decided by
  the fact classes below.
- `MPICollectiveAnalysis`: owns collective protocol composition, collective
  compatibility, and rank-guarded collective reasoning.
- `MPIRMAAnalysis`: tracks RMA windows, synchronization epochs, and possible
  RMA races.
- `MPIRankAnalysis`: symbolic rank reasoning used by the collective checker.

## Authoritative Facts

The following internal facts are the inputs that the MPI checkers reason over:

- `MPIProcessSetFact` / `MPIParticipantSet`: canonical process/rank scope facts
- `MPIRequestSetFact`: request lifecycle and completion-scope facts
- `MPIChannelAutomaton`: channel state, ambiguity, and discharge facts
- `MPIChannelObligation`: projected point-to-point and request/discharge facts
- `CollectiveProtocolFrontier`: collective grouping/proof state
- `RMASynchronizationFact`: RMA epoch, completion, and synchronization facts
- `MPIFunctionSummary`: projected function exit-state across channel/request and
  collective effects

The public result buckets in `MPIAnalysis::getResults()` are derived from these
facts.

## Entry Point

Use [MPIAnalysis.h](../../include/Concurrency/MPI/MPIAnalysis.h):

```c++
mpi::MPIAnalysis analysis(module);
analysis.runAnalysis();
const auto &results = analysis.getResults();
```

The top-level results include:

- orphaned non-blocking requests
- potential blocking send/recv deadlocks
- mismatched collectives
- conditional collectives
- unsynchronized RMA operations
- potential RMA races
- leaked windows

## Supported Modeling

- Point-to-point: blocking and non-blocking send/recv, matched-message
  operations (`MPI_Mprobe`, `MPI_Improbe`, `MPI_Mrecv`, `MPI_Imrecv`), plus
  `MPI_Sendrecv`
- Collectives: barriers, common blocking/non-blocking collectives, neighbor
  collectives, and intercommunicator broadcast classification
- Requests: `Wait*`, `Test*`, `Request_free`, `Cancel`, persistent
  request creation/activation, and request-bearing communicator duplication
  (`MPI_Comm_idup`)
- Symbol aliases: `PMPI_*`, `__wrap_MPI_*`, and OpenMPI internal
  `ompi_mpi_*` symbols are normalized to MPI semantics
- Communicators: alias/canonicalization support for duplicated, split,
  nonblocking-duplicated, intercommunicator-created, and topology-derived
  communicators
- Sessions: MPI session lifecycle calls are recognized and surfaced as session
  events
- RMA: `Put`, `Get`, `Accumulate`, selected atomic ops, and lock/fence-style
  synchronization

## Limitations

- Deadlock detection is static and conservative; unresolved request/channel
  facts degrade to explicit model gaps.
- Collective checking is summary-driven but still conservative when
  communicator, participant, or helper-summary scopes are unresolved.
- Neighbor and intercommunicator collectives are classified more precisely, but
  topology-specific participant inference is still coarse.
- Session APIs are recognized, but session-created communicator derivation is
  not yet modeled as deeply as world/split/dup communicator flows.
- Unknown ranks, tags, or communicators are handled conservatively.
- PSCW RMA synchronization is modeled, but unresolved access/exposure scopes
  still degrade to model gaps rather than strong proofs.
- Some public result buckets are derived views over richer automaton/summary
  state, so their fields are coarser than the underlying facts.

## Tests

See:

- `tests/unit/Concurrency/MPI/MPIAnalysisTest.cpp`
- `tests/unit/Concurrency/MPI/MPIRankAnalysisTest_Basic.cpp`

## Related Work

- MC-CChecker, EuroMPI 2018
- MC-Checker, SC 2014
- Dynamic Data Race Detection for MPI-RMA Programs, EuroMPI 2021
