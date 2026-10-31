Verification Backend API
========================

``include/Verification/Driver/Backend.h`` and ``lib/Verification/Driver/Backend.cpp``
define the shared abstraction used to invoke different verification engines
through one API.

**Main components**:

- ``PropertyClass`` and ``VerificationTask`` describe the requested job.
- ``VerificationResult`` and ``VerificationResultInfo`` store standardized results.
- ``IDriver`` is the driver interface (namespace ``lotus::verification::driver``).
- ``DriverRegistry`` manages available implementations.

The built-in registry covers SeaHorn, Sifa, SymAbsAI, and Clam:
``DriverRegistry::availableDrivers()`` returns ``{"seahorn", "sifa",
"symabs_ai", "clam"}``.

Backend lifecycle
-----------------

A frontend describes the property and input as a ``VerificationTask``, selects
an implementation from ``DriverRegistry``, and receives a normalized
``VerificationResultInfo``.  This separation lets callers present one result
format even when engines have different command lines or witness formats.
Backend implementations should report unsupported tasks explicitly rather than
silently weakening the requested property.

``DriverRegistry::create`` instantiates an ``IDriver`` by name, and
``DriverRegistry::recommend`` lists the drivers that support a given
``PropertyClass``.  Each ``IDriver`` exposes ``name``, ``supports``,
``buildCommand``, and ``parseResult``.

See also :doc:`index` and :doc:`../user_guide/verification_backends`.
