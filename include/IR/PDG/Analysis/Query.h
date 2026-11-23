/**
 * @file Query.h
 * @brief Umbrella include for the split PDG query interface.
 *
 * This header is intentionally thin. It preserves the original single-include
 * experience for clients while delegating the actual declarations to the
 * focused headers introduced by the query-layer split:
 * - QueryCore.h: shared vocabulary and core query services
 * - SliceQuery.h: slicing and chopping queries
 * - DependenceQuery.h: reachability and path queries
 * - DataFlowQuery.h: dataflow-flavored convenience queries
 * - TransformQuery.h: transform legality and scheduling queries
 * - DiffQuery.h: structural differencing
 * - SummaryQuery.h: function summary extraction
 * - ImpactQuery.h: change/impact ranking over PDG reachability
 * - ResourceFlowQuery.h: built-in resource acquire/release tracking
 */

#pragma once

#include "IR/PDG/Analysis/ApiQuery.h"
#include "IR/PDG/Analysis/ArithmeticQuery.h"
#include "IR/PDG/Analysis/BoundsQuery.h"
#include "IR/PDG/Analysis/CallContractQuery.h"
#include "IR/PDG/Analysis/DataFlowQuery.h"
#include "IR/PDG/Analysis/DependenceQuery.h"
#include "IR/PDG/Analysis/DiffQuery.h"
#include "IR/PDG/Analysis/ImpactQuery.h"
#include "IR/PDG/Analysis/LifetimeQuery.h"
#include "IR/PDG/Analysis/QueryCore.h"
#include "IR/PDG/Analysis/ProtocolQuery.h"
#include "IR/PDG/Analysis/ResourceFlowQuery.h"
#include "IR/PDG/Analysis/RuleQuery.h"
#include "IR/PDG/Analysis/SliceQuery.h"
#include "IR/PDG/Analysis/StateQuery.h"
#include "IR/PDG/Analysis/SummaryQuery.h"
#include "IR/PDG/Analysis/TaintQuery.h"
#include "IR/PDG/Analysis/TransformQuery.h"
