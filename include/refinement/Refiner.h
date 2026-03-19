#ifndef ZXPART_REFINER_H
#define ZXPART_REFINER_H

#include "utils/Types.h"
#include "utils/Configuration.h"
#include "datastructures/Hypergraph.h"
#include "partitioning/Partition.h"
#include "partitioning/PartitionConstraints.h"

namespace consmlp {

/**
 * @brief Refinement statistics
 */
struct RefinementStats {
    Weight initial_cut;
    Weight final_cut;
    Weight improvement;
    uint32_t num_moves;
    uint32_t num_passes;
    
    RefinementStats()
        : initial_cut(0), final_cut(0), improvement(0)
        , num_moves(0), num_passes(0)
    {}
};

/**
 * @brief Abstract base class for refinement algorithms
 * 
 * Extensible design:
 * - Virtual interface for different refinement strategies
 * - Performance-oriented implementation
 * - Constraint-aware refinement
 */
class Refiner {
public:
    /**
     * @brief Constructor
     * @param config Configuration parameters
     */
    explicit Refiner(const Configuration& config);
    
    /**
     * @brief Virtual destructor
     */
    virtual ~Refiner() = default;
    
    /**
     * @brief Refine a partition
     * @param hg Hypergraph
     * @param partition Partition to refine (modified in-place)
     * @param constraints Partition constraints
     * @return Refinement statistics
     */
    virtual RefinementStats refine(const Hypergraph& hg,
                                   Partition& partition,
                                   const PartitionConstraints& constraints) = 0;
    
    /**
     * @brief Get algorithm name
     * @return Algorithm name string
     */
    virtual const char* getName() const = 0;

protected:
    Configuration config_;
};

/**
 * @brief FM (Fiduccia-Mattheyses) based refinement
 * 
 * Algorithm:
 * - Compute gain for each node movement
 * - Use efficient gain bucket structure
 * - Greedy moves with undo capability
 * - Multiple passes until no improvement
 * 
 * Performance: O(p) per pass where p is number of pins
 */
class FMRefiner : public Refiner {
public:
    explicit FMRefiner(const Configuration& config);
    
    RefinementStats refine(const Hypergraph& hg,
                          Partition& partition,
                          const PartitionConstraints& constraints) override;
    
    const char* getName() const override { return "FMRefiner"; }

private:
    /**
     * @brief Gain bucket for efficient node selection
     * Buckets are indexed by gain value for O(1) access
     */
    class GainBucket {
    public:
        GainBucket();
        
        /**
         * @brief Initialize with gain range
         * @param min_gain Minimum possible gain
         * @param max_gain Maximum possible gain
         */
        void initialize(int min_gain, int max_gain);
        
        /**
         * @brief Insert node with gain
         * @param node_id Node ID
         * @param gain Gain value
         */
        void insert(NodeID node_id, int gain);
        
        /**
         * @brief Remove node
         * @param node_id Node ID
         */
        void remove(NodeID node_id);
        
        /**
         * @brief Update node gain
         * @param node_id Node ID
         * @param new_gain New gain value
         */
        void updateGain(NodeID node_id, int new_gain);
        
        /**
         * @brief Get and remove node with maximum gain
         * @param node_id Output: node ID
         * @param gain Output: gain value
         * @return True if found
         */
        bool getMax(NodeID& node_id, int& gain);
        
        /**
         * @brief Check if empty
         * @return True if no nodes
         */
        bool empty() const { return size_ == 0; }
        
        /**
         * @brief Clear all nodes
         */
        void clear();
        
    private:
        int min_gain_;
        int max_gain_;
        int max_current_gain_;  // Current maximum gain in bucket
        
        // Bucket structure: gain -> list of nodes
        std::vector<std::vector<NodeID>> buckets_;
        
        // Node info
        std::vector<int> node_gains_;      // Node -> gain
        std::vector<bool> node_in_bucket_; // Node -> in bucket?
        
        size_t size_;  // Number of nodes in bucket
        
        /**
         * @brief Get bucket index for gain
         */
        inline size_t getBucketIndex(int gain) const {
            return static_cast<size_t>(gain - min_gain_);
        }
    };
    
    /**
     * @brief Move record for undo capability
     */
    struct MoveRecord {
        NodeID node_id;
        PartitionID from_partition;
        PartitionID to_partition;
        Weight cut_delta;
        
        MoveRecord(NodeID n, PartitionID f, PartitionID t, Weight d)
            : node_id(n), from_partition(f), to_partition(t), cut_delta(d)
        {}
    };
    
    /**
     * @brief Perform one refinement pass
     * @param hg Hypergraph
     * @param partition Partition
     * @param constraints Constraints
     * @param pass_number Current pass number (0-indexed)
     * @param first_pass_moves Number of moves in first pass (for adaptive strategy)
     * @return Cut improvement
     */
    Weight performPass(const Hypergraph& hg,
                      Partition& partition,
                      const PartitionConstraints& constraints,
                      uint32_t pass_number,
                      size_t first_pass_moves);
    
