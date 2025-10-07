#include "Fuzzing/Analysis/DAFL.hpp"
#include "Fuzzing/Analysis/TargetDetection.hpp"

#include <llvm/ADT/SmallSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/SetVector.h>
#include <llvm/ADT/StringMap.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/ADT/StringSet.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/FormatVariadic.h>
#include <llvm/Support/VirtualFileSystem.h>

#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace llvm;

AnalysisKey DAFLAnalysis::Key;

DAFLAnalysis::Result
DAFLAnalysis::readFromFile(Module &M, std::unique_ptr<MemoryBuffer> &Buffer) {
  SmallVector<StringRef, 0> AllLines;
  Buffer->getBuffer().split(AllLines, '\n');

  // The input file may contain multiple scores for the same file:line. This can
  // happen for example with ternary operators written in a single line. It
  // could also happen when linking multiple programs in parallel against the
  // same target library (i.e. specified target locations are in the library);
  // in this case make sure to use a unique DAFL file for each linker
  // invocation.
  // To  handle the general case we sum the scores for all file:line targets
  // instead of taking the max score over all file:line targets.
  StringMap<int> Scores;
  for (auto &Line : AllLines) {
    auto Trimmed = Line.trim();
    if (Trimmed.empty()) {
      continue;
    }

    // We expect a score followed by a file name, colon, line number.
    // For example: "42,foo.c:10"
    auto Parts = Trimmed.split(',');
    auto Score = Parts.first;
    auto FileLine = Parts.second;

    // If we didn't have a comma, then we are reading the old DAFL format.
    // In the old format, the file:line was first, then a colon, then the
    // score. For example: "foo.c:10:42"
    auto UseNewFormat = !Score.empty() && !FileLine.empty();
    if (!UseNewFormat) {
      auto Parts2 = Trimmed.rsplit(':');
      FileLine = Parts2.first;
      Score = Parts2.second;
    }

    int ScoreInt;
    if (Score.getAsInteger(10, ScoreInt)) {
      report_fatal_error("Invalid score in DAFL file");
    }

    if (!Scores.count(FileLine)) {
      Scores[FileLine] = ScoreInt;
    } else {
      Scores[FileLine] += ScoreInt;
    }
  }

  if (Scores.empty()) {
    report_fatal_error("No scores found in DAFL file");
  }

  StringMap<int> AggregatedScores;

  // Aggregate scores at the basic block level.
  // This is a map from file:line to the sum of all scores in the DAFL file for
  // that file:line.
  for (auto &Entry : Scores) {
    auto FileLine = Entry.first();
    auto Score = Entry.second;
    AggregatedScores[FileLine] = Score;
  }

  int MaxScore = 0;
  for (auto &KV : AggregatedScores) {
    if (KV.second > MaxScore) {
      MaxScore = KV.second;
    }
  }

  Result Res = Result::value_type();

  for (auto &F : M) {
    for (auto &BB : F) {
      for (auto &I : BB) {
        if (auto *DL = I.getDebugLoc().get()) {
          auto Line = DL->getLine();
          auto File = DL->getFilename();
          auto FileLine = formatv("{0}:{1}", File, Line).str();
          if (AggregatedScores.count(FileLine)) {
            auto Score = AggregatedScores[FileLine];
            // Invert score so that lower distance = higher score
            auto Distance = static_cast<DAFLAnalysis::WeightTy>(MaxScore - Score + 1);
            if (Res->find(&BB) == Res->end()) {
              Res->insert({&BB, Distance});
            } else {
              // Take the minimum distance if we have multiple
              (*Res)[&BB] = std::min(Distance, (*Res)[&BB]);
            }
          }
        }
      }
    }
  }

  return Res;
}

DAFLAnalysis::Result DAFLAnalysis::run(Module &M, ModuleAnalysisManager &MAM) {
  if (!InputFile.empty()) {
    auto VFS = vfs::getRealFileSystem();
    auto BufferOrErr = VFS->getBufferForFile(InputFile);
    if (auto EC = BufferOrErr.getError()) {
      auto ErrorMessage = formatv("can't open DAFL input file '{0}': {1}",
                                  InputFile, EC.message());
      report_fatal_error(ErrorMessage);
    }

    errs() << "[DAFL] input file: " << InputFile << '\n';
    return readFromFile(M, *BufferOrErr);
  }

  // get the target instructions
  SetVector<const Instruction *> TargetIs;
  auto &FAM = MAM.getResult<FunctionAnalysisManagerModuleProxy>(M).getManager();
  for (auto &F : M) {
    auto &FTargets = FAM.getResult<AFLGoTargetDetectionAnalysis>(F);
    TargetIs.insert(FTargets.Is.begin(), FTargets.Is.end());
  }

  if (TargetIs.empty()) {
    if (NoTargetsNoError) {
      return {};
    }
    report_fatal_error("No target instructions found from target detection");
  }

  // Simplified distance calculation without SVF
  // Assign distance 1 to basic blocks containing target instructions
  Result Res = Result::value_type();
  
  for (const auto *I : TargetIs) {
    auto *BB = I->getParent();
    (*Res)[BB] = 1;
  }

  if (Verbose) {
    errs() << "[DAFL] Found " << TargetIs.size() << " target instructions in "
           << Res->size() << " basic blocks (simplified mode without SVF)\n";
  }

  return Res;
}