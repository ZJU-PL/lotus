#include "CFL/Certificates/DyckCertificates.h"
#include <gtest/gtest.h>
#include <cassert>
#include <iostream>
#include <random>

namespace {
using namespace cflcert;
static int runImportedTest() {std::mt19937_64 r(7);for(int n=1;n<=100;n+=7){Matrix a((size_t)n*n),b((size_t)n*n),ref((size_t)n*n);for(auto&x:a)x=r()%10;for(auto&x:b)x=r()%10;for(int i=0;i<n;++i)for(int k=0;k<n;++k)for(int j=0;j<n;++j)ref[(size_t)i*n+j]+=a[(size_t)i*n+k]*b[(size_t)k*n+j];assert(multiply(a,b,n)==ref);assert(multiplyFastExact(a,b,n,1)==ref);}
// Strassen's signed intermediate sums can exceed uint64_t even when the
// final product fits; arbitrary-precision intermediates must preserve it.
{uint64_t x=std::numeric_limits<uint64_t>::max();Matrix a={x,x,x,x},b={1,0,0,1};assert(multiply(a,b,2)==a);}
bool overflow=false;try{Matrix x={std::numeric_limits<uint64_t>::max()},y={2};(void)multiply(x,y,1);}catch(const std::overflow_error&){overflow=true;}assert(overflow);std::cout<<"exact Strassen tests passed\n";    return 0;
}

} // namespace

TEST(CFLCertificates, MatrixBackend) {
    EXPECT_EQ(runImportedTest(), 0);
}
