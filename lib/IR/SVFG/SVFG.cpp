//===- SVFG.cpp -- SVFG Implementation --------------------------------------//
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
//===----------------------------------------------------------------------===//

#include "IR/SVFG/SVFG.h"

#include "IR/SVFG/SVFGEdge.h"
#include "IR/SVFG/SVFGNode.h"
#include "IR/SVFG/SVFGSerializer.h"

#include <limits>
#include <queue>
#include <unordered_set>

#include <llvm/Support/Casting.h>
#include <llvm/IR/Operator.h>

using namespace lotus::analysis;
using namespace llvm;

SVFGNodeBS SVFG::expandObjectsForAlias(const SVFGNodeBS &objects,
                                      const Value *pointer) const {
  // Lotus uses the base ID for offset zero. A direct first-field GEP still
  // denotes that field, whereas SVF represents it with a separate GepObjVar.
  // Do not turn that field query into an aggregate-wide query.
  const Value *origin = pointer;
  while (origin) {
    if (const auto *cast = dyn_cast<CastInst>(origin))
      origin = cast->getOperand(0);
    else if (const auto *constant = dyn_cast<ConstantExpr>(origin)) {
      if (!constant->isCast())
        break;
      origin = constant->getOperand(0);
    } else
      break;
  }
  const bool fieldPointer = isa_and_nonnull<GEPOperator>(origin);
  SVFGNodeBS expanded = objects;
  for (uint32_t object : objects) {
    const ObjectInfo *info = getObjectInfo(object);
    if (isFieldInsensitiveObject(object) ||
        (!fieldPointer &&
         (!info || info->baseObjId == 0 || info->baseObjId == object))) {
      const auto fields = getFieldObjects(object);
      expanded.insert(fields.begin(), fields.end());
    }
  }
  return expanded;
}

// Forward declarations for stat helpers defined later in this file.
static void adjustNodeStat(SVFGStat &stat, SVFGNode *node, int delta);
static void adjustEdgeStat(SVFGStat &stat, SVFGEdge *edge, int delta);

const llvm::Function *SVFGNode::getFunction() const {
  if (icfgNode) {
    return icfgNode->getFunction();
  }
  return nullptr;
}

std::string SVFGNode::toString() const {
  std::string str;
  llvm::raw_string_ostream os(str);
  os << "SVFGNode ID: " << getId()
     << " Kind: " << static_cast<uint32_t>(getNodeKind());
  if (const auto *val = getValue()) {
    os << " Value: " << val->getName().str();
  }
  return os.str();
}

std::string SVFGEdge::toString() const {
  std::string result;
  switch (kind) {
  case SVFGEdgeK::IntraCopy:
    result = "IntraCopy";
    break;
  case SVFGEdgeK::IntraDirect:
    result = "IntraDirect";
    break;
  case SVFGEdgeK::IntraLoad:
    result = "IntraLoad";
    break;
  case SVFGEdgeK::IntraStore:
    result = "IntraStore";
    break;
  case SVFGEdgeK::IntraGep:
    result = "IntraGep";
    break;
  case SVFGEdgeK::IntraPhi:
    result = "IntraPhi";
    break;
  case SVFGEdgeK::IntraCmp:
    result = "IntraCmp";
    break;
  case SVFGEdgeK::IntraBranch:
    result = "IntraBranch";
    break;
  case SVFGEdgeK::IntraMu:
    result = "IntraMu";
    break;
  case SVFGEdgeK::IntraChi:
    result = "IntraChi";
    break;
  case SVFGEdgeK::CallMu:
    result = "CallMu";
    break;
  case SVFGEdgeK::CallChi:
    result = "CallChi";
    break;
  case SVFGEdgeK::RetMu:
    result = "RetMu";
    break;
  case SVFGEdgeK::EntryChi:
    result = "EntryChi";
    break;
  case SVFGEdgeK::CallDir:
    result = "CallDir";
    break;
  case SVFGEdgeK::CallInd:
    result = "CallInd";
    break;
  case SVFGEdgeK::CallAIn:
    result = "CallAIn";
    break;
  case SVFGEdgeK::CallFIn:
    result = "CallFIn";
    break;
  case SVFGEdgeK::ParamCall:
    result = "ParamCall";
    break;
  case SVFGEdgeK::RetDir:
    result = "RetDir";
    break;
  case SVFGEdgeK::RetInd:
    result = "RetInd";
    break;
  case SVFGEdgeK::RetAOut:
    result = "RetAOut";
    break;
  case SVFGEdgeK::RetFOut:
    result = "RetFOut";
    break;
  case SVFGEdgeK::ParamRet:
    result = "ParamRet";
    break;
  case SVFGEdgeK::IntraIndirect:
    result = "IntraIndirect";
    break;
  case SVFGEdgeK::ThreadMHPIndirectVF:
    result = "ThreadMHPIndirectVF";
    break;
  case SVFGEdgeK::Variant:
    result = "Variant";
    break;
  default:
    result = "Unknown";
    break;
  }
  return result;
}

void SVFG::addNode(SVFGNode *node) {
  nodeMap[node->getId()] = node;
  if (node->getId() >= nextNodeId) {
    nextNodeId = node->getId() + 1;
  }
  if (node->hasValueId())
    valueIdToDefNodeMap.emplace(node->getValueId(), node->getId());
  if (node->isMemNode() && node->getMemReg() != 0 &&
      isMemDefSVFGNode(node->getNodeKind())) {
    setMSSADef(node->getMemReg(), node, node->getSSAVersion());
  }

  // Maintain instruction indices for MemorySSA nodes.
  if (auto *mu = dyn_cast<LoadMuSVFGNode>(node)) {
    if (const llvm::LoadInst *li = mu->getLoadInst())
      loadInstToMuMap[li].insert(mu);
  } else if (auto *chi = dyn_cast<StoreChiSVFGNode>(node)) {
    if (const llvm::StoreInst *si = chi->getStoreInst())
      storeInstToChiMap[si].insert(chi);
  }

  updateStat(node);
}

