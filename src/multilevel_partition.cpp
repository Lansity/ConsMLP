#include "partitioning/MultilevelPartitioner.h"
#include <iostream>

using namespace consmlp;

int main(int argc, char* argv[]) {
    std::cout << "ConsMLP - Multilevel Hypergraph Partitioner" << std::endl;
    std::cout << "==========================================\n" << std::endl;
    
    MultilevelPartitionerApp app;
    
    if (!app.parseArguments(argc, argv)) {
        printUsage(argv[0]);
        return 1;
    }
    
    return app.run();
}
