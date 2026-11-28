#pragma once

#include <algorithm>
#include <string>
#include <vector>

namespace concurrency {

enum class ProofStrength {
  Must,
  May,
  Unknown
};

enum class RelationKind {
  MustHappenBefore,
  SelectiveHappenBefore,
  MayHappenBefore,
  MutuallyExclusive,
  MatchedCommunication,
  MPICollectiveParticipation,
  MPICollectiveLocalCompletion,
  MPIGlobalBarrier,
  MPIRequestCompletion,
  MPICommunicatorProvenance,
  SameSynchronizationEpoch,
  SameCollectiveFrontier,
  SameProtocolSlot,
  DisjointParticipants,
  LocalOnlySynchronizationCompletion,
  UnknownDueToModelGap
};

struct Relation {
  RelationKind kind = RelationKind::UnknownDueToModelGap;
  ProofStrength proof = ProofStrength::Unknown;
  std::string reason;
  std::vector<std::string> evidence_reasons;
};

inline int proofUncertaintyPriority(ProofStrength proof) {
  switch (proof) {
  case ProofStrength::Must:
    return 0;
  case ProofStrength::May:
    return 1;
  case ProofStrength::Unknown:
    return 2;
  }
  return 2;
}

inline void addRelationEvidence(Relation &relation, const std::string &reason) {
  if (reason.empty() ||
      std::find(relation.evidence_reasons.begin(),
                relation.evidence_reasons.end(), reason) !=
          relation.evidence_reasons.end()) {
    return;
  }
  relation.evidence_reasons.push_back(reason);
}

inline void mergeSameKindRelation(Relation &retained,
                                  const Relation &incoming) {
  addRelationEvidence(retained, retained.reason);
  addRelationEvidence(retained, incoming.reason);
  for (const std::string &reason : incoming.evidence_reasons) {
    addRelationEvidence(retained, reason);
  }
  if (proofUncertaintyPriority(incoming.proof) >
      proofUncertaintyPriority(retained.proof)) {
    retained.proof = incoming.proof;
    retained.reason = incoming.reason;
  }
}

inline int relationPriority(RelationKind kind) {
  switch (kind) {
  case RelationKind::MustHappenBefore:
    return 6;
  case RelationKind::SelectiveHappenBefore:
    return 5;
  case RelationKind::MutuallyExclusive:
    return 4;
  case RelationKind::MatchedCommunication:
    return 4;
  case RelationKind::MPICollectiveParticipation:
    return 4;
  case RelationKind::MPICollectiveLocalCompletion:
    return 4;
  case RelationKind::MPIGlobalBarrier:
    return 4;
  case RelationKind::MPIRequestCompletion:
    return 4;
  case RelationKind::MPICommunicatorProvenance:
    return 4;
  case RelationKind::SameSynchronizationEpoch:
    return 3;
  case RelationKind::SameCollectiveFrontier:
    return 3;
  case RelationKind::SameProtocolSlot:
    return 3;
  case RelationKind::DisjointParticipants:
    return 3;
  case RelationKind::LocalOnlySynchronizationCompletion:
    return 3;
  case RelationKind::MayHappenBefore:
    return 2;
  case RelationKind::UnknownDueToModelGap:
    return 1;
  }
  return 0;
}

} // namespace concurrency
