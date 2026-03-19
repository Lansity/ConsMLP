# ConsMLP Agent Guide

This file contains essential information for AI agents working on the ConsMLP project.

## Project Overview

**ConsMLP** is a high-performance hypergraph multilevel partitioning library for k-way partitioning, written in C++11. It is specifically designed for FPGA design partitioning with support for multi-resource constraints (LUT, FF, DSP, BRAM, IO, etc.).

### Core Algorithm Pipeline

The implementation follows the three-stage multilevel paradigm:

1. **Coarsening Phase**: Iteratively contract nodes to build a hierarchy of smaller hypergraphs while preserving connectivity structure. This reduces problem size and improves the quality of the initial cut.
2. **Initial Partitioning Phase**: Find an initial k-way split on the coarsest level using random, GHG (Greedy Hypergraph Growing), or optimized GHG strategies.
3. **Uncoarsening + Refinement Phase**: Project the partition back to finer levels and iteratively improve it using FM-style (Fiduccia-Mattheyses) refinement algorithms while respecting balance and resource constraints.

### Key Features

- **Direct k-way or Recursive bipartition** (`-mode direct|recursive`): recursive mode splits into 2-way partitions until k is reached (requires k to be a power of two).
- **Three constraint-aware partitioning modes**:
  - **Balance mode** (default): constrain total weight by imbalance factor.
  - **Types mode** (`-types`): per-resource capacity constraints based on node types (LUT/FF/MUX/CARRY/IO/DSP/BRAM).
  - **XML mode** (`-xml`): absolute capacity constraints per SLR/resource type from XML file.
- **Multiple coarsening strategies**: Heavy Edge Matching, First Choice Matching, Cluster Matching.
- **Multiple refinement strategies**: Standard FM, Greedy FM (default), Simple Greedy.
- **Deterministic runs** with configurable random seed.
- **Parallel processing** using C++11 threads for recursive bipartition.

---

## Build System

### Prerequisites

- CMake >= 3.10
- C++11 compatible compiler (GCC/Clang/MSVC)
- POSIX threads library (pthreads)

### Build Commands

```bash
# Create build directory and compile
mkdir -p build && cd build
cmake ..
make -j$(nproc)

# The main executable is: cons_partitioner
```

### Build Types

- **Release** (default): Optimized with `-O3 -march=native -mtune=native -flto -funroll-loops -ffast-math`
- **Debug**: Use `cmake -DCMAKE_BUILD_TYPE=Debug ..` for debugging with symbols (`-g -O0 -Wall -Wextra`)

### MSVC Build Flags

On Windows with MSVC:
- Release: `/O2 /DNDEBUG /GL /fp:fast`
- Debug: `/Zi /Od /W3`

---

## Project Architecture

### Directory Structure

```
ConsMLP/
├── CMakeLists.txt              # Main build configuration
├── README.md                   # User-facing documentation
├── AGENTS.md                   # This file - agent guide
├── .cursorrules                # AI agent rules (Chinese)
├── project_introduction.md     # Technical introduction (Chinese)
├── include/                    # Header files
│   ├── coarsening/             # Coarsening algorithms
│   │   ├── Coarsener.h         # Base class + HEM/FCM/Cluster
│   │   └── MultilevelCoarsener.h
│   ├── datastructures/         # Core data structures
│   │   ├── Hypergraph.h        # Main hypergraph data structure
│   │   ├── HypergraphHierarchy.h
│   │   └── NodeType.h          # FPGA node type definitions
│   ├── partitioning/           # Partitioning algorithms
│   │   ├── MultilevelPartitioner.h  # Main application class
│   │   ├── Partitioner.h       # Initial partitioning algorithms
│   │   ├── Partition.h         # Partition data structure
│   │   ├── PartitionConstraints.h
│   │   └── PartitionMetrics.h
│   ├── refinement/             # Refinement strategies
│   │   └── Refiner.h           # FM/GreedyFM/Greedy refiners
│   └── utils/                  # Utilities
│       ├── Configuration.h     # Configuration struct
│       ├── HgrParser.h         # hMetis format parser
│       ├── Timer.h             # Performance timing
│       ├── Types.h             # Type aliases
│       └── Profiler.h          # Performance profiling
├── src/                        # Source files (mirrors include/)
│   ├── multilevel_partition.cpp # Main entry point
│   ├── coarsening/
│   ├── datastructures/
│   ├── partitioning/
│   ├── refinement/
│   └── utils/
├── benchmarks/                 # Benchmark test cases (.hgr files)
└── ss_benchmarks/              # Additional benchmarks
```

