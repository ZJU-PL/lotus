#include "Checker/kint/Utils.h"

namespace kint {
namespace utils {

int64_t getInstructionId(llvm::Instruction *Inst) {
    std::string str;
    llvm::raw_string_ostream rso(str);
    Inst->print(rso);
    auto inst_str = llvm::StringRef(rso.str());
    auto ids = inst_str.trim().split(' ').first;
    size_t v = 0;
    for (auto c : ids) {
        if (!std::isdigit(c))
            continue;
        v = v * 10 + c - '0';
    }
    return v;
}

} // namespace utils

int64_t get_id(llvm::Instruction* inst) {
    return utils::getInstructionId(inst);
}

} // namespace kint
