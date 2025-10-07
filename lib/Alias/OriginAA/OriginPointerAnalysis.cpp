#include "Alias/OriginAA/OriginPointerAnalysis.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/FileSystem.h"
#include "Alias/OriginAA/Flags.h"
#include <fstream>
#include "Support/cJSON.h"
#include "Alias/OriginAA/Util.h"

using namespace llvm;

static bool isThreadRelatedCallInstruction(const llvm::Value *callsite)
{
    if (!callsite)
        return false;

    if (const auto *call = dyn_cast<CallBase>(callsite))
    {
        if (const Function *callee = call->getCalledFunction())
        {
            std::string demangled = getDemangledName(callee->getName().str());

            if (DebugMode)
                errs() << "Demangled name: " << demangled << "\n";

            // TODO: Add more thread-related functions if needed
            return (demangled == "std::thread::spawn" ||
                    demangled == "tokio::task::spawn::spawn");
        }
    }
    return false;
}

Context OriginPointerAnalysis::getContext(Context context, const Value *newCallSite)
{
    if (isThreadRelatedCallInstruction(newCallSite))
    {
        Context newContext = context;
        newContext.values.push_back(newCallSite);
        if (newContext.values.size() > K)
        {
            newContext.values.pop_front();
        }

        // if (DebugMode)
        errs() << "HIT! New origin context for call site: " << newContext << "\n";

        return newContext;
    }
    return context;
}