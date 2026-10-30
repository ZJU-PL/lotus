#pragma once

#include "Dataflow/DemandAPA/Support.h"

class RegEx {
public:
  // to create a regular expression 0, RegEx(RegEx::ZERO)
  // to create a regular expression 1, RegEx(RegEx::ONE)
  // to create a regular expression (re)* [where re is another RegEx],
  // RegEx(RegEx::STAR, re, nullptr) to create a regular expression re1 + re2
  // [where re1, re2 are other RegExs], RegEx(RegEx::PLUS, re1, re2) to create a
  // regular expression re1 . re2 [where re1, re2 are other RegExs],
  // RegEx(RegEx::DOT, re1, re2) to create a regular expression Project_{c}(re)
  // [where re is a RegEx], RegEx(RegEx::PROJECT, new RegEx(c), re)
  const static int ZERO = -1, ONE = -2, PLUS = -3, DOT = -4, STAR = -5,
                   PROJECT = -6;
  // For variables X_0, X_1 ..., we represent X_i with -7-i

  RegEx *L;
  RegEx *R;
  ll eId;

  void copy(RegEx *other) {
    L = other->L;
    R = other->R;
    eId = other->eId;
  }

  RegEx(ll _eId) : L(nullptr), R(nullptr), eId(_eId) {}

  RegEx(ll type, RegEx *_L, RegEx *_R) {
    eId = type;
    L = _L;
    R = _R;
    if (eId == PLUS) {
      if (L->eId == ZERO)
        copy(R);
      else if (R->eId == ZERO)
        copy(L);
      else if ((L->eId == ONE || L->eId >= 0) &&
               L->eId == R->eId) // 1 + 1 = 1, and e + e = e
        copy(L);
    } else if (eId == DOT) {
      if (L->eId == ZERO or R->eId == ZERO)
        eId = ZERO, L = R = nullptr;
      else if (L->eId == ONE)
        copy(R);
      else if (R->eId == ONE)
        copy(L);
    } else if (eId == STAR) {
      if (L->eId == ONE or L->eId == ZERO)
        eId = ONE, L = R = nullptr;
      else if (L->eId == STAR)
        copy(L);
    }
  }

  friend auto operator<<(ostream &os, RegEx const &m) -> ostream & {
    if (m.eId == ZERO)
      os << "0";
    else if (m.eId == ONE)
      os << "1";
    else if (m.eId == STAR) {
      if (m.L->eId == PLUS or m.L->eId == DOT)
        os << (*(m.L)) << "*";
      else
        os << "(" << (*(m.L)) << ")*";
    } else if (m.eId == PLUS) {
      os << "(" << (*(m.L)) << " + " << (*(m.R)) << ")";
    } else if (m.eId == DOT) {
      os << "(" << (*(m.L)) << " . " << (*(m.R)) << ")";
    } else if (m.eId == PROJECT) {
      os << "P_" << (m.L->eId) << "(" << (*(m.R)) << ")";
    } else if (m.eId < 0) {
      os << "X_" << int(-(m.eId + 7));
    } else {
      int u = m.eId >> 32, v = m.eId & FIRST_32BIT;
      os << "<" << u << ", " << v << ">";
    }
    return os;
  }

  /*	~RegEx() {
                  if (L != nullptr)
                          delete L;
                  if (R != nullptr)
                          delete R;
          }*/
};

