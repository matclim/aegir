cmake_minimum_required(VERSION 3.21)
project(
    aegir
    VERSION 0.5.0
    DESCRIPTION "Phlex-based simulation framework for the SHiP experiment"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(
    AEGIR_ENABLE_TRACE
    "Emit Chrome Trace Event JSON from the Geant4 module for profiling"
    OFF
)

option(
    AEGIR_TLS_INITIAL_EXEC
    "Compile the dlopened geant4_module with -ftls-model=initial-exec (relies on the loader's surplus static TLS; disable if plugin loading fails with 'cannot allocate memory in static TLS block')"
    ON
)

# Dependencies
find_package(SHiPDataModel 0.6...<0.7 REQUIRED)
find_package(SHiPFieldService 0.5...<0.6 REQUIRED COMPONENTS G4Adapter MapIO)

find_package(spdlog REQUIRED)
find_package(phlex 0.4.1 REQUIRED)
find_package(
    ROOT
    6.40
    REQUIRED
    COMPONENTS Core RIO Tree Hist MathCore ROOTNTuple
)
find_package(Geant4 REQUIRED)

# Pythia8 has no CMake config; create an imported target from pythia8-config
find_program(PYTHIA8_CONFIG pythia8-config REQUIRED)
execute_process(
    COMMAND ${PYTHIA8_CONFIG} --prefix
    OUTPUT_VARIABLE PYTHIA8_PREFIX
    OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY
)
if(NOT PYTHIA8_PREFIX OR NOT EXISTS "${PYTHIA8_PREFIX}/include")
    message(
        FATAL_ERROR
        "pythia8-config --prefix returned '${PYTHIA8_PREFIX}', which is not a Pythia8 installation"
    )
endif()
add_library(Pythia8::Pythia8 SHARED IMPORTED)
set_target_properties(
    Pythia8::Pythia8
    PROPERTIES
        IMPORTED_LOCATION
            "${PYTHIA8_PREFIX}/lib/libpythia8${CMAKE_SHARED_LIBRARY_SUFFIX}"
        INTERFACE_INCLUDE_DIRECTORIES "${PYTHIA8_PREFIX}/include"
)

find_path(Random123_INCLUDE_DIR Random123/philox.h REQUIRED)
find_package(SHiPRandom 0.4 REQUIRED)

# SHiP::geometry_thread() and SHiP::GeometryService::sharedFromFile are
# shared between geant4_module and any other Geant4 geometry user (e.g.
# aegir-genie's GENIE source), see aegir-genie issue #11. >=0.6:
# geant4WorldLogical() returns a reference and the library requires C++23.
find_package(SHiPGeometryService 0.6...<0.7 CONFIG REQUIRED)

# Declare a Phlex plugin built from src/<name>.cpp: create the MODULE library,
# link phlex::module plus the given LIBS, add any INCLUDES, apply the trace
# define when TRACE is set, and record the target for install.
# Note: ${Geant4_LIBRARIES} already expands to the exported Geant4:: imported
# targets, so callers link those directly through it.
function(aegir_add_plugin name)
    cmake_parse_arguments(ARG "TRACE" "" "LIBS;INCLUDES" ${ARGN})
    add_library(${name} MODULE src/${name}.cpp)
    target_link_libraries(${name} PRIVATE phlex::module ${ARG_LIBS})
    if(ARG_INCLUDES)
        target_include_directories(${name} PRIVATE ${ARG_INCLUDES})
    endif()
    if(ARG_TRACE AND AEGIR_ENABLE_TRACE)
        target_compile_definitions(${name} PRIVATE AEGIR_ENABLE_TRACE)
    endif()
    set_property(GLOBAL APPEND PROPERTY AEGIR_PLUGIN_TARGETS ${name})
endfunction()

aegir_add_plugin(
    particle_gun_source
    LIBS SHiP::DataModel SHiP::Units spdlog::spdlog SHiP::Random
)

aegir_add_plugin(
    sim_output_module
    LIBS
        SHiP::DataModel
        ROOT::Hist
        ROOT::RIO
        ROOT::ROOTHistUtil
        ROOT::ROOTNTuple
        spdlog::spdlog
)

aegir_add_plugin(
    geometry_builtin_provider
    LIBS SHiP::DataModel SHiP::Units ${Geant4_LIBRARIES}
    INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/src
)

aegir_add_plugin(
    geometry_gdml_provider
    LIBS SHiP::DataModel ${Geant4_LIBRARIES}
    INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/src
)

aegir_add_plugin(
    geometry_geomodel_provider
    LIBS
        SHiP::DataModel
        ${Geant4_LIBRARIES}
        SHiP::GeometryService
    INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/src
)

aegir_add_plugin(
    geant4_module
    LIBS
        SHiP::DataModel
        SHiP::Units
        ${Geant4_LIBRARIES}
        spdlog::spdlog
        SHiP::FieldService
        SHiP::FieldServiceG4Adapter
        SHiP::GeometryService
    INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/src ${Random123_INCLUDE_DIR}
    TRACE
)
# The plugin is dlopened, so its thread_local hit buffers default to the
# global-dynamic TLS model (__tls_get_addr on every sensitive-detector
# call). initial-exec claims static TLS space instead; glibc reserves
# surplus for dlopened objects. If plugin loading ever fails with
# "cannot allocate memory in static TLS block", configure with
# -DAEGIR_TLS_INITIAL_EXEC=OFF.
if(AEGIR_TLS_INITIAL_EXEC)
    target_compile_options(geant4_module PRIVATE -ftls-model=initial-exec)
endif()

aegir_add_plugin(field_map_provider LIBS SHiP::FieldServiceMapIO)
aegir_add_plugin(field_null_provider LIBS SHiP::FieldService)

aegir_add_plugin(
    pythia8_source
    LIBS SHiP::DataModel SHiP::Units Pythia8::Pythia8 spdlog::spdlog
        SHiP::Random
)

aegir_add_plugin(
    fixed_target_source
    LIBS SHiP::DataModel SHiP::Units Pythia8::Pythia8 spdlog::spdlog
        SHiP::Random
)

aegir_add_plugin(
    file_source
    LIBS SHiP::DataModel ROOT::ROOTNTuple ROOT::RIO spdlog::spdlog
    INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/src
)

aegir_add_plugin(
    genie_reader_source
    LIBS SHiP::DataModel SHiP::Units ROOT::RIO ROOT::Tree
)

# Standalone Pythia8 benchmark (no Phlex dependency)
add_executable(pythia8_benchmark benchmarks/pythia8_benchmark.cpp)
target_link_libraries(pythia8_benchmark PRIVATE Pythia8::Pythia8 SHiP::Units)
target_include_directories(
    pythia8_benchmark
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Install
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

get_property(PLUGIN_TARGETS GLOBAL PROPERTY AEGIR_PLUGIN_TARGETS)
install(TARGETS ${PLUGIN_TARGETS} LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(TARGETS pythia8_benchmark RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY workflows/ DESTINATION share/aegir/workflows)

# The generator-source helper, for event-generator plugins built outside
# aegir (e.g. aegir-genie): find_package(aegir), link aegir::source_helper
# and #include <aegir/mc_particle_source.hpp>.
add_library(aegir_source_helper INTERFACE)
add_library(aegir::source_helper ALIAS aegir_source_helper)
set_target_properties(aegir_source_helper PROPERTIES EXPORT_NAME source_helper)
target_include_directories(
    aegir_source_helper
    INTERFACE $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(
    aegir_source_helper
    INTERFACE phlex::module SHiP::DataModel
)
install(
    FILES src/mc_particle_source.hpp
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/aegir
)
install(TARGETS aegir_source_helper EXPORT aegirTargets)
install(
    EXPORT aegirTargets
    NAMESPACE aegir::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/aegir
)
configure_package_config_file(
    cmake/aegirConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/aegirConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/aegir
)
# 0.x: a minor release may change the helper's interface.
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/aegirConfigVersion.cmake
    COMPATIBILITY SameMinorVersion
)
install(
    FILES
        ${CMAKE_CURRENT_BINARY_DIR}/aegirConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/aegirConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/aegir
)

# Tests
enable_testing()

# Determinism: same seed -> identical generator output. Runs the gun workflow
# twice and compares validation histograms. Relies on PHLEX_PLUGIN_PATH being
# set (activate.sh does this under `pixi run`).
add_test(
    NAME determinism
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/determinism.sh
)

# Exhaustion: the parallel Pythia8 source must throw, not emit empty events,
# when the driver requests more events than its configured num_events.
add_test(
    NAME pythia_exhaustion
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/pythia_exhaustion.sh
)

# file_source round-trip: write an RNTuple, read it back through file_source +
# Geant4, and check the count helper, read-all, the skip offset, the
# re-simulation, and the errors for a missing or non-MCParticle product.
add_test(
    NAME file_source_roundtrip
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/file_source_roundtrip.sh
)

# Event header round-trip: file_source must publish the header stored in the
# input, and fall back to the unweighted default for a file written before the
# event_header field existed. Reads back via mc_only, so no Geant4.
add_test(
    NAME event_header_roundtrip
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/event_header_roundtrip.sh
)

# Physics-list failure: an unknown physics_list must fail cleanly (catchable
# error, no abort) even when concurrent init retries race on std::call_once.
add_test(
    NAME physics_list_failure
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/physics_list_failure.sh
)

# Source stage: a generator source block without its own `stage` key, or with
# the reserved 'CURRENT', must fail with a clear error.
add_test(
    NAME source_stage
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/source_stage.sh
)

# Writer pool: many threads share few fill contexts with tiny clusters; no
# event may be lost or duplicated (issue #77).
add_test(
    NAME writer_pool
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/writer_pool.sh
)

# Field deflection: a generated 0.5 T map goes through field_map_provider and
# the muon must follow the analytic circle (needs field-service-tools).
add_test(
    NAME field_deflection
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/field_deflection.sh
)

# Thread independence: the Pythia-based generators write the same events at
# -j 1 and -j 8 for a fixed seed (issue #133).
add_test(
    NAME thread_independence
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/thread_independence.sh
)

# Generator status: only HepMC status-1 (final-state) entries become Geant4
# primaries; decayed and beam entries are skipped unless track_all_primaries.
add_test(
    NAME generator_status
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/generator_status.sh
)
# Physics parameters end to end: Pythia8 settings, Geant4 parameters and
# commands reach the generators and Geant4, run_info is stored in the output,
# and bad settings fail cleanly.
add_test(
    NAME physics_parameters
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/physics_parameters.sh
)

# Scoring planes: neutrinos and muons fired through planes placed in the
# builtin geometry are recorded where the geometry says they cross, the
# killing plane stops them, and bad plane configurations fail cleanly.
add_test(
    NAME scoring_planes
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/scoring_planes.sh
)

# Plane particles: the replay record of a recording scoring plane holds each
# recorded track once with status 1, and its full ancestry as history, built
# here from a hand-made event without Geant4.
add_executable(plane_particles_test tests/plane_particles_test.cpp)
target_link_libraries(
    plane_particles_test
    PRIVATE SHiP::SHiPDataModel SHiP::SHiPUnits
)
target_include_directories(
    plane_particles_test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
add_test(NAME plane_particles COMMAND plane_particles_test)

# Sensitive volumes: each sensitive_volumes entry gets the right detector id,
# and bad entries are rejected when the configuration is read.
add_executable(sensitive_volumes_test tests/sensitive_volumes_test.cpp)
target_link_libraries(
    sensitive_volumes_test
    PRIVATE SHiP::SHiPDataModel phlex::configuration
)
target_include_directories(
    sensitive_volumes_test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
add_test(NAME sensitive_volumes COMMAND sensitive_volumes_test)

# Physics parameters: hadronic parameters, FTF developer parameters and UI
# commands from the configuration reach Geant4 in PreInit, and unknown names,
# rejected values and failing commands are errors rather than silently ignored
# or a process abort.
add_executable(geant4_tuning_test tests/geant4_tuning_test.cpp)
target_link_libraries(
    geant4_tuning_test
    PRIVATE phlex::configuration SHiP::Units Geant4::G4physicslists
)
target_include_directories(
    geant4_tuning_test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
add_test(NAME geant4_tuning COMMAND geant4_tuning_test)
