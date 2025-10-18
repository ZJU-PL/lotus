# Nisse

Ball Larus edge profiling with loop optimization

https://github.com/lac-dcc/Nisse

An exact profiler inserts counters in a program to record how many times each edge of that program's control-flow graph has been traversed during an execution of it.
It is common practice to instrument only edges in the complement of a [minimum spanning tree](https://en.wikipedia.org/wiki/Minimum_spanning_tree) of the program's [control-flow graph](https://en.wikipedia.org/wiki/Control-flow_graph), following the algorithm proposed by [Knuth and Stevenson](https://doi.org/10.1007/BF01951942) in 1973.
This repository introduces a technique to reduce the overhead of exact profiling even more.
It is possible to use the values of variables incremented by constant steps within loops (henceforth called affine variables) as a replacement for some counters.
Such affine variables are common, for they include [induction variables](https://en.wikipedia.org/wiki/Induction_variable) of loops.
This repository implements this technique in the [LLVM](https://llvm.org/) compilation infrastructure.

# Build
Nisse is implemented on top of LLVM.
The code can be built [out of the LLVM tree](https://github.com/banach-space/llvm-tutor#helloworld-your-first-pass) as follows:

```bash
LLVM_INSTALL_DIR=<path/to/llvm/build>
cmake -DLLVM_INSTALL_DIR=$LLVM_INSTALL_DIR -G "Unix Makefiles" -B build/ .
cd build
make
```

# Running
We provide a bash script to run our passes, which can be invoked as follows:

```bash
bash nisse_profiler.sh <path/to/file.c>
```

It also requires to configure the file `config.sh`.

LLVM_INSTALL_DIR is the path to the build directory of llvm.
SOURCE_DIR is the path to this folder.

This basic command will create a folder `file.c.profiling`.
In that folder there are 5 subfolders:
* `compiled` contains 3 files:
  *  `file.ll`: an IR file modified by the following LLVM passes: mem2reg, instnamer, loop-simplify and break-crit-edges.
  *  `file.profiled.ll`: an IR file instrumented with KS counters.
  *  `file`: an executable file compiled from `file.profiled.ll`.
* `profiles` contains the complete profile for each function. The profile will contain the total execution for each function, having the execution for each edge and each basic block of the function.
* `partial_profiles` contains the profile data obtained for every function in one file, along with a file with the number of edges for each function.
* `graphs` contains the vertices, edges, spanning tree and instrumented edges of each function's CFG.
* `dot` contains a `dot` file with the representation of each function's CFG.

Functions with no branches are not instrumented (since their execution is always linear).