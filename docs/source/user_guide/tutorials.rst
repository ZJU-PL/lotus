Tutorials and Examples
======================

Hands-on tutorials for analyzing programs with Lotus tools.
For optimization passes that can reshape IR before analysis, see
:doc:`../optimization/index` and the :doc:`../optimization/swprefetching` guide.

Tutorial 1: Basic Alias Analysis
---------------------------------

Create ``example1.c``:

.. code-block:: c

   #include <stdlib.h>
   int *global_ptr;
   void update_value(int *p, int value) {
       *p = value;
   }
   int main() {
       int x = 10;
       int y = 20;
       int *ptr1 = &x;
       int *ptr2 = &y;
       update_value(ptr1, 30);
       int *heap = (int *)malloc(sizeof(int));
       *heap = 40;
       global_ptr = heap;
       free(heap);
       return 0;
   }

.. code-block:: bash

   clang -emit-llvm -c -g example1.c -o example1.bc
   clang -emit-llvm -S -g example1.c -o example1.ll

SparrowAA:

.. code-block:: bash

   ./build/bin/lotus-alias-sparrow-aa example1.bc

This performs context-insensitive, inclusion-based pointer analysis. Good for quick scans.

Note: lotus-alias-sparrow-aa also has context-sensitive variants.

AserPTA with callsite sensitivity:

.. code-block:: bash

   ./build/bin/lotus-alias-aser-aa -analysis-mode=1-cfa -solver=wave -dump-stats example1.bc

This adds call-site sensitivity for better precision while maintaining good performance.

DyckAA:

.. code-block:: bash

   ./build/bin/lotus-alias-dyck-aa -v example1.bc

Uses Dyck-CFL reachability; precision depends on the program and analysis model.

Understanding Output
~~~~~~~~~~~~~~~~~~~~

The analyses will report:

- **Points-to sets**: What each pointer may point to
- **Alias sets**: Which pointers may alias each other
- **Call graph**: Which functions may be called through function pointers
- **Statistics**: Number of pointers, objects, constraints solved

Tutorial 2: Detecting Integer Overflow
---------------------------------------

Create ``overflow.c``:

.. code-block:: c

   #include <stdio.h>
   #include <limits.h>
   int add_values(int a, int b) {
       return a + b;  // Potential overflow
   }
   int multiply_values(int a, int b) {
       return a * b;  // Potential overflow
   }
   int safe_add(int a, int b) {
       if (a > 0 && b > INT_MAX - a) {
           return INT_MAX;  // Saturate
       }
       if (a < 0 && b < INT_MIN - a) {
           return INT_MIN;  // Saturate
       }
       return a + b;  // Safe
   }
   int main() {
       int x = INT_MAX - 10;
       int y = 20;
       int result1 = add_values(x, y);  // Bug: overflow
       int result2 = multiply_values(1000, 3000000);  // Bug: overflow
       int result3 = safe_add(x, y);  // Safe
       printf("Results: %d, %d, %d\n", result1, result2, result3);
       return 0;
   }

.. code-block:: bash

   clang -emit-llvm -S -g overflow.c -o overflow.ll
   ./build/bin/lotus-check --engine=kint overflow.ll --checks=int-overflow --kint.analyze-all-functions

Advanced: All Checks
~~~~~~~~~~~~~~~~~~~~

.. code-block:: bash

   ./build/bin/lotus-check --engine=kint overflow.ll --checks=all --kint.analyze-all-functions

This enables the Kint checks: integer overflow, division by zero, bad shift, array bounds, and dead branches.
Review guarded arithmetic findings: Kint may over-approximate conditions in ``safe_add``.

Tutorial 3: Null Pointer Detection
-----------------------------------

Let's detect null pointer dereferences.
Use ``ae`` to follow a callee's NULL return to ``print_person``.
Other engines have different coverage; see :ref:`Choosing a Checker <choosing-a-checker>`.

Create ``nullpointer.c``:

.. code-block:: c

   #include <stdio.h>
   #include <stdlib.h>
   #include <string.h>
   typedef struct {
       int id;
       char *name;
   } Person;
   Person *create_person(int id, const char *name) {
       if (id < 0) {
           return NULL;  // Error case
       }
       Person *p = (Person *)malloc(sizeof(Person));
       if (p) {
           p->id = id;
           p->name = strdup(name);
       }
       return p;
   }
   void print_person(Person *p) {
       printf("Person %d: %s\n", p->id, p->name);  // Bug if p is NULL
   }
   int main() {
       Person *p1 = create_person(1, "Alice");
       print_person(p1);  // Safe if malloc succeeded
       Person *p2 = create_person(-1, "Bob");
       print_person(p2);  // Bug: p2 is NULL
       Person *p3 = create_person(2, "Charlie");
       if (p3) {
           print_person(p3);  // Safe: null check
       }
       return 0;
   }