SVFGEdge *SVFG::addEdge(SVFGNode *src, SVFGNode *dst, SVFGEdgeK kind,
                        const llvm::CallBase *callSite,
                        const SVFGNodeBS &pointsTo, std::string callSiteDebug) {
  if (!src || !dst)
    return nullptr;

  if (callSiteDebug.empty() && callSite) {
    llvm::raw_string_ostream os(callSiteDebug);
    os << callSite->getFunction()->getName();
    if (const Function *callee = callSite->getCalledFunction()) {
      os << "->" << callee->getName();
    } else {
      os << "->ind";
    }
    if (const DebugLoc &dl = callSite->getDebugLoc()) {
      os << "@" << dl.getLine() << ":" << dl.getCol();
    }
  }

  for (auto *existing : src->getOutEdges()) {
    if (existing->getDstNode() == dst && existing->getEdgeKind() == kind &&
        existing->getCallSite() == callSite) {
      existing->addPointsTo(pointsTo);
      // Opportunistically fill in the debug string if it was missing.
      if (existing->getCallSiteDebug().empty() && !callSiteDebug.empty()) {
        existing->setCallSiteDebug(callSiteDebug);
      }
      return existing;
    }
  }

  SVFGEdge *edge = new SVFGEdge(src, dst, kind, SVFGEdge::EdgeWeight::One,
                                callSite, std::move(callSiteDebug), pointsTo);
  src->addOutEdge(edge);
  dst->addInEdge(edge);
  updateStat(edge);
  return edge;
}

void SVFG::removeEdge(SVFGEdge *edge) {
  if (!edge)
    return;

  adjustEdgeStat(stat, edge, -1);

  SVFGNode *src = edge->getSrcNode();
  SVFGNode *dst = edge->getDstNode();

  if (src)
    src->removeOutEdge(edge);
  if (dst)
    dst->removeInEdge(edge);

  delete edge;
}

void SVFG::setObjectValue(uint32_t objId, const llvm::Value *v) {
  if (objId == 0 || !v)
    return;
  objIdToValue[objId] = v;
  valueToObjIds[v].insert(objId);
  // Preserve the first reverse mapping to avoid clobbering a canonical base
  // object with a later refined alias to the same abstract object.
  if (valueToObjId.find(v) == valueToObjId.end())
    valueToObjId[v] = objId;
}

const llvm::Value *SVFG::getObjectValue(uint32_t objId) const {
  auto it = objIdToValue.find(objId);
  return (it != objIdToValue.end()) ? it->second : nullptr;
}

uint32_t SVFG::getObjectId(const llvm::Value *v) const {
  if (!v)
    return 0;
  auto it = valueToObjId.find(v);
  return (it != valueToObjId.end()) ? it->second : 0;
}

uint32_t SVFG::getFieldInsensitiveObject(uint32_t baseObjId) const {
  if (baseObjId == 0)
    return 0;
  if (isUnknownObject(baseObjId))
    return baseObjId;

  uint32_t canonical = baseObjId;
  if (const ObjectInfo *info = getObjectInfo(baseObjId))
    if (info->baseObjId != 0)
      canonical = info->baseObjId;

  // A pre-analysis may hand us a field object that is itself the collapsed
  // representative of its base (SVF's `getFIObjVar` can return such a node).
  if (const ObjectInfo *info = getObjectInfo(baseObjId))
    if (info->isFieldInsensitive && baseObjId != canonical &&
        !info->hasFieldOffset)
      return baseObjId;

  for (const auto &entry : objIdToInfo) {
    if (entry.first == canonical)
      continue;
    const ObjectInfo &info = entry.second;
    if (!info.isFieldInsensitive || info.hasFieldOffset)
      continue;
    if (info.baseObjId == canonical)
      return entry.first;
  }
  return 0;
}

uint32_t SVFG::getOrCreateFieldInsensitiveObject(uint32_t baseObjId) {
  if (baseObjId == 0)
    return 0;
  if (isUnknownObject(baseObjId))
    return baseObjId;

  if (const uint32_t existing = getFieldInsensitiveObject(baseObjId))
    return existing;

  uint32_t base = baseObjId;
  if (const ObjectInfo *info = getObjectInfo(baseObjId))
    if (info->baseObjId != 0)
      base = info->baseObjId;

  // Mark the base first: every subsequent field access must collapse, exactly
  // like SVF's irreversible `setObjFieldInsensitive`.
  ObjectInfo baseUpdate;
  baseUpdate.isFieldInsensitive = true;
  baseUpdate.baseObjId = base;
  updateObjectInfo(base, baseUpdate);

  ObjectInfo info;
  if (const ObjectInfo *baseInfo = getObjectInfo(base))
    info = *baseInfo;
  info.isFieldInsensitive = true;
  info.isArray = false;
  info.baseObjId = base;
  info.hasFieldOffset = false;
  info.fieldOffset = 0;

  const uint32_t fiObjId = allocateObjectId();
  setObjectInfo(fiObjId, info);
  if (const llvm::Value *value = getObjectValue(base))
    setObjectValue(fiObjId, value);
  setObjectDebug(fiObjId, "FI_OBJ(" + std::to_string(base) + ")");
  return fiObjId;
}

uint32_t SVFG::canonicalObjectId(uint32_t objId) const {
  const ObjectInfo *info = getObjectInfo(objId);
  if (!info || !info->isFieldInsensitive)
    return objId;
  // Only the dedicated representative (whose base differs from itself) is
  // rewritten.  The marked base already is the canonical object.
  if (info->baseObjId != 0 && info->baseObjId != objId)
    return info->baseObjId;
  return objId;
}

SVFGNode *SVFG::getCanonicalDefNodeForDDAId(uint32_t ddaId) const {
  if (SVFGNode *valueNode = getValueIdNode(ddaId))
    return valueNode;

  const llvm::Value *objValue = getObjectValue(ddaId);
  if (!objValue)
    return nullptr;

  if (SVFGNode *valueNode = getValueNode(objValue))
    return valueNode;

  if (const auto *inst = llvm::dyn_cast<llvm::Instruction>(objValue))
    return getDef(inst);

  return nullptr;
}

