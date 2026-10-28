// SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration
//
// SPDX-License-Identifier: LGPL-3.0-or-later

// pythia_settings.hpp — user settings for the Pythia8-based generators
//
// Any Pythia8 setting can be passed from the job configuration, as a list of
// "Key = value" lines (`pythia_settings`) and/or a command file
// (`pythia_cmnd`, one setting per line). They are applied after aegir's own
// settings and before init(), so they can override them (processes, tunes,
// PDFs, particle data, ...). A line Pythia rejects — unknown key, bad value —
// is an error: a misspelt setting must not silently leave the default in
// place. Settings of the Random: group are refused, since aegir seeds and keys
// Pythia's random numbers itself (pythia_philox_engine.hpp, seed_config.hpp).
//
// Header-only and free of Phlex dependencies, like pythia_common.hpp.

#pragma once

#include <Pythia8/Pythia.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace SHiP::aegir {

struct PythiaSettings {
  std::vector<std::string> lines;  // "Key = value"
  std::string cmnd_file;           // read first, then `lines`

  [[nodiscard]] bool empty() const {
    return lines.empty() && cmnd_file.empty();
  }
};

// The setting a line changes, lower-cased ("" for blank and comment lines,
// which Pythia ignores: lines not starting with a letter or digit).
inline std::string pythia_setting_key(std::string_view line) {
  auto const begin = line.find_first_not_of(" \t");
  if (begin == std::string_view::npos ||
      std::isalnum(static_cast<unsigned char>(line[begin])) == 0) {
    return {};
  }
  auto const end = line.find_first_of(" \t=", begin);
  std::string key{line.substr(begin, end == std::string_view::npos
                                         ? std::string_view::npos
                                         : end - begin)};
  std::ranges::transform(key, key.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return key;
}

// All lines to apply: those of the command file, then the configured ones.
// Throws for an unreadable file and for Random: settings.
inline std::vector<std::string> pythia_setting_lines(
    PythiaSettings const& settings, std::string_view source) {
  std::vector<std::string> lines;
  if (!settings.cmnd_file.empty()) {
    std::ifstream in{settings.cmnd_file};
    if (!in) {
      throw std::invalid_argument(std::string{source} +
                                  ": cannot read pythia_cmnd '" +
                                  settings.cmnd_file + "'");
    }
    for (std::string line; std::getline(in, line);) {
      lines.push_back(line);
    }
  }
  lines.insert(lines.end(), settings.lines.begin(), settings.lines.end());
  for (auto const& line : lines) {
    if (pythia_setting_key(line).starts_with("random:")) {
      throw std::invalid_argument(
          std::string{source} + ": Pythia8 setting '" + line +
          "' refused: aegir sets the random numbers itself (use `seed`)");
    }
  }
  return lines;
}

// Apply the settings to a Pythia8::Pythia or PythiaParallel before init().
template <typename Pythia>
void apply_pythia_settings(Pythia& pythia, PythiaSettings const& settings,
                           std::string_view source) {
  for (auto const& line : pythia_setting_lines(settings, source)) {
    if (!pythia.readString(line)) {
      throw std::invalid_argument(std::string{source} +
                                  ": Pythia8 rejected the setting '" + line +
                                  "'");
    }
  }
}

// The settings that differ from Pythia's defaults, as Pythia writes them
// (one "Key = value" per line), for the log.
inline std::string changed_pythia_settings(Pythia8::Pythia& pythia) {
  std::ostringstream os;
  pythia.settings.writeFile(os, false);
  return os.str();
}

}  // namespace SHiP::aegir
