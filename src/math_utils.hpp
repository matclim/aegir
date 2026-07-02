// SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration
//
// SPDX-License-Identifier: LGPL-3.0-or-later

// math_utils.hpp — small numeric helpers shared across plugins

#pragma once

#include <array>
#include <cmath>

namespace aegir {

// Euclidean norm of a 3-vector (momentum, position, ...).
inline double magnitude(std::array<double, 3> const& v) {
  return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

}  // namespace aegir