std::vector<uint32_t> SVFG::collectReferencedObjectIds() const {
  std::unordered_set<uint32_t> ids;
  auto add = [&](uint32_t id) {
    if (id != 0)
      ids.insert(id);
  };
  for (const auto &entry : objIdToInfo) {
    add(entry.first);
    add(entry.second.baseObjId);
  }
  for (const auto &entry : objectDebug)
    add(entry.first);
  for (const auto &entry : objIdToValue)
    add(entry.first);
  for (const auto &entry : valueToObjId)
    add(entry.second);
  for (const auto &entry : valueToObjIds)
    for (uint32_t id : entry.second)
      add(id);
  for (const auto &entry : baseToFieldObjects) {
    add(entry.first);
    for (uint32_t id : entry.second)
      add(id);
  }
  for (const auto &entry : baseOffsetToFieldObject) {
    add(entry.first.first);
    add(entry.second);
  }
  for (const auto &entry : gepBaseToFieldObject)
    for (const auto &inner : entry.second) {
      add(inner.first);
      add(inner.second);
    }

  for (const auto &entry : nodeMap) {
    SVFGNode *node = entry.second;
    if (!node)
      continue;
    if (const SVFGNodeBS *pts = node->getPointsTo())
      for (uint32_t id : *pts)
        add(id);
    if (auto *addr = llvm::dyn_cast<AddrSVFGNode>(node))
      add(addr->getObjectId());
    for (const SVFGEdge *edge : node->getOutEdges())
      for (uint32_t id : edge->getPointsTo())
        add(id);
  }

  std::vector<uint32_t> result(ids.begin(), ids.end());
  std::sort(result.begin(), result.end());
  return result;
}

std::vector<SVFGNodeBS> SVFG::collectPointsToSets() const {
  std::vector<SVFGNodeBS> sets;
  auto collect = [&](const SVFGNodeBS &set) {
    if (!set.empty())
      sets.push_back(set);
  };
  for (const auto &entry : valueToObjIds)
    collect(entry.second);
  for (const auto &entry : baseToFieldObjects)
    collect(entry.second);
  for (const auto &entry : nodeMap) {
    SVFGNode *node = entry.second;
    if (!node)
      continue;
    if (const SVFGNodeBS *pts = node->getPointsTo())
      collect(*pts);
    for (const SVFGEdge *edge : node->getOutEdges())
      collect(edge->getPointsTo());
  }
  return sets;
}

bool SVFG::remapObjectIds(
    const std::unordered_map<uint32_t, uint32_t> &oldToNew,
    std::string *error) {
  auto fail = [&](std::string message) {
    if (error)
      *error = std::move(message);
    return false;
  };

  const std::vector<uint32_t> referenced = collectReferencedObjectIds();
  std::unordered_set<uint32_t> newIds;
  newIds.reserve(referenced.size() * 2);
  for (uint32_t id : referenced) {
    const auto it = oldToNew.find(id);
    if (it == oldToNew.end())
      return fail("missing object ID " + std::to_string(id));
    if (!newIds.insert(it->second).second)
      return fail("object-ID mapping is not injective at " + std::to_string(id));
  }

  auto mapId = [&](uint32_t id) -> uint32_t {
    if (id == 0)
      return 0;
    const auto it = oldToNew.find(id);
    return it == oldToNew.end() ? id : it->second;
  };

  std::unordered_map<uint32_t, ObjectInfo> newInfo;
  newInfo.reserve(objIdToInfo.size());
  for (const auto &entry : objIdToInfo) {
    ObjectInfo info = entry.second;
    if (info.baseObjId != 0)
      info.baseObjId = mapId(info.baseObjId);
    newInfo.emplace(mapId(entry.first), info);
  }

  std::unordered_map<uint32_t, std::string> newDebug;
  newDebug.reserve(objectDebug.size());
  for (const auto &entry : objectDebug)
    newDebug.emplace(mapId(entry.first), entry.second);

  std::unordered_map<uint32_t, const llvm::Value *> newObjToValue;
  newObjToValue.reserve(objIdToValue.size());
  for (const auto &entry : objIdToValue)
    newObjToValue.emplace(mapId(entry.first), entry.second);

  std::unordered_map<const llvm::Value *, uint32_t> newValueToObj;
  newValueToObj.reserve(valueToObjId.size());
  for (const auto &entry : valueToObjId)
    newValueToObj.emplace(entry.first, mapId(entry.second));

  std::unordered_map<const llvm::Value *, SVFGNodeBS> newValueToObjs;
  newValueToObjs.reserve(valueToObjIds.size());
  for (const auto &entry : valueToObjIds) {
    SVFGNodeBS set = entry.second;
    set.remapIds(mapId);
    newValueToObjs.emplace(entry.first, std::move(set));
  }

  std::unordered_map<uint32_t, SVFGNodeBS> newBaseToFields;
  newBaseToFields.reserve(baseToFieldObjects.size());
  for (const auto &entry : baseToFieldObjects) {
    SVFGNodeBS set = entry.second;
    set.remapIds(mapId);
    newBaseToFields.emplace(mapId(entry.first), std::move(set));
  }

  std::map<std::pair<uint32_t, uint64_t>, uint32_t> newBaseOffset;
  for (const auto &entry : baseOffsetToFieldObject)
    newBaseOffset.emplace(std::make_pair(mapId(entry.first.first),
                                         entry.first.second),
                          mapId(entry.second));

  std::unordered_map<const llvm::GetElementPtrInst *,
                     std::unordered_map<uint32_t, uint32_t>>
      newGepBaseToField;
  newGepBaseToField.reserve(gepBaseToFieldObject.size());
  for (const auto &entry : gepBaseToFieldObject) {
    std::unordered_map<uint32_t, uint32_t> inner;
    inner.reserve(entry.second.size());
    for (const auto &field : entry.second)
      inner.emplace(mapId(field.first), mapId(field.second));
    newGepBaseToField.emplace(entry.first, std::move(inner));
  }

  SVFGNode::ObjectIdRemap remap = [&](uint32_t id) { return mapId(id); };
  for (const auto &entry : nodeMap)
    if (entry.second)
      entry.second->remapObjectIds(remap);

  std::unordered_set<const SVFGEdge *> visitedEdges;
  for (const auto &entry : nodeMap) {
    if (!entry.second)
      continue;
    for (SVFGEdge *edge : entry.second->getOutEdges()) {
      if (visitedEdges.insert(edge).second)
        edge->remapObjectIds(remap);
    }
  }

  objIdToInfo = std::move(newInfo);
  objectDebug = std::move(newDebug);
  objIdToValue = std::move(newObjToValue);
  valueToObjId = std::move(newValueToObj);
  valueToObjIds = std::move(newValueToObjs);
  baseToFieldObjects = std::move(newBaseToFields);
  baseOffsetToFieldObject = std::move(newBaseOffset);
  gepBaseToFieldObject = std::move(newGepBaseToField);

  uint32_t maxNew = 0;
  for (uint32_t id : newIds)
    maxNew = std::max(maxNew, id);
  nextObjectId =
      std::max<uint32_t>(maxNew == std::numeric_limits<uint32_t>::max()
                             ? maxNew
                             : maxNew + 1,
                         kObjectIdBase);

  objectIdMapping = oldToNew;
  reverseObjectIdMapping.clear();
  reverseObjectIdMapping.reserve(oldToNew.size());
  for (const auto &entry : oldToNew)
    reverseObjectIdMapping[entry.second] = entry.first;
  return true;
}

