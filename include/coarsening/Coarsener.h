#ifndef ZXPART_COARSENER_H
#define ZXPART_COARSENER_H

#include "utils/Types.h"
#include "utils/Configuration.h"
#include "utils/Profiler.h"
#include "datastructures/Hypergraph.h"
#include "datastructures/HypergraphHierarchy.h"
#include <vector>
#include <limits>

namespace consmlp {

/**
 * @brief Coarsening statistics
 */
struct CoarseningStats {
    NodeID original_nodes;
    NodeID coarse_nodes;
    EdgeID original_nets;
    EdgeID coarse_nets;
    double contraction_ratio;
    uint32_t num_matched_pairs;
    uint32_t num_singletons;
    
    CoarseningStats()
        : original_nodes(0), coarse_nodes(0)
        , original_nets(0), coarse_nets(0)
        , contraction_ratio(0.0)
        , num_matched_pairs(0), num_singletons(0)
    {}
};

/**
 * @brief Abstract base class for coarsening algorithms
 * 
 * Extensible design:
 * - Virtual interface for different coarsening strategies
 * - Constraint-aware coarsening
 * - Performance-oriented implementation
 */
class Coarsener {
public:
    /**
     * @brief Constructor
     * @param config Configuration parameters
     */
    explicit Coarsener(const Configuration& config);
    
    /**
     * @brief Virtual destructor
     */
    virtual ~Coarsener() = default;
    
    /**
     * @brief Coarsen a hypergraph and add to hierarchy
     * @param hierarchy Hypergraph hierarchy
     * @param level_idx Current level index (fine level)
     * @param profiler Optional profiler for performance analysis
     * @return Coarsening statistics
     */
    virtual CoarseningStats coarsen(HypergraphHierarchy& hierarchy,
                                   uint32_t level_idx,
                                   Profiler* profiler = nullptr) = 0;
    
    /**
     * @brief Get algorithm name
     * @return Algorithm name string
     */
    virtual const char* getName() const = 0;
    
    /**
     * @brief Check if should stop coarsening
     * @param num_nodes Number of nodes in current graph
     * @return True if should stop
     */
    bool shouldStopCoarsening(NodeID num_nodes) const;

protected:
    Configuration config_;
    
    uint32_t curr_level_idx_;
    
    /**
     * @brief Check if node can be coarsened
     * Nodes that cannot be coarsened:
     * - IO type nodes
     * - Ignored nodes
     * @param hg Hypergraph
     * @param node_id Node ID
     * @return True if can be coarsened
     */
    bool canCoarsenNode(const Hypergraph& hg, NodeID node_id) const;
    
    /**
     * @brief Check if two nodes can be matched together
     * @param hg Hypergraph
     * @param node1 First node
     * @param node2 Second node
     * @return True if can be matched
     */
    bool canMatchNodes(const Hypergraph& hg, NodeID node1, NodeID node2) const;
};

/**
 * @brief Heavy Edge Matching coarsening
 * 
 * Algorithm:
 * - Compute rating for each node pair based on edge weights
 * - Match nodes greedily in order of rating
 * - Handle constraints (IO, fixed, type)
 * - Detect and handle parallel nets
 * 
 * Performance: O(|E|) time complexity
 */
class HeavyEdgeMatching : public Coarsener {
public:
    explicit HeavyEdgeMatching(const Configuration& config);
    
    CoarseningStats coarsen(HypergraphHierarchy& hierarchy,
                           uint32_t level_idx,
                           Profiler* profiler = nullptr) override;
    
    const char* getName() const override { return "HeavyEdgeMatching"; }

private:
    /**
     * @brief Node matching candidate
     */
    struct MatchCandidate {
        NodeID node1;
        NodeID node2;
        double rating;
        
        MatchCandidate() 
            : node1(INVALID_NODE), node2(INVALID_NODE), rating(0.0) {}
        
        MatchCandidate(NodeID n1, NodeID n2, double r)
            : node1(n1), node2(n2), rating(r) {}
        
        bool operator<(const MatchCandidate& other) const {
            return rating > other.rating;  // Higher rating first
        }
    };
    
    /**
     * @brief Compute matching using heavy edge strategy
     * @param hg Hypergraph
     * @param matching Output: node -> matched_node mapping
     * @param profiler Optional profiler for performance analysis
     * @return Number of matched pairs
     */
    uint32_t computeMatching(const Hypergraph& hg,
                            std::vector<NodeID>& matching,
                            Profiler* profiler = nullptr);
    
    /**
     * @brief Compute rating between two nodes
     * Higher rating = better match candidate
     * @param hg Hypergraph
     * @param node1 First node
     * @param node2 Second node
     * @return Rating value
     */
    double computeRating(const Hypergraph& hg, NodeID node1, NodeID node2);
    