    /**
     * @brief Compute gain for moving node to target partition
     * Gain = reduction in cut size
     * @param hg Hypergraph
     * @param partition Partition
     * @param node_id Node to move
     * @param to_partition Target partition
     * @return Gain (positive = improvement)
     */
    int computeGain(const Hypergraph& hg,
                    const Partition& partition,
                    NodeID node_id,
                    PartitionID to_partition) const;
    
    /**
     * @brief Initialize gain buckets for all partitions
     * @param hg Hypergraph
     * @param partition Partition
     */
    void initializeGains(const Hypergraph& hg,
                        const Partition& partition);
    
    /**
     * @brief Update gains of neighbors after a move
     * @param hg Hypergraph
     * @param partition Partition
     * @param moved_node Moved node
     */
    void updateNeighborGains(const Hypergraph& hg,
                            const Partition& partition,
                            NodeID moved_node);
    
    /**
     * @brief Check if move is valid (respects constraints)
     * @param hg Hypergraph
     * @param partition Partition
     * @param constraints Constraints
     * @param node_id Node to move
     * @param to_partition Target partition
     * @return True if valid
     */
    bool isValidMove(const Hypergraph& hg,
                    const Partition& partition,
                    const PartitionConstraints& constraints,
                    NodeID node_id,
                    PartitionID to_partition) const;
    
    // Per-partition gain buckets
    std::vector<GainBucket> gain_buckets_;
    
    // Locked nodes (already moved in current pass)
    std::vector<bool> locked_;
};

/**
 * @brief Simple greedy refinement (faster but less quality)
 */
class GreedyRefiner : public Refiner {
public:
    explicit GreedyRefiner(const Configuration& config);
    
    RefinementStats refine(const Hypergraph& hg,
                          Partition& partition,
                          const PartitionConstraints& constraints) override;
    
    const char* getName() const override { return "GreedyRefiner"; }

private:
    Weight performPass(const Hypergraph& hg,
                      Partition& partition,
                      const PartitionConstraints& constraints);
};

/**
 * @brief Greedy FM refinement - optimized version that only initializes cut nodes
 * 
 * Key optimization:
 * - Only initialize gains for nodes connected to cut nets (boundary nodes)
 * - Lazily add adjacent nodes to gain bucket after moves
 * - Significantly faster than standard FM for large graphs
 */
class GreedyFMRefiner : public Refiner {
public:
    explicit GreedyFMRefiner(const Configuration& config);
    
    RefinementStats refine(const Hypergraph& hg,
                          Partition& partition,
                          const PartitionConstraints& constraints) override;
    
    const char* getName() const override { return "GreedyFMRefiner"; }

private:
    /**
     * @brief Gain bucket (reuse from FMRefiner)
     */
    class GainBucket {
    public:
        GainBucket();
        void initialize(int min_gain, int max_gain);
        void insert(NodeID node_id, int gain);
        void remove(NodeID node_id);
        void updateGain(NodeID node_id, int new_gain);
        bool getMax(NodeID& node_id, int& gain);
        bool empty() const { return size_ == 0; }
        void clear();
        bool contains(NodeID node_id) const {
            return node_id < node_in_bucket_.size() && node_in_bucket_[node_id];
        }
        
    private:
        int min_gain_;
        int max_gain_;
        int max_current_gain_;
        std::vector<std::vector<NodeID>> buckets_;
        std::vector<int> node_gains_;
        std::vector<bool> node_in_bucket_;
        size_t size_;
        
        inline size_t getBucketIndex(int gain) const {
            return static_cast<size_t>(gain - min_gain_);
        }
    };
    
    struct MoveRecord {
        NodeID node_id;
        PartitionID from_partition;
        PartitionID to_partition;
        Weight cut_delta;
        
        MoveRecord(NodeID n, PartitionID f, PartitionID t, Weight d)
            : node_id(n), from_partition(f), to_partition(t), cut_delta(d) {}
    };
    
    /**
     * @brief Perform one greedy FM pass
     */
    Weight performPass(const Hypergraph& hg,
                      Partition& partition,
                      const PartitionConstraints& constraints,
                      uint32_t pass_number);
    
    /**
     * @brief Initialize gains only for nodes on cut nets (boundary nodes)
     */
    void initializeBoundaryGains(const Hypergraph& hg,
                                 const Partition& partition);
    
    /**
     * @brief Compute gain for moving node to target partition
     */
    int computeGain(const Hypergraph& hg,
                    const Partition& partition,
                    NodeID node_id,
                    PartitionID to_partition) const;
    
    /**
     * @brief Update gains of neighbors and add new boundary nodes to bucket
     */
    void updateNeighborGainsAndAddToBucket(const Hypergraph& hg,
                                           const Partition& partition,
                                           NodeID moved_node);
    
    /**
     * @brief Check if move is valid
     */
    bool isValidMove(const Hypergraph& hg,
                    const Partition& partition,
                    const PartitionConstraints& constraints,
                    NodeID node_id,
                    PartitionID to_partition) const;
    
    // Per-partition gain buckets
    std::vector<GainBucket> gain_buckets_;
    
    // Locked nodes
    std::vector<bool> locked_;
    
    // Track which nodes have been added to bucket
    std::vector<bool> in_bucket_;
};

} // namespace consmlp

#endif // ZXPART_REFINER_H

