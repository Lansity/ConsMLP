#ifndef ZXPART_PARTITIONER_H
#define ZXPART_PARTITIONER_H

#include "utils/Types.h"
#include "utils/Configuration.h"
#include "datastructures/Hypergraph.h"
#include "Partition.h"
#include "PartitionConstraints.h"

namespace consmlp {

/**
 * @brief Abstract base class for partitioning algorithms
 * 
 * Extensible design:
 * - Virtual interface for different partitioning strategies
 * - Supports initial partitioning and refinement
 * - Configuration-based parameter passing
 */
class Partitioner {
public:
    /**
     * @brief Constructor
     * @param config Configuration parameters
     */
    explicit Partitioner(const Configuration& config);
    
    /**
     * @brief Virtual destructor
     */
    virtual ~Partitioner() = default;
    
    /**
     * @brief Partition the hypergraph
     * @param hg Hypergraph to partition
     * @param constraints Partition constraints
     * @return Partition result
     */
    virtual Partition partition(const Hypergraph& hg,
                               const PartitionConstraints& constraints) = 0;
    
    /**
     * @brief Get algorithm name
     * @return Algorithm name string
     */
    virtual const char* getName() const = 0;

protected:
    Configuration config_;
    
    /**
     * @brief Apply fixed node constraints
     * @param hg Hypergraph
     * @param constraints Constraints
     * @param partition Partition to update
     */
    void applyFixedConstraints(const Hypergraph& hg,
                              const PartitionConstraints& constraints,
                              Partition& partition);
};

/**
 * @brief Simple greedy partitioner for initial partitioning
 * 
 * Algorithm:
 * - Assign nodes to partitions in a balanced greedy manner
 * - Respects capacity constraints
 * - Fast O(n) initial partitioning
 */
class GreedyPartitioner : public Partitioner {
public:
    explicit GreedyPartitioner(const Configuration& config);
    
    Partition partition(const Hypergraph& hg,
                       const PartitionConstraints& constraints) override;
    
    const char* getName() const override { return "GreedyPartitioner"; }
};

/**
 * @brief Random partitioner (baseline)
 * 
 * Algorithm:
 * - Randomly assign nodes while respecting constraints
 * - Useful for testing and baseline comparison
 */
class RandomPartitioner : public Partitioner {
public:
    explicit RandomPartitioner(const Configuration& config);
    
    Partition partition(const Hypergraph& hg,
                       const PartitionConstraints& constraints) override;
    
    const char* getName() const override { return "RandomPartitioner"; }
};

/**
 * @brief Greedy Hypergraph Growing (GHG) Partitioner
 * 
 * Algorithm:
 * - Start with a seed node in partition 0
 * - Greedily grow partition 0 by adding nodes that minimize cut increase
 * - Continue until partition 0 reaches target size
 * - Assign remaining nodes to partition 1
 * - Only works for k=2 (bipartitioning)
 */
class GHGPartitioner : public Partitioner {
public:
    explicit GHGPartitioner(const Configuration& config);
    
    Partition partition(const Hypergraph& hg,
                       const PartitionConstraints& constraints) override;
    
    const char* getName() const override { return "GHGPartitioner"; }

private:
    /**
     * @brief Compute gain of adding a node to partition 0
     * @param hg Hypergraph
     * @param partition Current partition
     * @param node_id Node to evaluate
     * @return Gain value (negative means cut increase)
     */
    int computeGain(const Hypergraph& hg,
                    const Partition& partition,
                    NodeID node_id) const;
};

/**
 * @brief Optimized GHG Partitioner with cut-aware BFS
 * 
 * Algorithm:
 * - Similar to GHG but uses cut-aware node selection
 * - During BFS expansion, prioritizes nodes that minimize cut increase
 * - Considers both connectivity and cut size impact
 * - Only works for k=2 (bipartitioning)
 */
class GHGOptPartitioner : public Partitioner {
public:
    explicit GHGOptPartitioner(const Configuration& config);
    
    Partition partition(const Hypergraph& hg,
                       const PartitionConstraints& constraints) override;
    
    const char* getName() const override { return "GHGOptPartitioner"; }

private:
    /**
     * @brief Compute the impact on cut size if node is added to partition 0
     * @param hg Hypergraph
     * @param partition Current partition
     * @param node_id Node to evaluate
     * @return Cut delta (negative means cut reduction)
     */
    int computeCutDelta(const Hypergraph& hg,
                        const Partition& partition,
                        NodeID node_id) const;
    
    /**
     * @brief Select seed node based on strategy
     * @param hg Hypergraph
     * @param assigned Already assigned nodes
     * @param strategy Seed selection strategy (0-4)
     * @return Selected seed node
     */
    NodeID selectSeedNode(const Hypergraph& hg,
                          const std::vector<bool>& assigned,
                          int strategy) const;
};

/**
 * @brief Enforce balance constraints on a partition
 * 
 * If the partition violates balance constraints, move nodes between
 * partitions to satisfy the constraints.
 * 
 * @param hg Hypergraph
 * @param partition Partition to fix (modified in place)
 * @param constraints Balance constraints
 * @param use_types Whether to consider node types for balance
 * @return true if partition was successfully balanced, false otherwise
 */
bool enforceBalanceConstraints(const Hypergraph& hg,
                               Partition& partition,
                               const PartitionConstraints& constraints,
                               bool use_types);

} // namespace consmlp

#endif // ZXPART_PARTITIONER_H

