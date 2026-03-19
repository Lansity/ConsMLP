cmake_minimum_required(VERSION 3.10)
project(ConsMLP VERSION 1.0.0 LANGUAGES CXX)

# Set C++11 standard
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Include directories
include_directories(${PROJECT_SOURCE_DIR}/include)

# Release build flags
if(MSVC)
    set(CMAKE_CXX_FLAGS_RELEASE "/O2 /DNDEBUG /GL /fp:fast")
    set(CMAKE_CXX_FLAGS_DEBUG "/Zi /Od /W3")
    add_definitions(-D_CRT_SECURE_NO_WARNINGS)
else()
    set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG -march=native -mtune=native -flto -funroll-loops -ffast-math ")
    set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -Wall -Wextra")
endif()

# Print build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
    message(STATUS "Build type not specified: Using Release by default")
endif()
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")

# Find threads library for parallel execution
find_package(Threads REQUIRED)

# Library source files
set(LIB_SOURCES
    src/datastructures/Hypergraph.cpp
    src/datastructures/HypergraphHierarchy.cpp
    src/partitioning/Partition.cpp
    src/partitioning/PartitionConstraints.cpp
    src/partitioning/PartitionMetrics.cpp
    src/partitioning/Partitioner.cpp
    src/partitioning/MultilevelPartitioner.cpp
    src/coarsening/Coarsener.cpp
    src/coarsening/MultilevelCoarsener.cpp
    src/refinement/Refiner.cpp
    src/utils/HgrParser.cpp
)

# Main executable (demos) - commented out as not needed
# add_executable(ConsMLP src/main.cpp ${LIB_SOURCES})

# Partition HGR file executable - commented out
# add_executable(partition_hgr src/partition_hgr.cpp ${LIB_SOURCES})

# Test coarsening - commented out
# add_executable(test_coarsen src/test_coarsen.cpp ${LIB_SOURCES})

# Test refinement - commented out
# add_executable(test_refine src/test_refine.cpp ${LIB_SOURCES})

# Multilevel partition (main tool) - ONLY THIS IS COMPILED
add_executable(cons_partitioner src/multilevel_partition.cpp ${LIB_SOURCES})
target_link_libraries(cons_partitioner PRIVATE Threads::Threads)

# Comprehensive analysis tool - commented out
# add_executable(comprehensive_analysis src/comprehensive_analysis.cpp ${LIB_SOURCES})

# Profile coarsening performance - commented out
# add_executable(profile_coarsen src/profile_coarsen.cpp ${LIB_SOURCES})

# Example: Hierarchy - commented out
# add_executable(hierarchy_example src/examples/hierarchy_example.cpp ${LIB_SOURCES})

# Link libraries if needed
# target_link_libraries(ConsMLP PRIVATE ...)