bool SVFG::objectsOverlap(uint32_t lhs, uint32_t rhs) const {
  if (lhs == rhs)
    return true;
  // Field-insensitive representatives and their base denote the same memory.
  if (canonicalObjectId(lhs) == canonicalObjectId(rhs))
    return true;
  // Unknown objects act as wildcards in either direction.
  if (isUnknownObject(lhs) || isUnknownObject(rhs))
    return true;

  auto baseObject = [&](uint32_t object) {
    const ObjectInfo *info = getObjectInfo(object);
    return info && info->baseObjId != 0 ? info->baseObjId : object;
  };
  if (baseObject(lhs) != baseObject(rhs))
    return false;

  const ObjectInfo *lhsInfo = getObjectInfo(lhs);
  const ObjectInfo *rhsInfo = getObjectInfo(rhs);
  const bool lhsField = lhsInfo && lhsInfo->hasFieldOffset;
  const bool rhsField = rhsInfo && rhsInfo->hasFieldOffset;
  // A query for the aggregate covers all of its fields; two distinct fields
  // only overlap at the same offset.
  if (!lhsField || !rhsField)
    return true;
  return lhsInfo->fieldOffset == rhsInfo->fieldOffset;
}

bool SVFG::objectSetsOverlap(const SVFGNodeBS &lhs,
                             const SVFGNodeBS &rhs) const {
  for (uint32_t lhsObject : lhs)
    for (uint32_t rhsObject : rhs)
      if (objectsOverlap(lhsObject, rhsObject))
        return true;
  return false;
}

std::optional<std::pair<SVFGNodeBS, SVFGNodeBS>>
SVFG::getCallReadWriteObjects(const llvm::CallBase *call) const {
  if (!call)
    return std::nullopt;

  const SVFGNodeBS *readOnly = getCallReadOnlyObjects(call);
  const SVFGNodeBS *writeOnly = getCallWriteObjects(call);
  if (readOnly || writeOnly)
    return std::make_pair(readOnly ? *readOnly : SVFGNodeBS{},
                          writeOnly ? *writeOnly : SVFGNodeBS{});

  const SVFGNodeSet &ins = getActualIns(call);
  const SVFGNodeSet &outs = getActualOuts(call);
  if (ins.empty() && outs.empty())
    return std::nullopt;

  auto unionPointsTo = [](const SVFGNodeSet &nodes) {
    SVFGNodeBS result;
    for (SVFGNode *node : nodes) {
      if (!node)
        continue;
      if (const SVFGNodeBS *pts = node->getPointsTo())
        result.insert(pts->begin(), pts->end());
    }
    return result;
  };
  return std::make_pair(unionPointsTo(ins), unionPointsTo(outs));
}

std::optional<SVFG::ModRefInfo>
SVFG::getModRefInfo(const llvm::CallBase *call,
                    const SVFGNodeBS &objects) const {
  if (!call)
    return std::nullopt;
  if (objects.empty())
    return ModRefInfo{};

  auto touchesSet = [&](const SVFGNodeBS &region) {
    return objectSetsOverlap(objects, region);
  };
  auto touchesNodes = [&](const SVFGNodeSet &nodes) {
    for (SVFGNode *node : nodes) {
      const SVFGNodeBS *pts = node ? node->getPointsTo() : nullptr;
      if (pts && touchesSet(*pts))
        return true;
    }
    return false;
  };

  const SVFGNodeBS *readOnly = getCallReadOnlyObjects(call);
  const SVFGNodeBS *writeOnly = getCallWriteObjects(call);
  if (readOnly || writeOnly) {
    ModRefInfo info;
    info.reads = readOnly && touchesSet(*readOnly);
    info.writes = writeOnly && touchesSet(*writeOnly);
    return info;
  }

  const SVFGNodeSet &ins = getActualIns(call);
  const SVFGNodeSet &outs = getActualOuts(call);
  if (!ins.empty() || !outs.empty()) {
    // No refined read-only summary (for example after deserialization):
    // ActualIn also carries may-write regions, so this is conservative.
    ModRefInfo info;
    info.reads = touchesNodes(ins);
    info.writes = touchesNodes(outs);
    return info;
  }

  // No MemorySSA summary: fall back to LLVM memory attributes. A call with no
  // attributes remains unknown rather than being reported as side-effect free.
  if (call->doesNotAccessMemory())
    return ModRefInfo{};
  if (call->onlyReadsMemory())
    return ModRefInfo{true, false};
  return std::nullopt;
}

