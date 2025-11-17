OWL – SMT/Model Checking Front-End
===================================

``owl`` is a lightweight front-end for feeding SMT-LIB2 problems to the
configured SMT solver (Z3 in the default build).

**Binary**: ``owl``  
**Location**: ``tools/owl/owl.cpp``

**Note**: This tool is optional and must be enabled with ``BUILD_OWL=ON`` during
CMake configuration.

**Usage**:

.. code-block:: bash

   ./build/bin/owl file.smt2

**Example**:

.. code-block:: bash

   ./build/bin/owl examples/solver/example.smt2

See :doc:`../../solvers/smt_model_checking` for details about the solver stack.