    /**
     * @brief Build coarse hypergraph from matching
     * @param fine_hg Fine hypergraph
     * @param matching Node matching
     * @param coarse_hg Output: coarse hypergraph
     * @param node_mapping Output: fine node -> coarse node
     * @param num_coarse_nodes Output: number of coarse nodes
     * @param profiler Optional profiler for performance analysis
     */
    void buildCoarseGraph(const Hypergraph& fine_hg,
                         const std::vector<NodeID>& matching,
                         Hypergraph& coarse_hg,
                         std::vector<NodeID>& node_mapping,
                         NodeID& num_coarse_nodes,
                         Profiler* profiler = nullptr);
    
    /**
     * @brief Check if two nodes share nets (are neighbors)
     * @param hg Hypergraph
     * @param node1 First node
     * @param node2 Second node
     * @return True if share at least one net
     */
    bool areNeighbors(const Hypergraph& hg, NodeID node1, NodeID node2) const;
    
    /**
     * @brief Count common nets between two nodes
     * @param hg Hypergraph
     * @param node1 First node
     * @param node2 Second node
     * @return Number of common nets
     */
    Index countCommonNets(const Hypergraph& hg, NodeID node1, NodeID node2) const;
};

/**
 * @brief First Choice coarsening (faster, less quality)
 * 
 * Algorithm:
 * - Visit nodes in random order
 * - Match with first unmatched neighbor
 * - Very fast O(|V|) expected time
 */
class FirstChoiceMatching : public Coarsener {
public:
    explicit FirstChoiceMatching(const Configuration& config);
    
    CoarseningStats coarsen(HypergraphHierarchy& hierarchy,
                           uint32_t level_idx,
                           Profiler* profiler = nullptr) override;
    
    const char* getName() const override { return "FirstChoiceMatching"; }

private:
    uint32_t computeMatching(const Hypergraph& hg,
                            std::vector<NodeID>& matching);
    
    /**
     * @brief Build coarse nets with parallel net merging and duplicate pin removal
     * @param fine_hg Fine hypergraph
     * @param node_mapping Mapping from fine to coarse nodes
     * @param coarse_hg Coarse hypergraph (output)
     */
    void buildCoarseNets(const Hypergraph& fine_hg,
                        const std::vector<NodeID>& node_mapping,
                        Hypergraph& coarse_hg);
};

/**
 * @brief Cluster Matching coarsening (aggressive clustering)
 * 
 * Algorithm:
 * - Allow clustering of 2+ nodes into a single coarse node
 * - When visiting node a, if best adj node u is already matched:
 *   - Check if a can join u's cluster via adjMatchAreaJudging
 *   - If yes, add a to cluster; if no, try next best neighbor
 * - Stop when all nodes matched or contraction ratio > 3.0
 */
class ClusterMatching : public Coarsener {
public:
    explicit ClusterMatching(const Configuration& config);
    
    CoarseningStats coarsen(HypergraphHierarchy& hierarchy,
                           uint32_t level_idx,
                           Profiler* profiler = nullptr) override;
    
    const char* getName() const override { return "ClusterMatching"; }

private:
    
    bool adjMatchAreaJudging(const Hypergraph& hg, NodeID node1, NodeID node2, Weight max_weight) const;
    
    /**
     * @brief Compute clustering (multi-node matching)
     * @param hg Hypergraph
     * @param cluster_id Output: node -> cluster_id mapping
     * @return Number of clusters formed
     */
    NodeID computeClustering(const Hypergraph& hg,
                             std::vector<NodeID>& cluster_id);
    
    /**
     * @brief Compute clustering V2 - considers accumulated cluster contribution
     * 
     * Unlike V1, this version accumulates connection contributions for nodes
     * that are already in the same cluster. For example:
     * - Node a has neighbors b, c, d
     * - b is unclustered, c and d are in cluster C
     * - a-b contribution = 1.5, a-c = 1, a-d = 1
     * - V1 would match a with b (1.5 > 1)
     * - V2 calculates cluster C contribution = 1 + 1 = 2, so a merges into C
     * 
     * @param hg Hypergraph
     * @param cluster_id Output: node -> cluster_id mapping
     * @return Number of clusters formed
     */
    NodeID computeClusteringV2(const Hypergraph& hg,
                               std::vector<NodeID>& cluster_id);
    
    /**
     * @brief Judge if a node can be added to an existing cluster
     * @param hg Hypergraph
     * @param cluster_size Current size of the cluster
     * @param cluster_weight Current total weight of the cluster
     * @param node_weight Weight of node to add
     * @return True if node can join the cluster
     */
    bool adjMatchAreaJudging(const Hypergraph& hg,
                             size_t cluster_size,
                             Weight cluster_weight,
                             Weight node_weight) const;
    
    /**
     * @brief Build coarse nets with parallel net merging and duplicate pin removal
     * @param fine_hg Fine hypergraph
     * @param node_mapping Mapping from fine to coarse nodes
     * @param coarse_hg Coarse hypergraph (output)
     */
    void buildCoarseNets(const Hypergraph& fine_hg,
                        const std::vector<NodeID>& node_mapping,
                        Hypergraph& coarse_hg);
};

} // namespace consmlp

#endif // ZXPART_COARSENER_H