std::optional<SVFG::ModRefInfo> SVFG::getModRefInfo(const llvm::CallBase *call,
                                                    uint32_t object) const {
  if (object == 0)
    return std::nullopt;
  return getModRefInfo(call, SVFGNodeBS{object});
}

std::optional<SVFG::ModRefInfo>
SVFG::getModRefInfo(const llvm::CallBase *call,
                    const llvm::Value *pointer) const {
  if (!call || !pointer || !pointer->getType()->isPointerTy())
    return std::nullopt;

  SVFGNodeBS objects = getObjectIds(pointer);
  if (objects.empty()) {
    if (const uint32_t single = getObjectId(pointer))
      objects.insert(single);
  }
  if (objects.empty())
    return std::nullopt;
  return getModRefInfo(call, objects);
}

std::optional<SVFG::ModRefInfo>
SVFG::getModRefInfo(const llvm::CallBase *call) const {
  if (!call)
    return std::nullopt;

  const SVFGNodeBS *readOnly = getCallReadOnlyObjects(call);
  const SVFGNodeBS *writeOnly = getCallWriteObjects(call);
  if (readOnly || writeOnly)
    return ModRefInfo{readOnly && !readOnly->empty(),
                      writeOnly && !writeOnly->empty()};

  const SVFGNodeSet &ins = getActualIns(call);
  const SVFGNodeSet &outs = getActualOuts(call);
  if (!ins.empty() || !outs.empty())
    return ModRefInfo{!ins.empty(), !outs.empty()};

  if (call->doesNotAccessMemory())
    return ModRefInfo{};
  if (call->onlyReadsMemory())
    return ModRefInfo{true, false};
  return std::nullopt;
}

std::optional<SVFG::ModRefInfo> SVFG::getModRefInfo(
    const llvm::CallBase *cs1, const llvm::CallBase *cs2) const {
  if (!cs1 || !cs2)
    return std::nullopt;
  if (cs1->doesNotAccessMemory() || cs2->doesNotAccessMemory())
    return ModRefInfo{};

  const auto first = getCallReadWriteObjects(cs1);
  const auto second = getCallReadWriteObjects(cs2);
  if (!first || !second)
    return std::nullopt;

  const SVFGNodeBS &cs1Reads = first->first;
  const SVFGNodeBS &cs1Writes = first->second;
  const SVFGNodeBS &cs2Reads = second->first;
  const SVFGNodeBS &cs2Writes = second->second;

  ModRefInfo info;
  // Ref: cs1 reads memory modified by cs2.
  info.reads = objectSetsOverlap(cs1Reads, cs2Writes);
  // Mod: cs1 writes memory read or modified by cs2.
  info.writes = objectSetsOverlap(cs1Writes, cs2Reads) ||
                objectSetsOverlap(cs1Writes, cs2Writes);
  return info;
}

uint32_t SVFG::getCallSiteId(const llvm::CallBase *cs,
                             const llvm::Function *callee) const {
  if (!cs || !callee)
    return 0;
  if (!hasConnectedCallee(cs, callee))
    return 0;
  CallSiteCalleeKey key{cs, callee};
  auto it = callSiteCalleeToId.find(key);
  return (it != callSiteCalleeToId.end()) ? it->second : 0;
}

void SVFG::initializeRefinedCallGraph(Module &M) {
  refinedCallGraph = std::make_unique<LTCallGraph>(M);
}

