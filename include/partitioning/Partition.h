#ifndef ZXPART_PARTITION_H
#define ZXPART_PARTITION_H

#include "utils/Types.h"
#include "datastructures/NodeType.h"
#include "datastructures/Hypergraph.h"
#include <vector>
#include <cassert>

namespace consmlp {

/**
 * @brief Partition assignment and capacity tracking
 * 
 * Performance design:
 * - O(1) query and update for node partition
 * - Incremental capacity tracking per partition
 * - Separate tracking by node type for fine-grained constraints
 */
class Partition {
public:
    /**
     * @brief Constructor
     * @param num_nodes Number of nodes
     * @param num_partitions Number of partitions (k-way)
     */
    Partition(NodeID num_nodes, PartitionID num_partitions);
    
    /**
     * @brief Get partition ID of a node
     * @param node_id Node ID
     * @return Partition ID
     */
    inline PartitionID getPartition(NodeID node_id) const {
        assert(node_id < node_partitions_.size());
        return node_partitions_[node_id];
    }
    
    /**
     * @brief Set partition of a node
     * @param node_id Node ID
     * @param partition_id Partition ID
     * @param hg Hypergraph (for weight tracking)
     */
    void setPartition(NodeID node_id, PartitionID partition_id, 
                     const Hypergraph& hg);
    
    /**
     * @brief Move a node to a different partition
     * @param node_id Node ID
     * @param to_partition Target partition
     * @param hg Hypergraph
     * @return True if move is successful
     */
    bool moveNode(NodeID node_id, PartitionID to_partition, 
                  const Hypergraph& hg);
    
    /**
     * @brief Get total weight of a partition
     * @param partition_id Partition ID
     * @return Total weight
     */
    inline Weight getPartitionWeight(PartitionID partition_id) const {
        assert(partition_id < num_partitions_);
        return partition_weights_[partition_id];
    }
    
    /**
     * @brief Get weight of specific node type in a partition
     * @param partition_id Partition ID
     * @param type Node type
     * @return Weight of the specified type
     */
    Weight getPartitionWeightByType(PartitionID partition_id, 
                                    NodeType type) const;
    
    /**
     * @brief Get number of nodes in a partition
     * @param partition_id Partition ID
     * @return Number of nodes
     */
    inline NodeID getPartitionSize(PartitionID partition_id) const {
        assert(partition_id < num_partitions_);
        return partition_sizes_[partition_id];
    }
    
    /**
     * @brief Get number of partitions
     * @return Number of partitions
     */
    inline PartitionID getNumPartitions() const { return num_partitions_; }
    
    /**
     * @brief Get total number of nodes
     * @return Number of nodes
     */
    inline NodeID getNumNodes() const { 
        return static_cast<NodeID>(node_partitions_.size()); 
    }
    
    /**
     * @brief Check if partition is empty
     * @param partition_id Partition ID
     * @return True if empty
     */
    inline bool isPartitionEmpty(PartitionID partition_id) const {
        return partition_sizes_[partition_id] == 0;
    }
    
    /**
     * @brief Reset all partitions
     */
    void reset();
    
    /**
     * @brief Initialize partition with hypergraph (set all to partition 0)
     * @param hg Hypergraph
     */
    void initialize(const Hypergraph& hg);

private:
    PartitionID num_partitions_;                    // Number of partitions
    
    // Node partition assignment (O(1) access)
    std::vector<PartitionID> node_partitions_;      // Node -> Partition
    
    // Partition statistics (incremental tracking)
    std::vector<Weight> partition_weights_;         // Total weight per partition
    std::vector<NodeID> partition_sizes_;           // Number of nodes per partition
    
    // Weight by node type per partition
    // Indexed as: [partition_id * NUM_NODE_TYPES + type]
    std::vector<Weight> partition_type_weights_;
    
    /**
     * @brief Get linear index for partition and type
     */
    inline size_t getTypeIndex(PartitionID partition_id, NodeType type) const {
        return partition_id * NUM_NODE_TYPES + static_cast<size_t>(type);
    }
};

} // namespace consmlp

#endif // ZXPART_PARTITION_H

