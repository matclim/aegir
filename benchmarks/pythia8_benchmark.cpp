// SPDX-FileCopyrightText: 2026 CERN for the benefit of the SHiP Collaboration
//
// SPDX-License-Identifier: LGPL-3.0-or-later

// pythia8_benchmark.cpp — Standalone Pythia8 benchmark
//
// Benchmarks Pythia8 event generation and particle extraction without any
// Phlex dependency.  Compares serial Pythia and (optionally) PythiaParallel.

#include <Pythia8/Pythia.h>
#include <Pythia8/PythiaParallel.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>

#include "pythia_common.hpp"

// Local MCParticle struct matching data_products.hpp layout, so the benchmark
// stays free of the SHiP data-model dependency while sharing the extraction
// logic via aegir::extract_particles<MCParticle>.
struct MCParticle {
  std::int32_t pdgCode{0};
  std::array<double, 3> vertex{0, 0, 0};
  std::array<double, 3> momentum{0, 0, 0};
  double energy{0};
  double time{0};
  std::int32_t motherId{-1};
  std::int32_t status{1};
};

struct Stats {
  double mean;
  double stddev;
  double min;
  double max;
  double total;
  int count;
};

Stats compute_stats(std::vector<double> const& times) {
  Stats s{};
  s.count = static_cast<int>(times.size());
  if (s.count == 0) return s;

  s.total = std::accumulate(times.begin(), times.end(), 0.0);
  s.mean = s.total / s.count;
  s.min = *std::min_element(times.begin(), times.end());
  s.max = *std::max_element(times.begin(), times.end());

  double sq_sum = 0.0;
  for (auto t : times) sq_sum += (t - s.mean) * (t - s.mean);
  s.stddev = std::sqrt(sq_sum / s.count);

  return s;
}

void print_stats(char const* label, Stats const& s) {
  std::cout << label << ":\n"
            << "  events:     " << s.count << "\n"
            << "  total:      " << s.total << " ms\n"
            << "  mean:       " << s.mean << " ms/event\n"
            << "  stddev:     " << s.stddev << " ms\n"
            << "  min:        " << s.min << " ms\n"
            << "  max:        " << s.max << " ms\n"
            << "  throughput: "
            << (s.total > 0 ? s.count / (s.total / 1000.0) : 0)
            << " events/s\n\n";
}

template <typename T>
void configure_pythia(T& pythia, double beam_energy, std::string const& process,
                      bool fairship) {
  aegir::configure_beams(pythia, 2212, 2212, beam_energy);
  pythia.readString("Print:quiet = on");

  if (fairship) {
    pythia.readString("SoftQCD:inelastic = on");
    pythia.readString("PhotonCollision:gmgm2mumu = on");
    pythia.readString("PromptPhoton:all = on");
    pythia.readString("WeakBosonExchange:all = on");
    pythia.readString("WeakSingleBoson:all = on");
  } else {
    pythia.readString(process + " = on");
  }
}

using Clock = std::chrono::steady_clock;

double elapsed_ms(Clock::time_point start, Clock::time_point end) {
  return std::chrono::duration<double, std::milli>(end - start).count();
}

struct Config {
  int events = 1000;
  int warmup = 10;
  int threads = 0;
  double beam_energy = 400.0;
  std::string process = "SoftQCD:inelastic";
  bool fairship = false;
};

Config parse_args(int argc, char* argv[]) {
  Config cfg;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--events" && i + 1 < argc)
      cfg.events = std::stoi(argv[++i]);
    else if (arg == "--warmup" && i + 1 < argc)
      cfg.warmup = std::stoi(argv[++i]);
    else if (arg == "--threads" && i + 1 < argc)
      cfg.threads = std::stoi(argv[++i]);
    else if (arg == "--beam-energy" && i + 1 < argc)
      cfg.beam_energy = std::stod(argv[++i]);
    else if (arg == "--process" && i + 1 < argc) {
      cfg.process = argv[++i];
      cfg.fairship = false;
    } else if (arg == "--fairship")
      cfg.fairship = true;
    else if (arg == "--help" || arg == "-h") {
      std::cout << "Usage: " << argv[0] << " [options]\n"
                << "  --events N        Number of events (default: 1000)\n"
                << "  --warmup N        Warmup events (default: 10)\n"
                << "  --threads N       PythiaParallel threads; 0=serial only "
                   "(default: 0)\n"
                << "  --beam-energy E   Beam energy in GeV (default: 400)\n"
                << "  --process P       Pythia process string (default: "
                   "SoftQCD:inelastic)\n"
                << "  --fairship        Use FairShip-like configuration\n";
      std::exit(0);
    } else {
      std::cerr << "Unknown argument: " << arg << "\n";
      std::exit(1);
    }
  }
  return cfg;
}

