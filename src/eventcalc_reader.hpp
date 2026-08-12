// SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration
//
// SPDX-License-Identifier: LGPL-3.0-or-later

// eventcalc_reader.hpp — parser for EventCalc-SHiP `*_data.dat` event records
//
// Header-only, so eventcalc_source.cpp stays the single translation unit
// aegir_add_plugin() compiles, and so the parser can be exercised without
// Phlex, ROOT or Geant4 in the picture.
//
// Values are kept exactly as EventCalc writes them: GeV for momenta, energies
// and masses; metres for the decay vertex, with the origin at the centre of
// the SHiP target. Per docs/units.md a raw double acquires its unit on the
// line it enters — that line is in eventcalc_source.cpp, so this header stays
// deliberately unit-free.

#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aegir::eventcalc {

// Normalisation numbers from the first line of the file. The expected number
// of decay events in the volume is
//   total_llps_produced * polar_acceptance * azimuthal_acceptance
//     * visible_branching_ratio * mean(decay_probability)
// and is also reported directly as total_events.
struct FileSummary {
  double sampled_events = 0;
  double coupling_squared = 0;
  double total_llps_produced = 0;  // over 15 years of SHiP running
  double polar_acceptance = 0;
  double azimuthal_acceptance = 0;
  double mean_decay_probability = 0;
  double visible_branching_ratio = 0;
  double total_events = 0;
};

// One particle of the record: the LLP or one of its decay products.
struct Particle {
  std::array<double, 3> momentum = {0, 0, 0};  // px, py, pz [GeV]
  double energy = 0;                           // [GeV]
  double mass = 0;                             // [GeV]
  std::int32_t pdg = 0;
};

// One sampled LLP decay.
struct DecayEvent {
  Particle llp;
  std::array<double, 3> vertex = {0, 0, 0};  // decay vertex [m]
  double decay_probability = 0;              // P_decay,LLP — per-event weight
  std::uint32_t process_id = 0;              // index into Reader::processes()
  std::vector<Particle> daughters;           // padding groups stripped
};

namespace detail {

// Leading columns describing the LLP itself.
inline constexpr std::size_t kMotherColumns = 10;
// Columns per decay product.
inline constexpr std::size_t kDaughterColumns = 6;
// EventCalc pads short rows with `0. 0. 0. 0. 0. -999.` so that channels of
// differing multiplicity stack into one flat array.
inline constexpr double kPaddingSentinel = -900.0;

// Read the first number following `label`. strtod rather than operator>>:
// the summary line writes values as sentence fragments ("Squared coupling:
// 1e-10. Total number ...") and the trailing full stop is not reliably
// rejected by every standard-library num_get.
inline std::optional<double> value_after(std::string const& line,
                                         std::string_view label) {
  auto const pos = line.find(label);
  if (pos == std::string::npos) return std::nullopt;
  char const* first = line.c_str() + pos + label.size();
  char* last = nullptr;
  double const value = std::strtod(first, &last);
  if (last == first) return std::nullopt;
  return value;
}

inline FileSummary parse_summary(std::string const& line) {
  FileSummary s;
  auto assign = [&line](double* target, std::string_view label) {
    if (auto const v = value_after(line, label)) *target = *v;
  };
  assign(&s.sampled_events, "Sampled");
  assign(&s.coupling_squared, "Squared coupling:");
  assign(&s.total_llps_produced, "Total number of produced LLPs:");
  assign(&s.polar_acceptance, "Polar acceptance:");
  assign(&s.azimuthal_acceptance, "Azimuthal acceptance:");
  assign(&s.mean_decay_probability, "Averaged decay probability:");
  assign(&s.visible_branching_ratio, "Visible Br Ratio:");
  assign(&s.total_events, "Total number of events:");
  return s;
}

// Extract NAME from a `#<process=NAME; sample_points=N>` block header.
// NAME contains the decay arrow ("N2 -> mu- mu+"), so the terminator is the
// semicolon; only when there is none does the closing bracket end the name.
inline std::string parse_process_name(std::string const& line) {
  constexpr std::string_view key = "process=";
  auto const pos = line.find(key);
  if (pos == std::string::npos) return "unknown";
  auto const start = pos + key.size();
  auto stop = line.find(';', start);
  if (stop == std::string::npos) stop = line.rfind('>');
  if (stop == std::string::npos || stop < start) stop = line.size();
  auto const name = line.substr(start, stop - start);
  auto const begin = name.find_first_not_of(" \t");
  if (begin == std::string::npos) return "unknown";
  auto const end = name.find_last_not_of(" \t\r");
  return name.substr(begin, end - begin + 1);
}

inline void parse_row(std::string const& line, std::vector<double>* out) {
  out->clear();
  char const* cursor = line.c_str();
  while (true) {
    char* next = nullptr;
    double const value = std::strtod(cursor, &next);
    if (next == cursor) break;
    out->push_back(value);
    cursor = next;
  }
}

}  // namespace detail

