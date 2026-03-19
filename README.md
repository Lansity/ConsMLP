# ConsMLP - Hypergraph Multilevel Partitioning

## Part 1: Algorithm Principle & Features

ConsMLP implements a multilevel hypergraph partitioning flow for k-way partitioning. The core pipeline is:

1. **Coarsening**: Iteratively contract nodes to build a hierarchy of smaller hypergraphs while preserving connectivity. This reduces problem size and improves the quality of the initial cut.
2. **Initial Partitioning**: Find an initial 2-way or k-way split on the coarsest level (random / GHG / all trials). This provides a good starting point for refinement.
3. **Uncoarsening + Refinement**: Project the partition back to finer levels and improve it using FM-style refiners while respecting balance/constraint rules.

Key features:
- **Direct k-way or Recursive bipartition** (`-mode direct|recursive`): recursive mode splits into 2-way partitions until k is reached (requires k to be a power of two).
- **Constraint-aware partitioning**:
  - **Balance mode**: constrain total weight by imbalance factor.
  - **Types mode** (`-types`): per-resource capacity constraints based on node types.
  - **XML mode** (`-xml`): absolute capacity constraints per SLR/resource type.
- **Multiple coarsening/refinement strategies**: select algorithm variants via command-line options.
- **Deterministic runs** with `-seed`.

## Part 2: Parameters, Meaning, Defaults

Below are the user-facing parameters (from CLI) and their defaults as implemented in the code.

### Partitioning & Constraints
- `-k <num>`: number of partitions (k-way). Default: `2`.
- `-imbalance <f>`: imbalance factor (e.g., 0.05 = 5%). Default: `0.05`.
- `-mode <direct|recursive>`: direct k-way or recursive bipartition. Default: `direct`.
- `-types <file>`: per-node type file (LUT/FF/MUX/CARRY/IO/DSP/BRAM/OTHER). Default: not set (balance mode).
- `-xml <file>`: XML constraint file with per-SLR resource limits. Default: not set.
- `-relaxed <f>`: relaxed multiplier for DSP/BRAM/IO when using type constraints. Default: `3.0`.

### Coarsening
- `-coarsen <heavy|first|cluster>`: coarsening strategy. Default: `cluster`.
- `-threshold <num>`: coarsening stop threshold (#nodes). Default: `100`.
- `-coarsen_opt`: enable coarsen optimization (skip large nets when small net found). Default: `off`.

### Initial Partitioning
- `-init <rand|ghg|all>`: initial partitioning strategy. Default: `rand`.
- `initial_partition_runs`: number of random trials (internal config). Default: `10`.
- `trial_refine_levels`: number of levels used for trial refinement (internal config). Default: `2`.

### Refinement
- `-refine <fm|greedyfm|greedy>`: refinement strategy. Default: `gfm` (Greedy FM).
- `-passes <num>`: max refinement passes. Default: `10`.

### Output & Reproducibility
- `-seed <num>`: random seed. Default: `42`.
- `-output <file>`: output partition file. Default: not set.

### Internal Defaults (not exposed via CLI)
- `contraction_limit`: `0.5` (coarsening contraction ratio).
- `large_net_threshold`: `500` (auto-adjusted based on data).

