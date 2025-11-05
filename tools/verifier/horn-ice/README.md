# Horn-ICE


Adapted from https://github.com/horn-ice/hice-dt

An online demo: https://horn-ice.mpi-sws.org/

OOPSLA 18: Horn-ICE learning for synthesizing invariants and contracts https://dl.acm.org/doi/10.1145/3276501

- About the frontend: I remove the Boogie frontend, which relies on dotnet. (NOTE: Boogie has its own verification engine.)
- The available frontend is "chc-verifier": A verifier for Constrained Horn Clauses built on top of Microsoft Z3, which verifies constrained Horn clauses in the SMTLib2 datalog format (e.g., as produced by SeaHorn)
- About the benchamark: "The third benchmark suite consists of 45 sequential programs without recursion taken from Dillig et al.. Our aim here is to evaluate the performance of our technique as a solver for constraint Horn clauses (CHCs). To this end, we first generated CHCs of the programs in Dillig et al.’s benchmarks suite using SeaHorn. Then, we compared Horn-DT-CHC with Z3/PDR, a state-of-the-art CHC solver.". The benchmarks are also called "HOLA" in some papers.
- There are also other benchmarks in the orignal repo.


## Other Baselines

- Ultimate Automizer
- Z3:  PDR (the Spacer engine)