.. code-block:: bash

   clang -emit-llvm -c -g nullpointer.c -o nullpointer.bc
   ./build/bin/lotus-check --engine=ae nullpointer.bc --checks=null-deref --verbose

Tutorial 4: Taint Analysis
---------------------------

Create ``taint.c``:

.. code-block:: c

   #include <stdio.h>
   #include <stdlib.h>
   #include <string.h>
   void execute_command(const char *cmd) {
       system(cmd);  // Dangerous sink
   }
   char *sanitize(const char *input) {
       // Simplified sanitization
       char *output = strdup(input);
       for (int i = 0; output[i]; i++) {
           if (output[i] == ';' || output[i] == '|') {
               output[i] = '_';
           }
       }
       return output;
   }
   int main() {
       char user_input[256];
       printf("Enter command: ");
       scanf("%255s", user_input);  // Tainted source
       // Bug: direct flow from source to sink
       execute_command(user_input);
       // Illustrative filtering only; not a secure shell sanitizer
       char *safe_cmd = sanitize(user_input);
       execute_command(safe_cmd);
       free(safe_cmd);
       return 0;
   }

.. code-block:: bash

   clang -emit-llvm -c -g taint.c -o taint.bc
   ./build/bin/lotus-check --engine=taint-ifds taint.bc --verbose

.. code-block:: bash

   ./build/bin/lotus-check --engine=taint-ifds taint.bc \
       --taint.sources=scanf,gets,read \
       --taint.sinks=system,exec,popen

Tutorial 5: Abstract Interpretation with CLAM
----------------------------------------------

Create ``verify.c``:

.. code-block:: c

   #include <assert.h>
   int abs(int x) {
       if (x < 0) {
           return -x;
       }
       return x;
   }
   int bounded_add(int a, int b) {
       // Pre-condition: -100 <= a, b <= 100
       int result = a + b;
       // Post-condition: -200 <= result <= 200
       assert(result >= -200 && result <= 200);
       return result;
   }
   int div_safe(int a, int b) {
       if (b == 0) {
           return 0;
       }
       return a / b;
   }
   int main() {
       int x = abs(-10);
       assert(x >= 0);  // Should be proven
       int y = bounded_add(50, 60);
       // Assertion in bounded_add should hold
       int z = div_safe(100, 5);
       // No division by zero
       return 0;
   }

.. code-block:: bash

   clang -emit-llvm -c -g verify.c -o verify.bc
   ./build/bin/clam-pp --clam-lower-unsigned-icmp --clam-lower-select \
       verify.bc -o verify.prep.bc

.. code-block:: bash

   ./build/bin/clam --crab-dom=int --crab-check \
       --crab-print-invariants=blocks verify.prep.bc

.. code-block:: bash

   ./build/bin/clam --crab-dom=zones --crab-check \
       --crab-print-invariants=blocks verify.prep.bc

.. code-block:: bash

   ./build/bin/clam --crab-dom=pk --crab-check \
       --crab-print-invariants=blocks verify.prep.bc

.. code-block:: bash

   ./build/bin/clam --crab-dom=zones --crab-check \
       -ojson=verify_results.json verify.prep.bc

Tutorial 6: Program Dependence Graph Queries
---------------------------------------------

Create ``security.c``:

.. code-block:: c

   #include <stdio.h>
   #include <string.h>
   #include <stdbool.h>
   bool authenticate(const char *password) {
       return strcmp(password, "secret123") == 0;
   }
   char *get_secret_data() {
       return "Confidential Information";
   }
   void log_message(const char *msg) {
       printf("LOG: %s\n", msg);
   }
   void send_to_network(const char *data) {
       printf("NETWORK: %s\n", data);
   }
   int main() {
       char password[100];
       printf("Enter password: ");
       scanf("%99s", password);
       if (authenticate(password)) {
           char *secret = get_secret_data();
           log_message("Access granted");
           // Secret should not flow to network without auth
           send_to_network(secret);
       } else {
           log_message("Access denied");
       }
       return 0;
   }

.. code-block:: bash

   clang -emit-llvm -c -g security.c -o security.bc
   ./build/bin/lotus-ir-pdg-query -i security.bc

Enter each interactive query on one line (without a prompt):

.. code-block:: text

   MATCH (n) RETURN n
   MATCH (func:FUNC_ENTRY)-[:PARAMETER_OUT]->(ret:INST_RET) WHERE func.name = 'get_secret_data' RETURN ret
   MATCH (func:FUNC_ENTRY)-[:PARAMETER_IN]->(param:PARAM_FORMALIN) WHERE func.name = 'send_to_network' RETURN param

