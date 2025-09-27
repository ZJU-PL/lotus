/*
 * Taint Configuration Manager Implementation
 */

#include "Checker/TaintConfigManager.h"

namespace checker {

std::unique_ptr<TaintConfigManager> TaintConfigManager::instance = nullptr;

} // namespace checker