### Namespace

All code resides in namespace `consmlp`.

### Key Components

| Component | Description | Key Files |
|-----------|-------------|-----------|
| Hypergraph | Core data structure using CSR (Compressed Sparse Row) format for cache-friendly memory layout | `Hypergraph.h/cpp` |
| HypergraphHierarchy | Manages multilevel hierarchy with forward/backward mappings between levels | `HypergraphHierarchy.h/cpp` |
| Coarsener | Abstract base + HEM/FCM/Cluster matching implementations | `Coarsener.h/cpp` |
| MultilevelCoarsener | Orchestrates the coarsening phase | `MultilevelCoarsener.h/cpp` |
| Partitioner | Initial partitioning (Random/GHG/GHGOpt/Greedy) | `Partitioner.h/cpp` |
| Refiner | FM/GreedyFM/Greedy refinement implementations | `Refiner.h/cpp` |
| PartitionConstraints | Balance/type/XML constraint management | `PartitionConstraints.h/cpp` |
| MultilevelPartitionerApp | Main application class, CLI parsing, orchestration | `MultilevelPartitioner.h/cpp` |

---

## Coding Style Guidelines

### Naming Conventions

- **Classes/Structs**: PascalCase (e.g., `MultilevelPartitioner`, `Hypergraph`)
- **Functions**: camelCase (e.g., `coarsenOnce()`, `computeGain()`)
- **Variables**: snake_case (e.g., `num_partitions`, `node_weights`)
- **Constants/MACROS**: UPPER_CASE with namespace prefix or `k` prefix (e.g., `kMaxRatingNetDegree`)
- **Member variables**: No special prefix (avoid `m_` or `_` prefix)
- **Template parameters**: PascalCase with descriptive names

### Type Aliases (from `include/utils/Types.h`)

```cpp
using NodeID = uint32_t;       // Node/vertex identifier (0-indexed internally)
using EdgeID = uint32_t;       // Hyperedge/net identifier
using PartitionID = uint32_t;  // Partition identifier
using Weight = int32_t;        // Node/net weights
using Index = uint32_t;        // Array index type

// Special values
constexpr NodeID INVALID_NODE = std::numeric_limits<NodeID>::max();
constexpr EdgeID INVALID_EDGE = std::numeric_limits<EdgeID>::max();
```

### Code Format

- **Indentation**: 4 spaces (no tabs)
- **Braces**: K&R style (opening brace on same line)
- **Max line length**: ~100 characters
- **Comments**: Use Doxygen-style `/** */` for public APIs, `//` for inline comments

### Example Code Style

```cpp
namespace consmlp {

/**
 * @brief Brief description of the function
 * @param node_id Node identifier to process
 * @param weight Weight value to apply
 * @return True if operation succeeded
 */
bool processNode(NodeID node_id, Weight weight) {
    if (node_id >= num_nodes_) {
        return false;  // Early return for invalid input
    }
    
    // Process the node
    node_weights_[node_id] += weight;
    
    return true;
}

class MyClass {
public:
    MyClass() : member_var_(0) {}
    
    void doSomething(NodeID node_id);
    
private:
    int member_var_;
    std::vector<Weight> weights_;
};

} // namespace consmlp
```

---

## Testing Strategy

### Test Scripts

The project uses shell scripts for running benchmark tests:

1. **Small Cases Test** (`scripts/run_small_cases.sh`):
   ```bash
   # Run all small cases from ss_benchmarks/
   ./scripts/run_small_cases.sh [case_list_file] [max_jobs]
   
   # Default runs with k=4, recursive mode, cluster coarsening, gfm refinement
   # Results saved to scripts/results/summary.csv
   ```

2. **Large Cases Test** (`benchmarks/run_large_cases.sh`):
   ```bash
   # Run large benchmark cases
   ./benchmarks/run_large_cases.sh [list_file] [xml_file] [max_jobs]
   ```

### Test Case Format

Test cases are listed in `.list` files with format:
```
path/to/case1 [optional_imbalance_factor]
path/to/case2
# Comments start with #
```

### Running Tests

```bash
# Build first
mkdir -p build && cd build && cmake .. && make -j$(nproc)
cd ..

# Run small cases
./scripts/run_small_cases.sh

# Run specific case list
./scripts/run_small_cases.sh ./scripts/small_case.list 4
```

---

## Input/Output Formats

### Hypergraph Format (.hgr)

