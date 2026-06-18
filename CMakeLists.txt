cmake_minimum_required(VERSION 3.21)
project(
    aegir
    VERSION 0.1.0
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
)
add_library(Pythia8::Pythia8 SHARED IMPORTED)
set_target_properties(
    Pythia8::Pythia8
    PROPERTIES
        IMPORTED_LOCATION
            "${PYTHIA8_PREFIX}/lib/libpythia8${CMAKE_SHARED_LIBRARY_SUFFIX}"
        INTERFACE_INCLUDE_DIRECTORIES "${PYTHIA8_PREFIX}/include"
)

find_path(Random123_INCLUDE_DIR Random123/philox.h REQUIRED)

# Particle gun source plugin
add_library(particle_gun_source MODULE src/particle_gun_source.cpp)
target_link_libraries(
    particle_gun_source
    PRIVATE phlex::module SHiP::SHiPDataModel
)
target_include_directories(particle_gun_source PRIVATE ${Random123_INCLUDE_DIR})

# Simulation output module plugin
add_library(sim_output_module MODULE src/sim_output_module.cpp)
target_link_libraries(
    sim_output_module
    PRIVATE
        phlex::module
        SHiP::SHiPDataModel
        ROOT::Hist
        ROOT::RIO
        ROOT::ROOTHistUtil
        ROOT::ROOTNTuple
        spdlog::spdlog
)

# Builtin geometry provider plugin
add_library(geometry_builtin_provider MODULE src/geometry_builtin_provider.cpp)
target_link_libraries(
    geometry_builtin_provider
    PRIVATE phlex::module SHiP::SHiPDataModel ${Geant4_LIBRARIES}
)
target_include_directories(
    geometry_builtin_provider
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# GDML geometry provider plugin
add_library(geometry_gdml_provider MODULE src/geometry_gdml_provider.cpp)
target_link_libraries(
    geometry_gdml_provider
    PRIVATE phlex::module SHiP::SHiPDataModel ${Geant4_LIBRARIES}
)
target_include_directories(
    geometry_gdml_provider
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# GeoModel geometry provider plugin (optional)
find_package(SHiPGeometryService CONFIG QUIET)
if(SHiPGeometryService_FOUND)
    message(
        STATUS
        "SHiPGeometryService found — building geometry_geomodel_provider"
    )
    add_library(
        geometry_geomodel_provider
        MODULE
        src/geometry_geomodel_provider.cpp
    )
    target_link_libraries(
        geometry_geomodel_provider
        PRIVATE
            phlex::module
            SHiP::SHiPDataModel
            ${Geant4_LIBRARIES}
            SHiPGeometryService::SHiPGeometryService
    )
    target_include_directories(
        geometry_geomodel_provider
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
else()
    message(
        STATUS
        "SHiPGeometryService not found — skipping geometry_geomodel_provider"
    )
endif()

# Geant4 simulation module plugin
add_library(geant4_module MODULE src/geant4_module.cpp)
target_link_libraries(
    geant4_module
    PRIVATE
        phlex::module
        SHiP::SHiPDataModel
        ${Geant4_LIBRARIES}
        spdlog::spdlog
        SHiPFieldService::SHiPFieldService
        SHiPFieldService::G4Adapter
)
target_include_directories(
    geant4_module
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(AEGIR_ENABLE_TRACE)
    target_compile_definitions(geant4_module PRIVATE AEGIR_ENABLE_TRACE)
endif()

# Field provider plugins
add_library(field_covfie_provider MODULE src/field_covfie_provider.cpp)
target_link_libraries(
    field_covfie_provider
    PRIVATE phlex::module SHiPFieldService::SHiPFieldService
)

add_library(field_null_provider MODULE src/field_null_provider.cpp)
target_link_libraries(
    field_null_provider
    PRIVATE phlex::module SHiPFieldService::SHiPFieldService
)

# Pythia8 source plugin
add_library(pythia8_source MODULE src/pythia8_source.cpp)
target_link_libraries(
    pythia8_source
    PRIVATE phlex::module SHiP::SHiPDataModel Pythia8::Pythia8 spdlog::spdlog
)

# Fixed-target source plugin
add_library(fixed_target_source MODULE src/fixed_target_source.cpp)
target_link_libraries(
    fixed_target_source
    PRIVATE phlex::module SHiP::SHiPDataModel Pythia8::Pythia8 spdlog::spdlog
)
target_include_directories(fixed_target_source PRIVATE ${Random123_INCLUDE_DIR})

# Standalone Pythia8 benchmark (no Phlex dependency)
add_executable(pythia8_benchmark benchmarks/pythia8_benchmark.cpp)
target_link_libraries(pythia8_benchmark PRIVATE Pythia8::Pythia8)

# Install
include(GNUInstallDirs)

set(PLUGIN_TARGETS
    particle_gun_source
    pythia8_source
    fixed_target_source
    geometry_builtin_provider
    geometry_gdml_provider
    field_covfie_provider
    field_null_provider
    geant4_module
    sim_output_module
)
if(SHiPGeometryService_FOUND)
    list(APPEND PLUGIN_TARGETS geometry_geomodel_provider)
endif()

install(TARGETS ${PLUGIN_TARGETS} LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(TARGETS pythia8_benchmark RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY workflows/ DESTINATION share/aegir/workflows)
