#include "partitioning/PartitionMetrics.h"
#include <vector>
#include <algorithm>
#include <cmath>

namespace consmlp {

Weight PartitionMetrics::calculateCutSize(const Hypergraph& hg,
                                         const Partition& partition) {
    Weight cut_size = 0;
    
    for (EdgeID net_id = 0; net_id < hg.getNumNets(); ++net_id) {
        // Skip global nets
        if (hg.isNetGlobal(net_id)) {
            continue;
        }
        
        if (isNetCut(hg, net_id, partition)) {
            cut_size += hg.getNetWeight(net_id);
        }
    }
    
    return cut_size;
}

Weight PartitionMetrics::calculateConnectivity(const Hypergraph& hg,
                                              const Partition& partition) {
    Weight connectivity = 0;
    
    for (EdgeID net_id = 0; net_id < hg.getNumNets(); ++net_id) {
        if (hg.isNetGlobal(net_id)) {
            continue;
        }
        
        PartitionID num_parts = countNetPartitions(hg, net_id, partition);
        
        // Connectivity metric: (k - 1) where k is number of partitions spanned
        if (num_parts > 1) {
            connectivity += hg.getNetWeight(net_id) * (num_parts - 1);
        }
    }
    
    return connectivity;
}

Weight PartitionMetrics::calculateSOED(const Hypergraph& hg,
                                      const Partition& partition) {
    Weight soed = 0;
    
    for (EdgeID net_id = 0; net_id < hg.getNumNets(); ++net_id) {
        if (hg.isNetGlobal(net_id)) {
            continue;
        }
        
        // Count pins in each partition
        std::vector<Index> partition_pin_count(partition.getNumPartitions(), 0);
        
        auto nodes = hg.getNetNodes(net_id);
        for (const NodeID* it = nodes.first; it != nodes.second; ++it) {
            PartitionID part_id = partition.getPartition(*it);
            if (part_id != INVALID_PARTITION) {
                partition_pin_count[part_id]++;
            }
        }
        
        // Find majority partition (partition with most pins)
        Index max_pins = *std::max_element(partition_pin_count.begin(),
                                          partition_pin_count.end());
        
        // SOED: sum of pins not in majority partition
        Index total_pins = hg.getNetSize(net_id);
        Index external_pins = total_pins - max_pins;
        
        soed += hg.getNetWeight(net_id) * external_pins;
    }
    
    return soed;
}

double PartitionMetrics::calculateImbalance(const Partition& partition) {
    if (partition.getNumPartitions() == 0) {
        return 0.0;
    }
    
    // Calculate average weight
    Weight total_weight = 0;
    for (PartitionID part_id = 0; part_id < partition.getNumPartitions(); ++part_id) {
        total_weight += partition.getPartitionWeight(part_id);
    }
    
    double avg_weight = static_cast<double>(total_weight) / 
                       partition.getNumPartitions();
    
    if (avg_weight == 0.0) {
        return 0.0;
    }
    
    // Find maximum deviation from average
    double max_imbalance = 0.0;
    for (PartitionID part_id = 0; part_id < partition.getNumPartitions(); ++part_id) {
        double current_weight = static_cast<double>(
            partition.getPartitionWeight(part_id));
        double imbalance = std::abs(current_weight - avg_weight) / avg_weight;
        max_imbalance = std::max(max_imbalance, imbalance);
    }
    
    return max_imbalance;
}

double PartitionMetrics::calculateTypeImbalance(const Partition& partition,
                                               NodeType type,
                                               const Hypergraph& hg) {
    if (partition.getNumPartitions() == 0) {
        return 0.0;
    }
    
    // Calculate total weight of this type
    Weight total_type_weight = hg.getNodeWeightByType(type);
    
    if (total_type_weight == 0) {
        return 0.0;
    }
    
    double avg_weight = static_cast<double>(total_type_weight) /
                       partition.getNumPartitions();
    
    // Find maximum deviation
    double max_imbalance = 0.0;
    for (PartitionID part_id = 0; part_id < partition.getNumPartitions(); ++part_id) {
        double current_weight = static_cast<double>(
            partition.getPartitionWeightByType(part_id, type));
        double imbalance = std::abs(current_weight - avg_weight) / avg_weight;
        max_imbalance = std::max(max_imbalance, imbalance);
    }
    
    return max_imbalance;
}

bool PartitionMetrics::isNetCut(const Hypergraph& hg, EdgeID net_id,
                               const Partition& partition) {
    auto nodes = hg.getNetNodes(net_id);
    
    if (nodes.first == nodes.second) {
        return false;  // Empty net
    }
    
    // Get partition of first node
    PartitionID first_partition = partition.getPartition(*nodes.first);
    
    // Check if all nodes are in same partition
    for (const NodeID* it = nodes.first + 1; it != nodes.second; ++it) {
        if (partition.getPartition(*it) != first_partition) {
            return true;  // Found different partition, net is cut
        }
    }
    
    return false;  // All nodes in same partition
}

PartitionID PartitionMetrics::countNetPartitions(const Hypergraph& hg,
                                                EdgeID net_id,
                                                const Partition& partition) {
    std::vector<bool> seen_partitions(partition.getNumPartitions(), false);
    PartitionID count = 0;
    
    auto nodes = hg.getNetNodes(net_id);
    for (const NodeID* it = nodes.first; it != nodes.second; ++it) {
        PartitionID part_id = partition.getPartition(*it);
        if (part_id != INVALID_PARTITION && !seen_partitions[part_id]) {
            seen_partitions[part_id] = true;
            count++;
        }
    }
    
    return count;
}

} // namespace consmlp