See :doc:`pdg_query_language` for slicing, security queries, and batch files.

Tutorial 7: Dynamic Validation with DynAA
------------------------------------------

Create ``dynamic.c``:

.. code-block:: c

   #include <stdio.h>
   void process(int *p, int *q) {
       *p = 10;
       *q = 20;
   }
   int main() {
       int x, y;
       process(&x, &y);
       printf("%d %d\n", x, y);
       return 0;
   }

.. code-block:: bash

   # 1. Compile to bitcode
   clang -emit-llvm -c -g dynamic.c -o dynamic.bc
   # 2. Instrument for dynamic analysis
   ./build/bin/dynaa-instrument dynamic.bc -o dynamic.inst.bc
   # 3. Compile instrumented code
   clang dynamic.inst.bc build/libRuntime.a -o dynamic.inst
   # 4. Run and collect traces
   mkdir logs
   LOG_DIR=logs/ ./dynamic.inst
   # 5. Check static analysis against traces
   ./build/bin/dynaa-check dynamic.bc logs/pts.log basic-aa

.. code-block:: bash

   ./build/bin/dynaa-log-dump logs/pts.log

Tutorial 8: Interprocedural Analysis
-------------------------------------

Create ``utils.c``:

.. code-block:: c

   #include <stdlib.h>
   #include "utils.h"
   Buffer *create_buffer(size_t size) {
       Buffer *buf = (Buffer *)malloc(sizeof(Buffer));
       if (buf) {
           buf->data = (char *)malloc(size);
       }
       return buf;
   }
   void destroy_buffer(Buffer *buf) {
       if (buf) {
           free(buf->data);
           free(buf);
       }
   }

Create ``utils.h``:

.. code-block:: c

   #ifndef UTILS_H
   #define UTILS_H
   #include <stddef.h>
   typedef struct {
       char *data;
   } Buffer;
   Buffer *create_buffer(size_t size);
   void destroy_buffer(Buffer *buf);
   #endif

Create ``main.c``:

.. code-block:: c

   #include <stdio.h>
   #include "utils.h"
   int main() {
       Buffer *buf = create_buffer(1024);
       if (buf) {
           destroy_buffer(buf);
       }
       return 0;
   }

.. code-block:: bash

   # Compile each file
   clang -emit-llvm -c -g utils.c -o utils.bc
   clang -emit-llvm -c -g main.c -o main.bc
   # Link into single module
   llvm-link utils.bc main.bc -o program.bc

.. code-block:: bash

   # Interprocedural alias analysis
   ./build/bin/lotus-alias-aser-aa -analysis-mode=1-cfa -dump-stats program.bc
   # Interprocedural abstract interpretation
   ./build/bin/clam --crab-inter --crab-dom=zones \
       --crab-check --crab-null-check --crab-bounds-check program.bc

Inspect how allocations and frees propagate across calls;
results depend on the selected analysis and models.

Tutorial 9: Concurrency Analysis
---------------------------------

Create ``concurrent.c``:

.. code-block:: c

   #include <pthread.h>
   #include <stdio.h>
   int shared_counter = 0;
   pthread_mutex_t lock;
   void *increment_thread(void *arg) {
       for (int i = 0; i < 1000; i++) {
           pthread_mutex_lock(&lock);
           shared_counter++;  // Protected
           pthread_mutex_unlock(&lock);
       }
       return NULL;
   }
   void *buggy_thread(void *arg) {
       for (int i = 0; i < 1000; i++) {
           shared_counter++;  // Bug: race condition
       }
       return NULL;
   }
   int main() {
       pthread_t t1, t2;
       pthread_mutex_init(&lock, NULL);
       pthread_create(&t1, NULL, increment_thread, NULL);
       pthread_create(&t2, NULL, buggy_thread, NULL);
       pthread_join(t1, NULL);
       pthread_join(t2, NULL);
       printf("Counter: %d\n", shared_counter);
       pthread_mutex_destroy(&lock);
       return 0;
   }

.. code-block:: bash

   clang -emit-llvm -c -g concurrent.c -o concurrent.bc
   ./build/bin/lotus-check --engine=concur concurrent.bc --verbose

Best Practices
--------------

Start with fast analyses, increase precision as needed, and review findings
against the program's assumptions. Dynamic traces cover only observed executions.

Next Steps
----------

- Explore :doc:`../tools/index` for detailed tool documentation
- Read :doc:`architecture` to understand the framework design
- See :doc:`../developer/developer_guide` to extend Lotus with custom analyses
- Check :doc:`../developer/api_reference` for programmatic usage
