# Resource semantics: use --analysis rules for object/CFG lifetime findings.
# lotus-ir-pdg-query input.bc --analysis rules --cwe 775,764,833 --format json
# --rule cpp/file-never-closed --rule cpp/file-may-not-be-closed
# --rule cpp/descriptor-never-closed --rule cpp/descriptor-may-not-be-closed
# --rule cpp/double-release --rule cpp/unreleased-lock --rule cpp/twice-locked
# --rule cpp/jpl-c/release-locks-when-acquired
# ResourceFlowQuery's API-pair summaries are exploratory. C++ new/delete are
# handled by native lifetime/family models, not its C resource API table.
# shutdown is not generally equivalent to close; inspect FD candidates using
# native descriptor rules. mmap/munmap exploration does not prove region safety.

MATCH (c:INST_FUNCALL) WHERE c.callee IN ['fopen','fclose','popen','pclose','open','close','socket','accept','opendir','closedir','mmap','munmap'] RETURN c.callee AS api, c.func AS function, c.src AS location, c

# Lock sites alone do not prove deadlock or pairing errors. Cross-worker
# lock-order candidates: --analysis rules --rule lotus/lock-order-cycle
MATCH (c:INST_FUNCALL) WHERE c.callee IN ['pthread_mutex_lock','pthread_mutex_unlock','pthread_mutex_trylock','mtx_lock','mtx_unlock'] RETURN c.callee AS api, c.func AS function, c.src AS location, c

# API co-occurrence is an inventory, without identity, order or pairing claims.
MATCH (a:INST_FUNCALL {callee:'fopen'}) MATCH (b:INST_FUNCALL {callee:'fclose'}) WHERE a.func = b.func RETURN a, b
