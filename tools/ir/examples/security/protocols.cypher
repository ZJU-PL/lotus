# Lotus-specific protocol candidates (these are not CodeQL migration IDs).
# lotus-ir-pdg-query input.bc --analysis rules --rule lotus/shared-global-race --rule lotus/lock-order-cycle --rule lotus/filesystem-toctou --format json
# --protocol-state-limit controls each worker/check/cycle search (default 4096;
# zero disables the budget). Inspect state_limit_hit and notes in JSON.
# Concurrency requires directly registered pthread workers launched in the
# same basic block without a join between them. Exact shared-global byte
# ranges, atomics and blocking-mutex locksets are modeled. Dynamic aliases,
# parent-thread effects, helper summaries and full happens-before are excluded.
# TOCTOU follows CFG check/use events on stable paths, not source-line order.
# Check/open API presence alone is not a finding. General path feasibility,
# symlink resolution and descriptor/openat-relative protocols remain partial.

MATCH (c:INST_FUNCALL) WHERE c.callee IN ['pthread_create','pthread_join','pthread_mutex_lock','pthread_mutex_unlock'] RETURN c.callee AS api, c.func AS function, c.src AS location, c

MATCH (n:INST) WHERE n.opcode IN ['load','store','atomicrmw','cmpxchg','fence'] RETURN n.opcode AS operation, n.func AS function, n.src AS location, n

MATCH (c:INST_FUNCALL) WHERE c.callee IN ['access','stat','lstat','open','fopen','unlink','chmod','chown'] RETURN c.callee AS api, c.src AS location, c
