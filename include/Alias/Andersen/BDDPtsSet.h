/*

FIXME: This is a temporary implementation of the "BDD points-to set", which actually uses a SparseBitVector.
It should be replaced with a proper BDD implementation.
*/


#ifndef ANDERSEN_BDDPTSSET_H
#define ANDERSEN_BDDPTSSET_H

#include <llvm/ADT/SparseBitVector.h>

// A points-to set representation that uses SparseBitVector for now
// but would be replaced with BDD for better performance
class BDDAndersPtsSet {
private:
  // Use a bitvector for now to make things build
  llvm::SparseBitVector<> bitvec;

public:
  using iterator = llvm::SparseBitVector<>::iterator;

  // Return true if *this has idx as an element
  bool has(unsigned idx) { return bitvec.test(idx); }
  bool has(unsigned idx) const {
    // Since llvm::SparseBitVector::test() does not have a const quantifier, we
    // have to use this ugly workaround to implement has()
    llvm::SparseBitVector<> idVec;
    idVec.set(idx);
    return bitvec.contains(idVec);
  }

  // Return true if the ptsset changes
  bool insert(unsigned idx) { return bitvec.test_and_set(idx); }

  // Return true if *this is a superset of other
  bool contains(const BDDAndersPtsSet &other) const {
    return bitvec.contains(other.bitvec);
  }

  // intersectWith: return true if *this and other share points-to elements
  bool intersectWith(const BDDAndersPtsSet &other) const {
    return bitvec.intersects(other.bitvec);
  }

  // Return true if the ptsset changes
  bool unionWith(const BDDAndersPtsSet &other) { return bitvec |= other.bitvec; }

  void clear() { bitvec.clear(); }

  unsigned getSize() const {
    return bitvec.count(); // NOT a constant time operation!
  }

  // Always prefer using this function to perform empty test
  bool isEmpty() const {
    return bitvec.empty();
  }

  bool operator==(const BDDAndersPtsSet &other) const {
    return bitvec == other.bitvec;
  }

  iterator begin() const { return bitvec.begin(); }
  iterator end() const { return bitvec.end(); }
};

#endif // ANDERSEN_BDDPTSSET_H 