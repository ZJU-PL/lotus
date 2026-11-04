#pragma once

#include <cassert>
#include <memory>
#include <set>
#include <vector>

#include <llvm/Support/MathExtras.h>

namespace lotus::gsaf {

// A mask key can relate to multiple values
template <typename V> class MultiMaskMap {
private:
  typedef int MaskTy;
  std::vector<std::pair<MaskTy, std::unique_ptr<std::set<V>>>> InternalTable;

public:
  void insert(MaskTy MaskKey, V Val) {
    bool Found = false;
    for (auto &Pair : InternalTable) {
      if (MaskKey == Pair.first) {
        Found = true;
        Pair.second->insert(Val);
        break;
      }
    }

    if (!Found) {
      auto VSet = std::make_unique<std::set<V>>();
      VSet->insert(Val);
      InternalTable.emplace_back(MaskKey, std::move(VSet));
    }
  }

  typedef std::vector<std::pair<MaskTy, std::set<V> *>> ResultVecTy;
  std::shared_ptr<ResultVecTy> find(MaskTy MaskKey) const {
    std::shared_ptr<ResultVecTy> Results = std::make_shared<ResultVecTy>();

    for (auto &Pair : InternalTable) {
      MaskTy Match = MaskKey & Pair.first;
      if (Match) {
        Results->push_back(std::make_pair(Match, Pair.second.get()));
      }
    }

    return Results;
  }

  bool contains(MaskTy MaskKey) const {
    for (auto &Pair : InternalTable) {
      MaskTy Match = MaskKey & Pair.first;
      if (Match) {
        return true;
      }
    }

    return false;
  }

  void clear() { InternalTable.clear(); }
};

// A mask key only relates to one value
template <typename V> class MaskMap {
private:
  typedef int MaskTy;
  std::vector<std::pair<MaskTy, V>> InternalTable;

  unsigned SlotMask;

public:
  MaskMap() {
    // 1: empty; 0: occupied
    SlotMask = ~0u;
    InternalTable.resize(sizeof(MaskTy) * 8);
  }

  ~MaskMap() {}

  void insert(MaskTy MaskKey, V Val) {
    bool Inserted = false;
    for (unsigned MaskCopy = ~SlotMask,
                  OccupiedBit = MaskCopy & (~MaskCopy + 1u);
         MaskCopy != 0; MaskCopy = MaskCopy - OccupiedBit,
                  OccupiedBit = MaskCopy & (~MaskCopy + 1u)) {
      int Pos = llvm::countTrailingZeros(OccupiedBit) + 1;
      assert(Pos > 0);
      auto &Pair = InternalTable[Pos - 1];

      if (Val == Pair.second) {
        Pair.first |= MaskKey;
        Inserted = true;
      } else {
        if (MaskKey & Pair.first) {
          Pair.first &= ~MaskKey;

          if (!Pair.first) {
            SlotMask |= OccupiedBit;
          }
        }
      }
    }

    if (!Inserted) {
      assert(SlotMask != 0 && "No free mask slots");
      int Pos = llvm::countTrailingZeros(SlotMask) + 1;
      assert(Pos > 0);
      InternalTable[Pos - 1] = std::make_pair(MaskKey, Val);
      SlotMask &= ~(1u << (Pos - 1));
    }
  }

  typedef std::vector<std::pair<MaskTy, V>> ResultVecTy;
  std::shared_ptr<ResultVecTy> find(MaskTy MaskKey) const {
    std::shared_ptr<ResultVecTy> Results = std::make_shared<ResultVecTy>();

    for (unsigned MaskCopy = ~SlotMask,
                  OccupiedBit = MaskCopy & (~MaskCopy + 1u);
         MaskCopy != 0; MaskCopy = MaskCopy - OccupiedBit,
                  OccupiedBit = MaskCopy & (~MaskCopy + 1u)) {
      int Pos = llvm::countTrailingZeros(OccupiedBit) + 1;
      assert(Pos > 0);
      auto &Pair = InternalTable[Pos - 1];
      MaskTy Match = MaskKey & Pair.first;
      if (Match) {
        Results->push_back(std::make_pair(Match, Pair.second));
      }
    }

    return Results;
  }

  void find(MaskTy MaskKey, ResultVecTy &Results, V DefautlVal) const {
    MaskTy UnmathcedMask = MaskKey;
    for (unsigned MaskCopy = ~SlotMask,
                  OccupiedBit = MaskCopy & (~MaskCopy + 1u);
         MaskCopy != 0; MaskCopy = MaskCopy - OccupiedBit,
                  OccupiedBit = MaskCopy & (~MaskCopy + 1u)) {
      int Pos = llvm::countTrailingZeros(OccupiedBit) + 1;
      assert(Pos > 0);
      auto &Pair = InternalTable[Pos - 1];
      MaskTy Match = MaskKey & Pair.first;
      if (Match) {
        Results.emplace_back(Match, Pair.second);
        UnmathcedMask &= ~Match;
      }
    }
    if (UnmathcedMask) {
      Results.emplace_back(UnmathcedMask, DefautlVal);
    }
  }

  void clear() {
    InternalTable.clear();
    InternalTable.resize(sizeof(MaskTy) * 8);
    SlotMask = ~0u;
  }
};

} // namespace lotus::gsaf
