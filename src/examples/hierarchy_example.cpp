#include "datastructures/Hypergraph.h"
#include "datastructures/HypergraphHierarchy.h"
#include <iostream>

using namespace consmlp;

/**
 * @brief Example: Create a hypergraph hierarchy with coarsening
 * 
 * This demonstrates how to maintain node mappings across levels:
 * - Level 0 (finest): Original hypergraph with 8 nodes
 * - Level 1 (coarser): Nodes merged: (0,1)->0, (2,3)->1, (4,5)->2, (6,7)->3
 * - Level 2 (coarsest): Nodes merged: (0,1)->0, (2,3)->1
 */
void exampleHierarchy() {
    std::cout << "Example: Hypergraph Hierarchy with Node Mappings\n" << std::endl;
    
    // ========== Level 0: Create finest hypergraph ==========
    std::cout << "Creating finest level (Level 0) with 8 nodes..." << std::endl;
    Hypergraph finest_hg(8, 4);
    
    // Add nodes
    for (int i = 0; i < 8; ++i) {
        finest_hg.addNode(NodeType::LUT, 1);
    }
    
    // Add nets
    EdgeID e0 = finest_hg.addNet(1, false);
    EdgeID e1 = finest_hg.addNet(1, false);
    EdgeID e2 = finest_hg.addNet(1, false);
    EdgeID e3 = finest_hg.addNet(1, false);
    
    // Create connections
    finest_hg.addNodeToNet(e0, 0);
    finest_hg.addNodeToNet(e0, 1);
    finest_hg.addNodeToNet(e0, 2);
    
    finest_hg.addNodeToNet(e1, 2);
    finest_hg.addNodeToNet(e1, 3);
    finest_hg.addNodeToNet(e1, 4);
    
    finest_hg.addNodeToNet(e2, 4);
    finest_hg.addNodeToNet(e2, 5);
    finest_hg.addNodeToNet(e2, 6);
    
    finest_hg.addNodeToNet(e3, 6);
    finest_hg.addNodeToNet(e3, 7);
    finest_hg.addNodeToNet(e3, 0);
    
    finest_hg.finalize();
    
    std::cout << "  Level 0: " << finest_hg.getNumNodes() << " nodes, "
              << finest_hg.getNumNets() << " nets\n" << std::endl;
    
    // ========== Create hierarchy ==========
    HypergraphHierarchy hierarchy(std::move(finest_hg));
    
    // ========== Level 1: First coarsening ==========
    std::cout << "Creating Level 1 (first coarsening)..." << std::endl;
    std::cout << "  Merging: (0,1)->0, (2,3)->1, (4,5)->2, (6,7)->3\n" << std::endl;
    
    HypergraphLevel& level1 = hierarchy.addCoarserLevel();
    Hypergraph& coarse_hg1 = level1.getHypergraph();
    
    // Create 4 coarse nodes (each represents 2 fine nodes)
    for (int i = 0; i < 4; ++i) {
        coarse_hg1.addNode(NodeType::LUT, 2);  // Weight = 2 (sum of merged nodes)
    }
    
    // Set up mappings: Level 0 -> Level 1
    HypergraphLevel& level0 = hierarchy.getLevel(0);
    level0.setCoarserNode(0, 0);  // Node 0 in L0 -> Node 0 in L1
    level0.setCoarserNode(1, 0);  // Node 1 in L0 -> Node 0 in L1
    level0.setCoarserNode(2, 1);
    level0.setCoarserNode(3, 1);
    level0.setCoarserNode(4, 2);
    level0.setCoarserNode(5, 2);
    level0.setCoarserNode(6, 3);
    level0.setCoarserNode(7, 3);
    
    // Set up reverse mappings: Level 1 -> Level 0
    level0.addFinerNode(0, 0);
    level0.addFinerNode(0, 1);
    level0.addFinerNode(1, 2);
    level0.addFinerNode(1, 3);
    level0.addFinerNode(2, 4);
    level0.addFinerNode(2, 5);
    level0.addFinerNode(3, 6);
    level0.addFinerNode(3, 7);
    level0.finalizeMapping();
    
    // Add nets to coarse level (simplified for example)
    EdgeID ce0 = coarse_hg1.addNet(1, false);
    EdgeID ce1 = coarse_hg1.addNet(1, false);
    coarse_hg1.addNodeToNet(ce0, 0);
    coarse_hg1.addNodeToNet(ce0, 1);
    coarse_hg1.addNodeToNet(ce1, 2);
    coarse_hg1.addNodeToNet(ce1, 3);
    coarse_hg1.finalize();
    
    std::cout << "  Level 1: " << coarse_hg1.getNumNodes() << " nodes, "
              << coarse_hg1.getNumNets() << " nets\n" << std::endl;
    
    // ========== Level 2: Second coarsening ==========
    std::cout << "Creating Level 2 (second coarsening)..." << std::endl;
    std::cout << "  Merging: (0,1)->0, (2,3)->1\n" << std::endl;
    
    HypergraphLevel& level2 = hierarchy.addCoarserLevel();
    Hypergraph& coarse_hg2 = level2.getHypergraph();
    
    // Create 2 coarse nodes
    for (int i = 0; i < 2; ++i) {
        coarse_hg2.addNode(NodeType::LUT, 4);  // Weight = 4
    }
    
    // Set up mappings: Level 1 -> Level 2
    level1.setCoarserNode(0, 0);
    level1.setCoarserNode(1, 0);
    level1.setCoarserNode(2, 1);
    level1.setCoarserNode(3, 1);
    
    // Set up reverse mappings
    level1.addFinerNode(0, 0);
    level1.addFinerNode(0, 1);
    level1.addFinerNode(1, 2);
    level1.addFinerNode(1, 3);
    level1.finalizeMapping();
    
    EdgeID ce2 = coarse_hg2.addNet(2, false);
    coarse_hg2.addNodeToNet(ce2, 0);
    coarse_hg2.addNodeToNet(ce2, 1);
    coarse_hg2.finalize();
    
    std::cout << "  Level 2: " << coarse_hg2.getNumNodes() << " nodes, "
              << coarse_hg2.getNumNets() << " nets\n" << std::endl;
    
    // ========== Test mappings ==========
    std::cout << "========== Testing Node Mappings ==========\n" << std::endl;
    
    // Test forward mapping: finest -> coarser levels
    std::cout << "Forward mapping (finest to coarser):" << std::endl;
    for (NodeID node = 0; node < 8; ++node) {
        NodeID node_l1 = hierarchy.mapNodeToLevel(node, 1);
        NodeID node_l2 = hierarchy.mapNodeToLevel(node, 2);
        std::cout << "  Node " << node << " (L0) -> " 
                  << node_l1 << " (L1) -> " 
                  << node_l2 << " (L2)" << std::endl;
    }
    
    std::cout << "\nReverse mapping (coarser to finest):" << std::endl;
    
    // Test reverse mapping from Level 2
    std::cout << "From Level 2:" << std::endl;
    for (NodeID node = 0; node < 2; ++node) {
        auto finest_nodes = hierarchy.mapNodeToFinestLevel(node, 2);
        std::cout << "  Node " << node << " (L2) -> {";
        for (size_t i = 0; i < finest_nodes.size(); ++i) {
            std::cout << finest_nodes[i];
            if (i < finest_nodes.size() - 1) std::cout << ", ";
        }
        std::cout << "} (L0)" << std::endl;
    }
    
    // Test reverse mapping from Level 1
    std::cout << "\nFrom Level 1:" << std::endl;
    for (NodeID node = 0; node < 4; ++node) {
        auto finest_nodes = hierarchy.mapNodeToFinestLevel(node, 1);
        std::cout << "  Node " << node << " (L1) -> {";
        for (size_t i = 0; i < finest_nodes.size(); ++i) {
            std::cout << finest_nodes[i];
            if (i < finest_nodes.size() - 1) std::cout << ", ";
        }
        std::cout << "} (L0)" << std::endl;
    }
    
    std::cout << "\n========================================" << std::endl;
}

int main() {
    std::cout << "ConsMLP - Hypergraph Hierarchy Example" << std::endl;
    std::cout << "======================================\n" << std::endl;
    
    exampleHierarchy();
    
    return 0;
}