The partitioner uses the hMetis hypergraph format with extensions:

**Standard hMetis format:**
```
<num_nets> <num_nodes> [format_flag]
```
- `format_flag` (optional, default=0):
  - 0: no weights
  - 1: hyperedge weights
  - 10: vertex weights
  - 11: both weights

**Extended format (optimized for faster parsing):**
```
<mode> <num_nodes> <num_nets> <total_pins>
```
- `mode`: 1=no weights, 2=node weights, 3=net weights, 4=both

**Node type file format (-types option):**
- One type per line, in node order (0, 1, 2, ...)
- Valid types: LUT, FF, MUX, CARRY, IO, DSP, BRAM, OTHER
- Example (from .insts files):
  ```
  LUTFF
  LUTFF
  LUT
  DSP
  ```

### XML Constraint Format (-xml option)

```xml
<SLR0>
  <LUT> <428000>
  <FF> <8560000>
  <MUX> <340000>
  <CARRY> <120000>
  <IO> <200>
  <DSP> <3072>
  <BRAM> <672>
  <OTHER> <200000>
</SLR0>
...
```

### Output Partition File

The output file contains one partition ID per line (0-indexed), in node order:
```
0
0
1
1
0
...
```

---

## Command-Line Interface

### Usage

```bash
./cons_partitioner <hgr_file> [options]
```

### Options

