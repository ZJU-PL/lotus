/*
 *
 * Author: rainoftime
*/
#include "Concurrency/Core/BVClock.h"
#include "Concurrency/Core/FBVClock.h"

#include <algorithm>
#include <stdexcept>
#include <string>

BVClock BVClock::operator+(const BVClock &vc) const{
  BVClock rv;
  if(vec.size() < vc.vec.size()){
    rv.vec = vc.vec;
  }else{
    rv.vec = vec;
  }
  const std::size_t m = std::min(vc.vec.size(),vec.size());
  for(std::size_t i = 0; i < m; ++i){
    rv.vec[i] = vec[i] || vc.vec[i];
  }
  return rv;
}

std::string BVClock::to_string() const{
  if(vec.size()){
    std::string s;
    s.resize(2*vec.size()+1,',');
    s[0] = '[';
    s[s.size()-1] = ']';
    for(std::size_t i = 0; i < vec.size(); ++i){
      s[2*i+1] = vec[i] ? '1' : '0';
    }
    return s;
  }
  return "[]";
}

bool BVClock::leq(const BVClock &vc) const{
  const std::size_t m = std::min(vec.size(),vc.vec.size());
  for(std::size_t i = 0; i < m; ++i){
    if(vec[i] && !vc.vec[i]) return false;
  }
  for(unsigned i = vc.vec.size(); i < vec.size(); ++i){
    if(vec[i]) return false;
  }
  return true;
}

bool BVClock::lt(const BVClock &vc) const{
  return leq(vc) && *this != vc;
}

bool BVClock::gt(const BVClock &vc) const{
  return vc.lt(*this);
}

bool BVClock::geq(const BVClock &vc) const{
  return vc.leq(*this);
}

bool BVClock::operator==(const BVClock &vc) const{
  return leq(vc) && vc.leq(*this);
}

bool BVClock::operator!=(const BVClock &vc) const{
  return !(*this == vc);
}

bool BVClock::operator<(const BVClock &vc) const{
  const std::size_t lhs_size = effectiveSize();
  const std::size_t rhs_size = vc.effectiveSize();
  return std::lexicographical_compare(vec.begin(), vec.begin() + lhs_size,
                                      vc.vec.begin(), vc.vec.begin() + rhs_size);
}

bool BVClock::operator<=(const BVClock &vc) const{
  return !(vc < *this);
}

bool BVClock::operator>(const BVClock &vc) const{
  return vc < *this;
}

bool BVClock::operator>=(const BVClock &vc) const{
  return !(*this < vc);
}

BVClock &BVClock::operator+=(BVClock &&vc) {
  return *this += static_cast<const BVClock &>(vc);
}

BVClock &BVClock::operator=(const FBVClock &vc){
  vec = vc.snapshot();
  return *this;
}

BVClock &BVClock::operator+=(const FBVClock &vc){
  const std::vector<bool> snapshot = vc.snapshot();
  if(vec.size() < snapshot.size()){
    vec.resize(snapshot.size(),false);
  }
  for(std::size_t i = 0; i < snapshot.size(); ++i){
    vec[i] = vec[i] || snapshot[i];
  }
  return *this;
}

void BVClock::validateIndex(int d) {
  if (d < 0)
    throw std::out_of_range("BVClock dimension must be non-negative");
}

std::size_t BVClock::effectiveSize() const {
  std::size_t size = vec.size();
  while (size > 0 && !vec[size - 1])
    --size;
  return size;
}
