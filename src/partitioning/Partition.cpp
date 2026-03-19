#include "partitioning/Partition.h"

namespace consmlp {

Partition::Partition(NodeID num_nodes, PartitionID num_partitions)
    : num_partitions_(num_partitions)
    , node_partitions_(num_nodes, INVALID_PARTITION)
    , partition_weights_(num_partitions, 0)
    , partition_sizes_(num_partitions, 0)
    , partition_type_weights_(num_partitions * NUM_NODE_TYPES, 0)
{
}

void Partition::setPartition(NodeID node_id, PartitionID partition_id,
                             const Hypergraph& hg) {
    assert(node_id < node_partitions_.size());
    assert(partition_id < num_partitions_);
    
    PartitionID old_partition = node_partitions_[node_id];
    
    // Skip if already in target partition
    if (old_partition == partition_id) {
        return;
    }
    
    Weight node_weight = hg.getNodeWeight(node_id);
    const TypeWeights& type_weights = hg.getNodeTypeWeights(node_id);
    
    // Remove from old partition (update all type weights)
    if (old_partition != INVALID_PARTITION) {
        partition_weights_[old_partition] -= node_weight;
        partition_sizes_[old_partition]--;
        for (size_t t = 0; t < NUM_NODE_TYPES; ++t) {
            if (type_weights[t] > 0) {
                partition_type_weights_[getTypeIndex(old_partition, static_cast<NodeType>(t))] -= type_weights[t];
            }
        }
    }
    
    // Add to new partition (update all type weights)
    node_partitions_[node_id] = partition_id;
    partition_weights_[partition_id] += node_weight;
    partition_sizes_[partition_id]++;
    for (size_t t = 0; t < NUM_NODE_TYPES; ++t) {
        if (type_weights[t] > 0) {
            partition_type_weights_[getTypeIndex(partition_id, static_cast<NodeType>(t))] += type_weights[t];
        }
    }
}

bool Partition::moveNode(NodeID node_id, PartitionID to_partition,
                         const Hypergraph& hg) {
    if (node_partitions_[node_id] == to_partition) {
        return false;  // Already in target partition
    }
    
    setPartition(node_id, to_partition, hg);
    return true;
}

Weight Partition::getPartitionWeightByType(PartitionID partition_id,
                                          NodeType type) const {
    assert(partition_id < num_partitions_);
    return partition_type_weights_[getTypeIndex(partition_id, type)];
}

void Partition::reset() {
    std::fill(node_partitions_.begin(), node_partitions_.end(), INVALID_PARTITION);
    std::fill(partition_weights_.begin(), partition_weights_.end(), 0);
    std::fill(partition_sizes_.begin(), partition_sizes_.end(), 0);
    std::fill(partition_type_weights_.begin(), partition_type_weights_.end(), 0);
}

void Partition::initialize(const Hypergraph& hg) {
    reset();
    
    // Initialize all nodes to partition 0
    for (NodeID node_id = 0; node_id < hg.getNumNodes(); ++node_id) {
        if (!hg.isNodeIgnored(node_id)) {
            setPartition(node_id, 0, hg);
        }
    }
}

} // namespace consmlp
