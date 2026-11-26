#pragma once

#include <algorithm>
#include <vector>

namespace tpa::taint {

template <typename InputVector, typename UnaryOperator,
          typename OutputVector = std::vector<
              std::remove_reference_t<decltype(std::declval<UnaryOperator>()(
                  std::declval<typename InputVector::value_type>()))>>>
OutputVector vectorTransform(const InputVector &input, UnaryOperator &&op) {
  OutputVector output;
  output.reserve(input.size());

  std::transform(input.begin(), input.end(), std::back_inserter(output),
                 std::forward<UnaryOperator>(op));

  return output;
}

} // namespace tpa::taint
