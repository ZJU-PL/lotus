#ifndef SUPPORT_STATISTICS_H
#define SUPPORT_STATISTICS_H

#include <llvm/IR/Module.h>

using namespace llvm;

class Statistics {
public:
    static void run(Module &);
};

#endif //SUPPORT_STATISTICS_H
