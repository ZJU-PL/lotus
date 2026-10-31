# Building Lotus and Preparing Bitcode

This guide covers building Lotus and compiling target C/C++ programs to LLVM bitcode (`.bc` or `.ll`) for analysis. For complete dependency requirements and build options, refer to [INSTALL.md](INSTALL.md).

## Building Lotus

### Using CMake

Ensure dependencies (LLVM 14.x, Z3 4.8.1+, CMake 3.18+, C++17 compiler) are installed:

```bash
git clone https://github.com/ZJU-PL/lotus
cd lotus
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

To include tests and run them:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DLOTUS_BUILD_TESTS=ON
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

If LLVM is in a custom path, pass `-DLLVM_BUILD_PATH=/path/to/llvm/lib/cmake/llvm`.

### Using Docker

Lotus can also be built inside a Docker container configured with LLVM 14 and required tooling. See the repository's top-level `Dockerfile`.

---

## Compiling Programs to LLVM Bitcode

Lotus analyses operate on LLVM bitcode (`.bc`) or human-readable assembly (`.ll`).

Common methods to generate bitcode include:
- `clang` (single-file or small projects)
- `gllvm` / `wllvm` (large multi-file projects, build systems like Make/Autotools/CMake)
- Clang with LTO (`-flto`)
- [Generate_Linux_Kernel_Bitcode](https://github.com/ZHYfeng/Generate_Linux_Kernel_Bitcode) (for Linux kernel analysis)

---

### Method 1: Using Clang Directly

For a single source file:

```c
// swap.c
void swap(char **p, char **q) {
  char *t = *p;
  *p = *q;
  *q = t;
}

int main() {
  char a1, b1;
  char *a = &a1;
  char *b = &b1;
  swap(&a, &b);
}
```

Compile with debug info and value names preserved:

```bash
# Generate human-readable LLVM IR (.ll)
clang -S -c -Xclang -disable-O0-optnone -fno-discard-value-names -emit-llvm swap.c -o swap.ll

# Optionally promote memory allocations to registers (mem2reg)
opt -S -p=mem2reg swap.ll -o swap.ll

# Or compile directly to binary bitcode (.bc)
clang -c -emit-llvm -g -fno-discard-value-names swap.c -o swap.bc
```

---

### Method 2: Using Whole-Program LLVM (gllvm / wllvm)

For complex projects using `make`, `configure`, or `cmake`, whole-program wrappers capture bitcode across multiple compilation units.

#### Option A: gllvm (Go-based, recommended)

1. **Install Go and gllvm**:
   ```bash
   # Install gllvm binary into $GOPATH/bin (usually ~/go/bin)
   go install github.com/SRI-CSL/gllvm/cmd/...@latest
   export PATH=$PATH:$(go env GOPATH)/bin
   ```

2. **Configure environment**:
   ```bash
   export LLVM_COMPILER_PATH=/usr/lib/llvm-14/bin  # adjust to your LLVM 14 bin directory
   gsanity-check
   ```

3. **Build the target project**:
   ```bash
   CC=gclang CXX=gclang++ ./configure
   make CC=gclang CXX=gclang++ -j$(nproc)
   ```

4. **Extract bitcode**:
   ```bash
   get-bc -b <binary_executable>
   # Produces <binary_executable>.bc
   ```

#### Option B: wllvm (Python-based)

1. **Install wllvm**:
   ```bash
   pip install wllvm
   ```

2. **Configure environment**:
   ```bash
   export LLVM_COMPILER=clang
   # If clang-14 is named with a suffix:
   export LLVM_CC_NAME=clang-14
   export LLVM_CXX_NAME=clang++-14
   export LLVM_LINK_NAME=llvm-link-14
   export LLVM_AR_NAME=llvm-ar-14

   wllvm-sanity-checker
   ```

3. **Build target and extract bitcode**:
   ```bash
   CC=wllvm CXX=wllvm++ make -j$(nproc)
   extract-bc <binary_executable>
   # Produces <binary_executable>.bc
   ```

---

### Example: Building RIOT-OS Bitcode

Target: [RIOT-OS](https://github.com/RIOT-OS/RIOT)

```bash
git clone https://github.com/RIOT-OS/RIOT
cd RIOT/examples/default/

make CC="gclang -O0 -Xclang -disable-O0-optnone -g -save-temps=obj -fno-discard-value-names -w" \
     CXX="gclang++ -O0 -Xclang -disable-O0-optnone -g -save-temps=obj -fno-discard-value-names -w" -j8

cd bin/native/
get-bc default.elf
# default.elf.bc is generated

# Disassemble to readable .ll if needed:
llvm-dis default.elf.bc
```

---

## Running Analyses

Once bitcode is generated, pass it to any Lotus binary under `build/bin/`:

```bash
# Pointer analysis
./build/bin/lotus-alias-sparrow-aa default.elf.bc

# Bug detection with Saber
./build/bin/lotus-check --engine=saber default.elf.bc

# Interprocedural control-flow graph construction
./build/bin/lotus-ir-pdg-query default.elf.bc
```

Additional bitcode benchmarks and resources: https://github.com/mbarbar/crux-bitcode