// Parses an EventCalc file eagerly and serves events by index.
//
// Every accessor is const and the state is fixed after construction, so one
// instance is safe to share across Phlex worker threads without locking —
// unlike genie_reader_source, whose TTree forces concurrency::serial.
class Reader {
 public:
  explicit Reader(std::string const& file) : file_name_{file} {
    std::ifstream in{file};
    if (!in.is_open())
      throw std::runtime_error("eventcalc_source: cannot open '" + file + "'");

    // Slot 0 catches rows appearing before any block header.
    processes_.emplace_back("unknown");
    std::uint32_t current_process = 0;
    bool summary_seen = false;

    std::string line;
    std::vector<double> values;
    values.reserve(64);

    while (std::getline(in, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (line.empty()) continue;

      if (!summary_seen && line.find("Sampled") != std::string::npos) {
        summary_ = detail::parse_summary(line);
        summary_seen = true;
        continue;
      }

      if (line.front() == '#') {
        processes_.push_back(detail::parse_process_name(line));
        current_process = static_cast<std::uint32_t>(processes_.size() - 1);
        continue;
      }

      detail::parse_row(line, &values);
      if (values.size() < detail::kMotherColumns + detail::kDaughterColumns)
        continue;

      DecayEvent event;
      event.llp.momentum = {values[0], values[1], values[2]};
      event.llp.energy = values[3];
      event.llp.mass = values[4];
      event.llp.pdg = to_pdg(values[5]);
      event.decay_probability = values[6];
      event.vertex = {values[7], values[8], values[9]};
      event.process_id = current_process;

      // Decay products come in groups of six; trailing groups are padding,
      // marked by a PDG code of -999. Test the whole group, never the
      // individual value — dropping only the sentinel leaves five zeros
      // behind and shifts every later group by one column.
      for (std::size_t i = detail::kMotherColumns;
           i + detail::kDaughterColumns - 1 < values.size();
           i += detail::kDaughterColumns) {
        if (values[i + 5] < detail::kPaddingSentinel) break;
        Particle daughter;
        daughter.momentum = {values[i], values[i + 1], values[i + 2]};
        daughter.energy = values[i + 3];
        daughter.mass = values[i + 4];
        daughter.pdg = to_pdg(values[i + 5]);
        event.daughters.push_back(daughter);
      }

      weight_sum_ += event.decay_probability;
      events_.push_back(std::move(event));
    }

    if (events_.empty())
      throw std::runtime_error("eventcalc_source: no decay rows in '" + file +
                               "' — is this an EventCalc <LLP>_..._data.dat "
                               "file?");
  }

  [[nodiscard]] std::string const& file_name() const { return file_name_; }
  [[nodiscard]] FileSummary const& summary() const { return summary_; }
  [[nodiscard]] std::vector<std::string> const& processes() const {
    return processes_;
  }
  [[nodiscard]] std::size_t size() const { return events_.size(); }
  [[nodiscard]] DecayEvent const& at(std::size_t i) const {
    return events_.at(i);
  }
  [[nodiscard]] std::string const& process_name(DecayEvent const& e) const {
    return processes_.at(e.process_id);
  }
  // Sum of the per-event decay probabilities — the denominator needed to turn
  // the stored sample into an absolute event count.
  [[nodiscard]] double summed_decay_probability() const { return weight_sum_; }

 private:
  static std::int32_t to_pdg(double value) {
    return static_cast<std::int32_t>(std::lround(value));
  }

  std::string file_name_;
  FileSummary summary_;
  std::vector<std::string> processes_;
  std::vector<DecayEvent> events_;
  double weight_sum_ = 0;
};

}  // namespace aegir::eventcalc
