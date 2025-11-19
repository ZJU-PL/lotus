SSI — Static Single Information
===============================

Overview
========

**Static Single Information (SSI)** is an extension of SSA that augments
variables with additional predicate and path information.
Its goal is to give analyses a more precise view of conditional value
relationships than plain SSA.

* **Location**: ``lib/IR/SSI/``, ``include/IR/SSI/``

Key Features
============

* Extends SSA with **predicate information** (e.g., conditions guarding
  a definition or use).
* Encodes relationships between values across different control-flow
  paths more explicitly.
* Serves as a basis for advanced optimizations and analyses that need
  path- and condition-sensitive reasoning.