| Option | Description | Default |
|--------|-------------|---------|
| `-k <num>` | Number of partitions | 2 |
| `-imbalance <f>` | Imbalance factor (e.g., 0.03 = 3%) | 0.03 |
| `-mode <direct\|recursive>` | Partitioning strategy | direct |
| `-init <rand\|ghg\|ghg_opt\|all>` | Initial partitioning method | rand |
| `-coarsen <heavy\|first\|cluster>` | Coarsening algorithm | cluster |
| `-threshold <num>` | Coarsening stop threshold (#nodes) | 100 |
| `-coarsen_opt` | Enable coarsening optimization | off |
| `-refine <fm\|gfm\|greedy>` | Refinement algorithm | gfm |
| `-passes <num>` | Maximum refinement passes | 10 |
| `-seed <num>` | Random seed | 42 |
| `-types <file>` | Node type file | (none) |
| `-xml <file>` | XML constraint file | (none) |
| `-relaxed <f>` | Relaxed multiplier for DSP/BRAM/IO | 3.0 |
| `-output <file>` | Output partition file | (none) |

### Example Commands

```bash
# Basic 4-way partitioning
./cons_partitioner benchmarks/case1.hgr -k 4

# Recursive bipartition (k must be power of 2)
./cons_partitioner benchmarks/case1.hgr -k 8 -mode recursive

# With type constraints
./cons_partitioner benchmarks/case1.hgr -k 4 -types benchmarks/case1.insts

# With XML constraints
./cons_partitioner benchmarks/case1.hgr -k 4 -xml constraints.xml

# Custom algorithms and seed
./cons_partitioner benchmarks/case1.hgr -k 4 -coarsen cluster -refine gfm -seed 123

# Higher imbalance tolerance
./cons_partitioner benchmarks/case1.hgr -k 4 -imbalance 0.10
```

---

## Node Types and Constraints

### FPGA Node Types

Defined in `include/datastructures/NodeType.h`:

| Type | Description | Relaxed Imbalance |
|------|-------------|-------------------|
| LUT | Look-Up Table | No |
| FF | Flip-Flop | No |
| MUX | Multiplexer | No |
| CARRY | Carry chain | No |
| IO | Input/Output pins | Yes (3x default) |
| DSP | Digital Signal Processor | Yes (3x default) |
| BRAM | Block RAM | Yes (3x default) |
| OTHER | Other/unknown | No |

### Constraint Modes

1. **Balance Mode** (default): Only total node weight is balanced using imbalance factor.

2. **Types Mode** (`-types <file>`): Each resource type has its own capacity constraint. DSP/BRAM/IO use relaxed multiplier.

3. **XML Mode** (`-xml <file>`): Absolute capacity limits per partition per resource type from XML file. No minimum capacity enforcement.

---

## Algorithm Details

### Coarsening Strategies

1. **Heavy Edge Matching (HEM)**: O(|E|) complexity. Computes rating based on edge weights, matches greedily by rating.

2. **First Choice Matching (FCM)**: O(|V|) expected. Visits nodes in random order, matches with first unmatched neighbor. Fastest for large graphs.

3. **Cluster Matching (default)**: Aggressive clustering allowing 2+ nodes per cluster. Uses `adjMatchAreaJudging` for area constraints. Contraction ratio can reach >3.0.

### Initial Partitioning Strategies

1. **Random**: Multiple random trials with refinement, picks best result.
2. **GHG**: Greedy Hypergraph Growing from seed nodes.
3. **GHGOpt**: Optimized GHG with cut-aware BFS and multiple seed strategies.

### Refinement Strategies

1. **FMRefiner**: Standard FM with gain buckets (O(1) max gain access), move records for undo capability.
2. **GreedyFMRefiner** (default): Only initializes gains for boundary nodes (cut nets). Lazily adds new boundary nodes. Significantly faster for large graphs.
3. **GreedyRefiner**: Simple greedy approach, fastest but lowest quality.

---

## Common Development Tasks

### Adding a New Coarsening Strategy

1. Create a new class inheriting from `Coarsener` in `include/coarsening/Coarsener.h`
2. Implement `coarsen()` and `getName()` methods in `src/coarsening/Coarsener.cpp`
3. Add factory case in `createCoarsener()` in `src/partitioning/MultilevelPartitioner.cpp`
4. Update `printUsage()` with new option

### Adding a New Refinement Algorithm

1. Create a new class inheriting from `Refiner` in `include/refinement/Refiner.h`
2. Implement `refine()` and `getName()` methods in `src/refinement/Refiner.cpp`
3. Add factory case in `createRefiner()` in `src/partitioning/MultilevelPartitioner.cpp`
4. Update `printUsage()` with new option

### Adding Configuration Parameters

1. Add field to `Configuration` struct in `include/utils/Configuration.h`
2. Set default value in the constructor
3. Parse in `MultilevelPartitionerApp::parseArguments()` in `src/partitioning/MultilevelPartitioner.cpp`
4. Use via `config_.parameter_name`
5. Document in `printUsage()`

---

## Important Implementation Notes

### Thread Safety

- The code uses C++11 threads for parallel recursive bipartition.
- Shared data structures are protected by `std::mutex`.
- `Hypergraph` and `Partition` are not thread-safe for concurrent modifications.

### Memory Management

- Use RAII patterns throughout.
- `Hypergraph` manages its own memory with `finalize()` for CSR construction.
- Use `clear()` or destructors to release memory.
- Large hypergraphs can consume significant memory during coarsening.

### Performance Considerations

- **CSR Format**: Hypergraph uses Structure of Arrays (SoA) for cache efficiency.
- **Large Net Threshold**: Nets larger than threshold are skipped in coarsening (auto-adjusted based on graph size).
- **Two-pass Parsing**: HgrParser uses optimized two-pass CSR construction.
- **Lazy Gain Computation**: GreedyFM only computes gains for boundary nodes.

### Random Seed

- Default seed is 42 for reproducibility.
- Use `-seed` option for different random sequences.
- Multiple trials use `seed + trial_index` for variation.

---

## Debugging and Logging

- Use `std::cout` for progress/info messages.
- Use `std::cerr` for error messages.
- The code includes timing output at each phase (coarsening, initial partitioning, refinement).
- Debug output for XML constraints includes partition capacity details.

---

## Dependencies

- **Standard Library only** (C++11)
- `<thread>` and `<mutex>` for parallelism
- No external libraries required
- CMake for build system

---

## File Modification Guidelines

When modifying code:

1. **Headers**: Update corresponding `.cpp` if interface changes.
2. **CLI**: Update `printUsage()` when adding new options.
3. **Defaults**: Document default values in both code and comments.
4. **Error Handling**: Use early returns with descriptive error messages to `stderr`.
5. **Logging**: Use `std::cout` for progress/info, `std::cerr` for errors.
6. **Language**: Documentation and comments are primarily in English; some Chinese documentation exists in `project_introduction.md` and `.cursorrules`.

---

## Project Statistics

- **Total Lines of Code**: ~9,500 lines (C++ headers and sources)
- **Main Executable**: `cons_partitioner`
- **Supported Platforms**: Linux, macOS, Windows (via CMake)

---

## References

- hMetis format: http://glaros.dtc.umn.edu/gkhome/metis/hmetis/overview
- FM Algorithm: Fiduccia and Mattheyses, "A Linear-Time Heuristic for Improving Network Partitions" (1982)
- Multilevel Paradigm: Hendrickson and Leland, "A Multilevel Algorithm for Partitioning Graphs" (1995)
