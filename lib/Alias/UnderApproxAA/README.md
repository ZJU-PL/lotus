# Under-Approximation Alias Analysis

Sound but incomplete alias analysis for identifying **must-alias** relationships.

## Design

1. **Atomic Rules**: Local, syntactic patterns that prove must-alias
2. **Propagation**: Union-find transitively propagates equivalences

## Files

- `UnderApproxAA.cpp` - AAResult interface, per-function caching
- `EquivDB.cpp` - Equivalence database with union-find
- `Canonical.cpp` - Pointer canonicalization helpers

## Atomic Rules

| # | Rule Name | Description | Example |
|---|-----------|-------------|---------|
| 1 | Identity | Same SSA value | `%p` and `%p` |
| 2 | Cast Equivalence | Bitcast or no-op addrspace cast | `%p` and `bitcast %p to i8*` |
| 3 | Constant Offset GEP | Same base + identical offsets | `GEP(%base, 0, i)` and `GEP(%base, 0, i)` |
| 4 | Zero GEP | All-zero indices same as base | `GEP(%p, 0, 0)` and `%p` |
| 5 | Round-Trip Cast | ptr→int→ptr with no arithmetic | `inttoptr(ptrtoint(%p))` and `%p` |
| 6 | Same Underlying Object | Same alloca/global via casts/GEPs | `bitcast(%alloca)` and `GEP(%alloca, 0)` |
| 7 | Constant Null | Null pointers in same address space | `null` and `null` |
| 8 | Trivial PHI | All incoming values are identical | `phi [%p, %bb1], [%p, %bb2]` and `%p` |
| 9 | Trivial Select | Both branches produce same value | `select %c, %p, %p` and `%p` |

## Adding New Rules

1. Create checker in `EquivDB.cpp`:
```cpp
/// Rule X: Brief Description
/// Example: concrete LLVM IR
static bool checkNewRule(const Value *S1, const Value *S2) {
  // Return true if must-alias, false otherwise
}
```

2. Call from `atomicMustAlias()`:
```cpp
if (checkNewRule(S1, S2))  return true;
```

3. Update table above

**Requirements**: Sound (no false positives), local, syntactic, fast

## Example

```llvm
%a = alloca i32
%b = bitcast i32* %a to i8*
%p = getelementptr i8* %b, 0
```
Result: `%a ≡ %b ≡ %p` (Rule 2 + Rule 4, propagated via union-find)

## Limitations

- Intra-procedural only (no inter-procedural reasoning)
- Stack/global allocations only (no heap)
- Syntactic patterns only (no arithmetic or semantic reasoning)

