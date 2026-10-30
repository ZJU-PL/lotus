#pragma once

#include <cstddef>

namespace lotus::verification::automata::test {
void resetAllocationProbe() noexcept;
void observeAllocationPhase(const char *phase, bool entering,
                            void *data) noexcept;
std::size_t allocationCount() noexcept;
std::size_t auditedPhaseCount() noexcept;
} // namespace lotus::verification::automata::test