void SVFG::removeNode(SVFGNode *node) {
  if (!node)
    return;

  const uint32_t nodeId = node->getId();
  const llvm::Value *value = node->getValue();
  const llvm::Instruction *inst = node->getInstruction();
  const llvm::Function *fun = node->getFunction();

  for (auto it = mssaVerToNodeMap.begin(); it != mssaVerToNodeMap.end();) {
    if (it->second == node) {
      it = mssaVerToNodeMap.erase(it);
    } else {
      ++it;
    }
  }

  if (value) {
    auto it = valueToNodeMap.find(value);
    if (it != valueToNodeMap.end() && it->second == nodeId) {
      valueToNodeMap.erase(it);
    }
  }

  if (node->hasValueId()) {
    auto it = valueIdToDefNodeMap.find(node->getValueId());
    if (it != valueIdToDefNodeMap.end() && it->second == nodeId)
      valueIdToDefNodeMap.erase(it);
  }

  if (inst) {
    auto it = instToDefMap.find(inst);
    if (it != instToDefMap.end() && it->second == nodeId) {
      instToDefMap.erase(it);
    }
  }

  nodeFunctionDebug.erase(nodeId);
  nodeCallSiteDebug.erase(nodeId);
  globalStoreNodes.erase(node);

  if (auto *mu = dyn_cast<LoadMuSVFGNode>(node)) {
    if (const llvm::LoadInst *li = mu->getLoadInst()) {
      auto muIt = loadInstToMuMap.find(li);
      if (muIt != loadInstToMuMap.end()) {
        muIt->second.erase(node);
        if (muIt->second.empty())
          loadInstToMuMap.erase(muIt);
      }
    }
  } else if (auto *chi = dyn_cast<StoreChiSVFGNode>(node)) {
    if (const llvm::StoreInst *si = chi->getStoreInst()) {
      auto chiIt = storeInstToChiMap.find(si);
      if (chiIt != storeInstToChiMap.end()) {
        chiIt->second.erase(node);
        if (chiIt->second.empty())
          storeInstToChiMap.erase(chiIt);
      }
    }
  }

  if (auto *actualIn = dyn_cast<ActualInSVFGNode>(node)) {
    const llvm::CallBase *cs = actualIn->getCallSite();
    if (cs) {
      auto mapIt = callSiteToActualInMap.find(cs);
      if (mapIt != callSiteToActualInMap.end()) {
        mapIt->second.erase(node);
        if (mapIt->second.empty())
          callSiteToActualInMap.erase(mapIt);
      }
    }
  } else if (auto *actualOut = dyn_cast<ActualOutSVFGNode>(node)) {
    const llvm::CallBase *cs = actualOut->getCallSite();
    if (cs) {
      auto mapIt = callSiteToActualOutMap.find(cs);
      if (mapIt != callSiteToActualOutMap.end()) {
        mapIt->second.erase(node);
        if (mapIt->second.empty())
          callSiteToActualOutMap.erase(mapIt);
      }
    }
  } else if (auto *actualParm = dyn_cast<ActualParmSVFGNode>(node)) {
    const llvm::CallBase *cs = actualParm->getCallSite();
    if (cs) {
      auto mapIt = callSiteToActualParmMap.find(cs);
      if (mapIt != callSiteToActualParmMap.end()) {
        mapIt->second.erase(node);
        if (mapIt->second.empty())
          callSiteToActualParmMap.erase(mapIt);
      }
    }
  } else if (auto *actualRet = dyn_cast<ActualRetSVFGNode>(node)) {
    const llvm::CallBase *cs = actualRet->getCallSite();
    if (cs) {
      auto mapIt = callSiteToActualRetMap.find(cs);
      if (mapIt != callSiteToActualRetMap.end()) {
        mapIt->second.erase(node);
        if (mapIt->second.empty())
          callSiteToActualRetMap.erase(mapIt);
      }
    }
  } else if (isa<FormalInSVFGNode>(node)) {
    if (fun) {
      auto mapIt = funcToFormalInMap.find(fun);
      if (mapIt != funcToFormalInMap.end()) {
        mapIt->second.erase(node);
        if (mapIt->second.empty())
          funcToFormalInMap.erase(mapIt);
      }
    }
  } else if (isa<FormalOutSVFGNode>(node)) {
    if (fun) {
      auto mapIt = funcToFormalOutMap.find(fun);
      if (mapIt != funcToFormalOutMap.end()) {
        mapIt->second.erase(node);
        if (mapIt->second.empty())
          funcToFormalOutMap.erase(mapIt);
      }
    }
  } else if (isa<FormalParmSVFGNode>(node)) {
    if (fun) {
      auto mapIt = funcToFormalParmMap.find(fun);
      if (mapIt != funcToFormalParmMap.end()) {
        mapIt->second.erase(node);
        if (mapIt->second.empty())
          funcToFormalParmMap.erase(mapIt);
      }
    }
  } else if (isa<FormalRetSVFGNode>(node)) {
    if (fun) {
      auto mapIt = funcToFormalRetMap.find(fun);
      if (mapIt != funcToFormalRetMap.end()) {
        mapIt->second.erase(node);
        if (mapIt->second.empty())
          funcToFormalRetMap.erase(mapIt);
      }
    }
  }

  // Remove incident edges before deleting the node.
  // Use a set to avoid double-removing self-loop edges.
  std::unordered_set<SVFGEdge *> incident;
  auto outEdges = node->getOutEdges();
  incident.insert(outEdges.begin(), outEdges.end());
  auto inEdges = node->getInEdges();
  incident.insert(inEdges.begin(), inEdges.end());
  for (SVFGEdge *edge : incident) {
    removeEdge(edge);
  }

  adjustNodeStat(stat, node, -1);

  nodeMap.erase(nodeId);
  nodesForUpdate.erase(node);
  delete node;
}

static void adjustNodeStat(SVFGStat &stat, SVFGNode *node, int delta) {
  if (!node)
    return;
  stat.numNodes += delta;
  switch (node->getNodeKind()) {
  case SVFGK::Addr:
    stat.numAddrNodes += delta;
    break;
  case SVFGK::Copy:
    stat.numCopyNodes += delta;
    break;
  case SVFGK::Load:
    stat.numLoadNodes += delta;
    break;
  case SVFGK::Store:
    stat.numStoreNodes += delta;
    break;
  case SVFGK::Gep:
    stat.numGepNodes += delta;
    break;
  case SVFGK::Phi:
  case SVFGK::IntraPhi:
  case SVFGK::InterPhi:
    stat.numPhiNodes += delta;
    break;
  case SVFGK::FormalIn:
  case SVFGK::FormalOut:
  case SVFGK::ActualIn:
  case SVFGK::ActualOut:
  case SVFGK::MPhi:
  case SVFGK::MIntraPhi:
  case SVFGK::MInterPhi:
  case SVFGK::LoadMu:
  case SVFGK::StoreChi:
  case SVFGK::CallMu:
  case SVFGK::CallChi:
  case SVFGK::RetMu:
  case SVFGK::EntryChi:
    stat.numMemNodes += delta;
    break;
  case SVFGK::FormalParm:
  case SVFGK::ActualParm:
  case SVFGK::FormalRet:
  case SVFGK::ActualRet:
    stat.numParamNodes += delta;
    break;
  default:
    break;
  }
}

static void adjustEdgeStat(SVFGStat &stat, SVFGEdge *edge, int delta) {
  if (!edge)
    return;
  stat.numEdges += delta;
  if (isCallVFGEdge(edge->getEdgeKind()))
    stat.numCallEdges += delta;
  else if (isRetVFGEdge(edge->getEdgeKind()))
    stat.numRetEdges += delta;
  else if (isIntraVFGEdge(edge->getEdgeKind()))
    stat.numIntraEdges += delta;
}

void SVFG::updateStat(SVFGNode *node) { adjustNodeStat(stat, node, +1); }
void SVFG::updateStat(SVFGEdge *edge) { adjustEdgeStat(stat, edge, +1); }

