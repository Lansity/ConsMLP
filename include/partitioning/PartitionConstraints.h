#ifndef ZXPART_PARTITION_CONSTRAINTS_H
#define ZXPART_PARTITION_CONSTRAINTS_H

#include "utils/Types.h"
#include "utils/Configuration.h"
#include "datastructures/NodeType.h"
#include "datastructures/Hypergraph.h"
#include "Partition.h"
#include <vector>
#include <unordered_map>
#include <string>

namespace consmlp {

/**
 * @brief Capacity constraints per node type per partition
 */
struct CapacityConstraint {
    Weight min_capacity;  // Minimum capacity
    Weight max_capacity;  // Maximum capacity
    
    CapacityConstraint() : min_capacity(0), max_capacity(0) {}
    CapacityConstraint(Weight min_cap, Weight max_cap) 
        : min_capacity(min_cap), max_capacity(max_cap) {}
};

/**
 * @brief Manages partition constraints
 * 
 * Features:
 * - Capacity constraints per node type
 * - Fixed node constraints
 * - Balance checking
 * - O(1) constraint queries
 */
class PartitionConstraints {
public:
    /**
     * @brief Constructor
     * @param num_partitions Number of partitions
     * @param config Configuration
     */
    PartitionConstraints(PartitionID num_partitions, 
                        const Configuration& config);
    
    /**
     * @brief Initialize balanced constraints from hypergraph
     * @param hg Hypergraph
     * @param imbalance_factor Allowed imbalance (e.g., 0.03 for 3%)
     */
    void initializeBalanced(const Hypergraph& hg, double imbalance_factor);
    
    /**
     * @brief Initialize balanced constraints with per-type imbalance factors
     * LUT, FF, MUX, CARRY, OTHER use base imbalance_factor
     * DSP, BRAM, IO use relaxed_multiplier * imbalance_factor
     * @param hg Hypergraph
     * @param imbalance_factor Base imbalance factor
     * @param relaxed_multiplier Multiplier for DSP/BRAM/IO (default: 3.0)
     */
    void initializeBalancedWithTypes(const Hypergraph& hg, 
                                     double imbalance_factor,
                                     double relaxed_multiplier = 3.0);
    
    /**
     * @brief Initialize constraints from XML constraint file
     * XML format specifies absolute capacity limits for each SLR (partition)
     * @param xml_filename Path to the XML constraint file
     * @return Number of partitions parsed from the file
     */
    PartitionID initializeFromXML(const std::string& xml_filename);
    
    /**
     * @brief Check if using XML-based absolute capacity constraints
     * @return True if using XML constraints (no minimum capacity enforcement)
     */
    inline bool isXMLConstraintMode() const { return xml_constraint_mode_; }

    /**
     * @brief Enable/disable XML constraint mode (no minimum enforcement)
     */
    inline void setXmlConstraintMode(bool enabled) { xml_constraint_mode_ = enabled; }
    
    /**
     * @brief Set capacity constraint for a specific type and partition
     * @param partition_id Partition ID
     * @param type Node type
     * @param min_capacity Minimum capacity
     * @param max_capacity Maximum capacity
     */
    void setCapacity(PartitionID partition_id, NodeType type, 
                    Weight min_capacity, Weight max_capacity);
    
    /**
     * @brief Get capacity constraint
     * @param partition_id Partition ID
     * @param type Node type
     * @return Capacity constraint
     */
    CapacityConstraint getCapacity(PartitionID partition_id, 
                                   NodeType type) const;
    
    /**
     * @brief Add a fixed node constraint
     * @param node_id Node ID
     * @param partition_id Partition ID
     */
    inline void addFixedNode(NodeID node_id, PartitionID partition_id) {
        fixed_nodes_[node_id] = partition_id;
    }
    
    /**
     * @brief Check if node is fixed
     * @param node_id Node ID
     * @return True if fixed
     */
    inline bool isNodeFixed(NodeID node_id) const {
        return fixed_nodes_.find(node_id) != fixed_nodes_.end();
    }
    
    /**
     * @brief Get fixed partition for a node
     * @param node_id Node ID
     * @return Partition ID (INVALID_PARTITION if not fixed)
     */
    inline PartitionID getFixedPartition(NodeID node_id) const {
        auto it = fixed_nodes_.find(node_id);
        return it != fixed_nodes_.end() ? it->second : INVALID_PARTITION;
    }
    
    /**
     * @brief Check if adding a node violates capacity constraint (single type)
     * @param partition_id Partition ID
     * @param node_type Node type
     * @param node_weight Node weight
     * @param current_weight Current partition weight of this type
     * @return True if would violate constraint
     */
    bool wouldViolateCapacity(PartitionID partition_id, NodeType node_type,
                             Weight node_weight, Weight current_weight) const;
    
    /**
     * @brief Check if adding a multi-type node would violate any capacity constraint
     * @param partition_id Partition ID
     * @param type_weights Per-type weights of the node
     * @param partition Current partition (for getting current weights)
     * @return True if would violate any type's constraint
     */
    bool wouldViolateCapacityMultiType(PartitionID partition_id, 
                                       const TypeWeights& type_weights,
                                       const Partition& partition) const;
    
    /**
     * @brief Check if partition satisfies minimum capacity
     * @param partition Partition
     * @param partition_id Partition ID
     * @param hg Hypergraph
     * @return True if satisfies minimum
     */
    bool satisfiesMinimum(const Partition& partition, 
                         PartitionID partition_id,
                         const Hypergraph& hg) const;
    
    /**
     * @brief Check if entire partition is balanced
     * @param partition Partition
     * @param hg Hypergraph
     * @return True if balanced
     */
    bool isBalanced(const Partition& partition, const Hypergraph& hg) const;
    
    /**
     * @brief Get number of partitions
     * @return Number of partitions
     */
    inline PartitionID getNumPartitions() const { return num_partitions_; }
    
    /**
     * @brief Print constraint summary for debugging
     * @param hg Hypergraph for weight information
     */
    void printConstraintSummary(const Hypergraph& hg) const;
    
    /**
     * @brief Print partition resource distribution and constraint violations
     * @param partition Partition to check
     * @param hg Hypergraph
     */
    void printConstraintViolations(const Partition& partition, const Hypergraph& hg) const;

private:
    PartitionID num_partitions_;
    
    // Capacity constraints: [partition_id][type] -> constraint
    std::vector<std::vector<CapacityConstraint>> capacity_constraints_;
    
    // Fixed nodes: node_id -> partition_id
    std::unordered_map<NodeID, PartitionID> fixed_nodes_;
    
    // Flag indicating XML-based absolute capacity mode (no minimum enforcement)
    bool xml_constraint_mode_ = false;
    
    static constexpr size_t NUM_TYPES = NUM_NODE_TYPES;
};

} // namespace consmlp

#endif // ZXPART_PARTITION_CONSTRAINTS_H

