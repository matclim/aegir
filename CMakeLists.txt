cmake_minimum_required(VERSION 3.21)
project(
    aegir
    VERSION 0.2.0
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

# Dependencies
find_package(SHiPDataModel REQUIRED)
find_package(SHiPFieldService REQUIRED COMPONENTS G4Adapter)

# Phlex's cmake config does not call find_dependency for its transitive deps
# (still true as of phlex 0.3.0: its generated deps block is empty), so declare
# them explicitly before find_package(phlex).
find_package(Boost REQUIRED COMPONENTS json)
find_package(TBB REQUIRED)
find_package(spdlog REQUIRED)
find_package(phlex REQUIRED)
find_package(
    ROOT
    6.40
    REQUIRED
    COMPONENTS Core RIO Hist MathCore ROOTNTuple
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

find_package(SHiPGeometryService CONFIG QUIET)

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
    LIBS SHiP::SHiPDataModel
    INCLUDES ${Random123_INCLUDE_DIR}
)

aegir_add_plugin(
    sim_output_module
    LIBS
        SHiP::SHiPDataModel
        ROOT::Hist
        ROOT::RIO
        ROOT::ROOTHistUtil
        ROOT::ROOTNTuple
        spdlog::spdlog
)

aegir_add_plugin(
    geometry_builtin_provider
    LIBS SHiP::SHiPDataModel ${Geant4_LIBRARIES}
    INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/src
)

aegir_add_plugin(
    geometry_gdml_provider
    LIBS SHiP::SHiPDataModel ${Geant4_LIBRARIES}
    INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# GeoModel geometry provider plugin (optional)
if(SHiPGeometryService_FOUND)
    message(
        STATUS
        "SHiPGeometryService found — building geometry_geomodel_provider"
    )
    aegir_add_plugin(
        geometry_geomodel_provider
        LIBS
            SHiP::SHiPDataModel
            ${Geant4_LIBRARIES}
            SHiPGeometryService::SHiPGeometryService
        INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
else()
    message(
        STATUS
        "SHiPGeometryService not found — skipping geometry_geomodel_provider"
    )
endif()

aegir_add_plugin(
    geant4_module
    LIBS
        SHiP::SHiPDataModel
        ${Geant4_LIBRARIES}
        spdlog::spdlog
        SHiPFieldService::SHiPFieldService
        SHiPFieldService::G4Adapter
    INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/src
    TRACE
)

aegir_add_plugin(field_covfie_provider LIBS SHiPFieldService::SHiPFieldService)
aegir_add_plugin(field_null_provider LIBS SHiPFieldService::SHiPFieldService)

aegir_add_plugin(
    pythia8_source
    LIBS SHiP::SHiPDataModel Pythia8::Pythia8
)

aegir_add_plugin(
    fixed_target_source
    LIBS SHiP::SHiPDataModel Pythia8::Pythia8 spdlog::spdlog
    INCLUDES ${Random123_INCLUDE_DIR}
)

# Standalone Pythia8 benchmark (no Phlex dependency)
add_executable(pythia8_benchmark benchmarks/pythia8_benchmark.cpp)
target_link_libraries(pythia8_benchmark PRIVATE Pythia8::Pythia8)
target_include_directories(
    pythia8_benchmark
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Install
include(GNUInstallDirs)

get_property(PLUGIN_TARGETS GLOBAL PROPERTY AEGIR_PLUGIN_TARGETS)
install(TARGETS ${PLUGIN_TARGETS} LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(TARGETS pythia8_benchmark RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY workflows/ DESTINATION share/aegir/workflows)

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