SVFGNodeSet SVFG::getPreds(SVFGNode *node) const {
  SVFGNodeSet result;
  if (!node)
    return result;

  SVFGNodeSet visited;
  visited.insert(node); // mark start as visited so we don't re-enqueue it
  std::queue<SVFGNode *> worklist;
  worklist.push(node);

  while (!worklist.empty()) {
    SVFGNode *current = worklist.front();
    worklist.pop();

    for (auto *edge : current->getInEdges()) {
      SVFGNode *pred = edge->getSrcNode();
      if (pred == node)
        continue; // skip back-edges to the start node
      if (visited.insert(pred).second) {
        result.insert(pred);
        worklist.push(pred);
      }
    }
  }

  return result;
}

SVFGNodeSet SVFG::getSuccs(SVFGNode *node) const {
  SVFGNodeSet result;
  if (!node)
    return result;

  SVFGNodeSet visited;
  visited.insert(node);
  std::queue<SVFGNode *> worklist;
  worklist.push(node);

  while (!worklist.empty()) {
    SVFGNode *current = worklist.front();
    worklist.pop();

    for (auto *edge : current->getOutEdges()) {
      SVFGNode *succ = edge->getDstNode();
      if (succ == node)
        continue; // skip back-edges to the start node
      if (visited.insert(succ).second) {
        result.insert(succ);
        worklist.push(succ);
      }
    }
  }

  return result;
}

bool SVFG::hasPath(SVFGNode *src, SVFGNode *dst) const {
  if (!src || !dst || src == dst)
    return src == dst;

  SVFGNodeSet visited;
  std::queue<SVFGNode *> worklist;
  worklist.push(src);
  visited.insert(src);

  while (!worklist.empty()) {
    SVFGNode *current = worklist.front();
    worklist.pop();

    if (current == dst)
      return true;

    for (auto *edge : current->getOutEdges()) {
      SVFGNode *succ = edge->getDstNode();
      if (visited.insert(succ).second) {
        worklist.push(succ);
      }
    }
  }

  return false;
}

SVFGEdge *SVFG::getIntraVFGEdge(const SVFGNode *src, const SVFGNode *dst,
                                SVFGEdgeK kind) const {
  if (!src || !dst)
    return nullptr;
  for (SVFGEdge *edge : dst->getInEdges()) {
    if (edge->getSrcNode() == src && edge->getEdgeKind() == kind)
      return edge;
  }
  return nullptr;
}

SVFGNode *SVFG::getLHSTopLevPtr(SVFGNode *node) const {
  if (!node)
    return nullptr;
  // For stmt/phi/param nodes the LHS (the value defined) is the node itself.
  if (node->isStmtNode() || node->isPhiNode() || node->isParamNode())
    return node;
  // For memory SSA nodes the "LHS" is the node (it defines a memory version).
  if (node->isMemNode())
    return node;
  return node;
}

const llvm::CallBase *SVFG::isCallSiteRetSVFGNode(const SVFGNode *n) const {
  if (!n)
    return nullptr;
  if (const auto *ar = llvm::dyn_cast_or_null<ActualRetSVFGNode>(n))
    return ar->getCallSite();
  if (const auto *phi = llvm::dyn_cast_or_null<InterPhiSVFGNode>(n))
    return phi->isActualRetPHI() ? phi->getCallSite() : nullptr;
  if (const auto *ao = llvm::dyn_cast_or_null<ActualOutSVFGNode>(n))
    return ao->getCallSite();
  if (n->getNodeKind() == SVFGK::MInterPhi)
    return n->getCallSite();
  return nullptr;
}

const llvm::Function *SVFG::isFunEntrySVFGNode(const SVFGNode *n) const {
  if (!n)
    return nullptr;
  if (const auto *fp = llvm::dyn_cast_or_null<FormalParmSVFGNode>(n))
    return fp->getFunction();
  if (const auto *phi = llvm::dyn_cast_or_null<InterPhiSVFGNode>(n))
    return phi->isFormalParmPHI() ? phi->getFunction() : nullptr;
  if (const auto *fi = llvm::dyn_cast_or_null<FormalInSVFGNode>(n))
    return fi->getFunction();
  if (n->getNodeKind() == SVFGK::MInterPhi)
    return n->getFunction();
  return nullptr;
}

void SVFG::getInterVFEdgesForIndirectCallSite(
    const llvm::CallBase *cs, const llvm::Function *callee,
    std::vector<SVFGEdge *> &edges) const {
  if (!cs || !callee)
    return;

  std::unordered_set<SVFGEdge *> seen;
  auto pushEdge = [&](SVFGEdge *edge) {
    if (edge && seen.insert(edge).second)
      edges.push_back(edge);
  };

  auto findInterEdge = [&](SVFGNode *src, SVFGNode *dst,
                           std::initializer_list<SVFGEdgeK> kinds) {
    if (!src || !dst)
      return;
    for (SVFGEdge *edge : src->getOutEdges()) {
      if (!edge || edge->getDstNode() != dst || edge->getCallSite() != cs)
        continue;
      if (std::find(kinds.begin(), kinds.end(), edge->getEdgeKind()) !=
          kinds.end()) {
        pushEdge(edge);
      }
    }
  };

  for (SVFGNode *actualParmNode : getActualParms(cs)) {
    auto *actualParm = dyn_cast<ActualParmSVFGNode>(actualParmNode);
    if (!actualParm)
      continue;
    const unsigned actualIdx = actualParm->getParamIndex();
    const bool isVarArgExtra =
        callee->isVarArg() && actualIdx >= callee->arg_size();
    for (SVFGNode *formalParmNode : getFormalParms(callee)) {
      if (isVarArgExtra) {
        if (!isa<VarArgSVFGNode>(formalParmNode))
          continue;
      } else {
        auto *formalParm = dyn_cast<FormalParmSVFGNode>(formalParmNode);
        if (!formalParm || formalParm->getParamIndex() != actualIdx)
          continue;
      }
      findInterEdge(
          actualParmNode, formalParmNode,
          {SVFGEdgeK::CallDir, SVFGEdgeK::CallInd, SVFGEdgeK::ParamCall});
    }
  }

  for (SVFGNode *formalRetNode : getFormalRets(callee)) {
    for (SVFGNode *actualRetNode : getActualRets(cs)) {
      findInterEdge(
          formalRetNode, actualRetNode,
          {SVFGEdgeK::RetDir, SVFGEdgeK::RetInd, SVFGEdgeK::ParamRet});
    }
  }

  for (SVFGNode *actualInNode : getActualIns(cs)) {
    for (SVFGEdge *edge : actualInNode->getOutEdges()) {
      if (!edge || edge->getCallSite() != cs)
        continue;
      if (edge->getEdgeKind() != SVFGEdgeK::CallAIn &&
          edge->getEdgeKind() != SVFGEdgeK::CallFIn)
        continue;
      if (edge->getDstNode() && edge->getDstNode()->getFunction() == callee)
        pushEdge(edge);
    }
  }

  for (SVFGNode *actualOutNode : getActualOuts(cs)) {
    for (SVFGEdge *edge : actualOutNode->getInEdges()) {
      if (!edge || edge->getCallSite() != cs)
        continue;
      if (edge->getEdgeKind() != SVFGEdgeK::RetAOut &&
          edge->getEdgeKind() != SVFGEdgeK::RetFOut)
        continue;
      if (edge->getSrcNode() && edge->getSrcNode()->getFunction() == callee)
        pushEdge(edge);
    }
  }
}