std::string xml_dir() {
  if (auto const* env = std::getenv("PYTHIA8DATA")) return std::string{env};
  return std::string{"../share/Pythia8/xmldoc"};
}

int main(int argc, char* argv[]) {
  auto cfg = parse_args(argc, argv);

  std::cout << "=== Pythia8 Standalone Benchmark ===\n"
            << "Events: " << cfg.events << ", Warmup: " << cfg.warmup
            << ", Beam energy: " << cfg.beam_energy << " GeV\n"
            << "Config: " << (cfg.fairship ? "FairShip-like" : cfg.process)
            << "\n";
  if (cfg.threads > 0)
    std::cout << "PythiaParallel threads: " << cfg.threads << "\n";
  std::cout << "\n";

  // ── Serial benchmarks ──────────────────────────────────────────────

  {
    Pythia8::Pythia pythia(xml_dir(), false);
    configure_pythia(pythia, cfg.beam_energy, cfg.process, cfg.fairship);
    pythia.init();
    if (cfg.fairship) aegir::stabilise_long_lived(pythia, 1.0);

    // Warmup
    for (int i = 0; i < cfg.warmup; ++i) pythia.next();

    // Generation only
    std::vector<double> gen_times;
    gen_times.reserve(cfg.events);
    for (int i = 0; i < cfg.events; ++i) {
      auto t0 = Clock::now();
      pythia.next();
      auto t1 = Clock::now();
      gen_times.push_back(elapsed_ms(t0, t1));
    }
    print_stats("Serial: generation only", compute_stats(gen_times));
  }

  {
    Pythia8::Pythia pythia(xml_dir(), false);
    configure_pythia(pythia, cfg.beam_energy, cfg.process, cfg.fairship);
    pythia.init();
    if (cfg.fairship) aegir::stabilise_long_lived(pythia, 1.0);

    // Warmup
    for (int i = 0; i < cfg.warmup; ++i) {
      pythia.next();
      aegir::extract_particles<MCParticle>(pythia.event);
    }

    // Generation + extraction
    std::vector<double> full_times;
    full_times.reserve(cfg.events);
    for (int i = 0; i < cfg.events; ++i) {
      auto t0 = Clock::now();
      pythia.next();
      auto parts = aegir::extract_particles<MCParticle>(pythia.event);
      auto t1 = Clock::now();
      full_times.push_back(elapsed_ms(t0, t1));
    }
    print_stats("Serial: generation + extraction", compute_stats(full_times));
  }

  // ── PythiaParallel benchmark ───────────────────────────────────────

  if (cfg.threads > 0) {
    Pythia8::PythiaParallel pythia(xml_dir(), false);
    configure_pythia(pythia, cfg.beam_energy, cfg.process, cfg.fairship);
    pythia.readString("Parallelism:numThreads = " +
                      std::to_string(cfg.threads));
    pythia.init();
    if (cfg.fairship) aegir::stabilise_long_lived(pythia, 1.0);

    // Warmup
    int warmup_count = 0;
    pythia.run(cfg.warmup, [&](Pythia8::Pythia* p) {
      aegir::extract_particles<MCParticle>(p->event);
      ++warmup_count;
    });

    // Re-initialise for clean benchmark
    Pythia8::PythiaParallel pythia2(xml_dir(), false);
    configure_pythia(pythia2, cfg.beam_energy, cfg.process, cfg.fairship);
    pythia2.readString("Parallelism:numThreads = " +
                       std::to_string(cfg.threads));
    pythia2.init();
    if (cfg.fairship) aegir::stabilise_long_lived(pythia2, 1.0);

    int event_count = 0;
    auto t0 = Clock::now();
    pythia2.run(cfg.events, [&](Pythia8::Pythia* p) {
      aegir::extract_particles<MCParticle>(p->event);
      ++event_count;
    });
    auto t1 = Clock::now();

    double total_ms = elapsed_ms(t0, t1);
    std::cout << "PythiaParallel (" << cfg.threads << " threads):\n"
              << "  events:     " << event_count << "\n"
              << "  total:      " << total_ms << " ms\n"
              << "  throughput: "
              << (total_ms > 0 ? event_count / (total_ms / 1000.0) : 0)
              << " events/s\n\n";
  }

  return 0;
}
