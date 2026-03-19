#ifndef ZXPART_PARTITION_METRICS_H
#define ZXPART_PARTITION_METRICS_H

#include "utils/Types.h"
#include "datastructures/Hypergraph.h"
#include "Partition.h"

namespace consmlp {

/**
 * @brief Partition quality metrics
 * 
 * Performance design:
 * - Efficient cut size calculation
 * - Incremental update support (future)
 */
class PartitionMetrics {
public:
    /**
     * @brief Calculate cut size (connectivity metric)
     * A net is cut if it spans multiple partitions
     * @param hg Hypergraph
     * @param partition Partition
     * @return Cut size (sum of weights of cut nets)
     */
    static Weight calculateCutSize(const Hypergraph& hg, 
                                   const Partition& partition);
    
    /**
     * @brief Calculate connectivity metric (sum of (k-1) for each net)
     * where k is the number of partitions the net spans
     * @param hg Hypergraph
     * @param partition Partition
     * @return Connectivity cost
     */
    static Weight calculateConnectivity(const Hypergraph& hg,
                                       const Partition& partition);
    
    /**
     * @brief Calculate SOED (Sum of External Degrees)
     * For each net, counts pins in non-majority partitions
     * @param hg Hypergraph
     * @param partition Partition
     * @return SOED value
     */
    static Weight calculateSOED(const Hypergraph& hg,
                               const Partition& partition);
    
    /**
     * @brief Calculate imbalance ratio
     * @param partition Partition
     * @return Maximum imbalance ratio across all partitions
     */
    static double calculateImbalance(const Partition& partition);
    
    /**
     * @brief Calculate imbalance for specific node type
     * @param partition Partition
     * @param type Node type
     * @param hg Hypergraph
     * @return Imbalance ratio for the type
     */
    static double calculateTypeImbalance(const Partition& partition,
                                        NodeType type,
                                        const Hypergraph& hg);
    
    /**
     * @brief Check if a net is cut
     * @param hg Hypergraph
     * @param net_id Net ID
     * @param partition Partition
     * @return True if net is cut (spans multiple partitions)
     */
    static bool isNetCut(const Hypergraph& hg, EdgeID net_id,
                        const Partition& partition);
    
    /**
     * @brief Count number of partitions a net spans
     * @param hg Hypergraph
     * @param net_id Net ID
     * @param partition Partition
     * @return Number of unique partitions
     */
    static PartitionID countNetPartitions(const Hypergraph& hg, 
                                         EdgeID net_id,
                                         const Partition& partition);
};

} // namespace consmlp

#endif // ZXPART_PARTITION_METRICS_H