void SVFG::dump(const std::string &filename, bool simple) const {
  (void)SVFGSerializer::writeDot(*this, filename, simple);
}

bool SVFG::writeToFile(const std::string &filename) const {
  return SVFGSerializer::writeText(*this, filename);
}

bool SVFG::readFromFile(const std::string &filename) {
  return SVFGSerializer::readText(*this, filename);
}

void SVFG::printStat() const {
  llvm::errs() << "=== SVFG Statistics ===\n";
  llvm::errs() << "Total Nodes: " << stat.numNodes << "\n";
  llvm::errs() << "  Addr Nodes: " << stat.numAddrNodes << "\n";
  llvm::errs() << "  Copy Nodes: " << stat.numCopyNodes << "\n";
  llvm::errs() << "  Load Nodes: " << stat.numLoadNodes << "\n";
  llvm::errs() << "  Store Nodes: " << stat.numStoreNodes << "\n";
  llvm::errs() << "  Gep Nodes: " << stat.numGepNodes << "\n";
  llvm::errs() << "  Phi Nodes: " << stat.numPhiNodes << "\n";
  llvm::errs() << "  Memory Nodes: " << stat.numMemNodes << "\n";
  llvm::errs() << "  Param Nodes: " << stat.numParamNodes << "\n";
  llvm::errs() << "Total Edges: " << stat.numEdges << "\n";
  llvm::errs() << "  Intra Edges: " << stat.numIntraEdges << "\n";
  llvm::errs() << "  Call Edges: " << stat.numCallEdges << "\n";
  llvm::errs() << "  Ret Edges: " << stat.numRetEdges << "\n";
}

void SVFG::swapWith(SVFG &other) {
  using std::swap;
  swap(nodeMap, other.nodeMap);
  swap(instToDefMap, other.instToDefMap);
  swap(valueToNodeMap, other.valueToNodeMap);
  swap(valueIdToDefNodeMap, other.valueIdToDefNodeMap);
  swap(mssaVerToNodeMap, other.mssaVerToNodeMap);
  swap(callSiteToActualInMap, other.callSiteToActualInMap);
  swap(callSiteToActualOutMap, other.callSiteToActualOutMap);
  swap(callReadOnlyObjects, other.callReadOnlyObjects);
  swap(callWriteObjects, other.callWriteObjects);
  swap(callSiteToActualParmMap, other.callSiteToActualParmMap);
  swap(callSiteToActualRetMap, other.callSiteToActualRetMap);
  swap(funcToFormalInMap, other.funcToFormalInMap);
  swap(funcToFormalOutMap, other.funcToFormalOutMap);
  swap(funcToFormalParmMap, other.funcToFormalParmMap);
  swap(funcToFormalRetMap, other.funcToFormalRetMap);
  swap(loadInstToMuMap, other.loadInstToMuMap);
  swap(storeInstToChiMap, other.storeInstToChiMap);
  swap(icfg, other.icfg);
  swap(nextNodeId, other.nextNodeId);
  swap(nextObjectId, other.nextObjectId);
  swap(stat, other.stat);
  swap(objectDebug, other.objectDebug);
  swap(objIdToValue, other.objIdToValue);
  swap(valueToObjId, other.valueToObjId);
  swap(valueToObjIds, other.valueToObjIds);
  swap(gepBaseToFieldObject, other.gepBaseToFieldObject);
  swap(baseOffsetToFieldObject, other.baseOffsetToFieldObject);
  swap(gepAccessInfo, other.gepAccessInfo);
  swap(objIdToInfo, other.objIdToInfo);
  swap(baseToFieldObjects, other.baseToFieldObjects);
  swap(objectIdMapping, other.objectIdMapping);
  swap(reverseObjectIdMapping, other.reverseObjectIdMapping);
  swap(nodeFunctionDebug, other.nodeFunctionDebug);
  swap(nodeCallSiteDebug, other.nodeCallSiteDebug);
  swap(nodesForUpdate, other.nodesForUpdate);
  swap(funPtrToIndCallSites, other.funPtrToIndCallSites);
  swap(callSiteToConnectedCallees, other.callSiteToConnectedCallees);
  swap(calleeToIndCallSites, other.calleeToIndCallSites);
  swap(callSiteCalleeToId, other.callSiteCalleeToId);
  swap(globalStoreNodes, other.globalStoreNodes);
  swap(nextCallSiteId, other.nextCallSiteId);
  swap(refinedCallGraph, other.refinedCallGraph);
}

void SVFG::markForUpdate(SVFGNode *node) {
  if (node) {
    nodesForUpdate.insert(node);
  }
}

SVFGNodeSet SVFG::getNodesForUpdate() const { return nodesForUpdate; }

void SVFG::clearUpdateMarkers() { nodesForUpdate.clear(); }
