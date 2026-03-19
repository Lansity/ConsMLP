#ifndef ZXPART_HGR_PARSER_H
#define ZXPART_HGR_PARSER_H

#include "Types.h"
#include "datastructures/Hypergraph.h"
#include <string>
#include <fstream>
#include <sstream>
#include <vector>

namespace consmlp {

/**
 * @brief Parser for hMetis .hgr hypergraph format
 * 
 * Supports two format specifications:
 * 
 * Standard format:
 * - First line: num_nets num_nodes [format_flag]
 * - format_flag (optional, default=0):
 *   - 0: no weights
 *   - 1: hyperedge weights
 *   - 10: vertex weights
 *   - 11: both hyperedge and vertex weights
 * - Next num_nets lines: hyperedge definitions
 *   - If format_flag & 1: [weight] node1 node2 ...
 *   - Otherwise: node1 node2 ...
 * - Next num_nodes lines (if format_flag & 10): vertex weights
 * 
 * Extended format (optimized):
 * - First line: mode num_nodes num_nets total_pins
 * - mode:
 *   - 1: no weights
 *   - 2: node weights (after net lines)
 *   - 3: net weights (first number on each net line)
 *   - 4: both net and node weights
 * - total_pins: total number of pins (enables optimized parsing)
 * 
 * Note: Node IDs in .hgr files are 1-indexed
 */
class HgrParser {
public:
    /**
     * @brief Parse .hgr file and build hypergraph
     * @param filename Path to .hgr file
     * @return Parsed hypergraph
     */
    static Hypergraph parse(const std::string& filename);
    
    /**
     * @brief Parse .hgr file with type file and build hypergraph
     * @param hgr_filename Path to .hgr file
     * @param type_filename Path to type file (one type per line, matching node order)
     * @return Parsed hypergraph with node types
     */
    static Hypergraph parseWithTypes(const std::string& hgr_filename,
                                     const std::string& type_filename);
    
    /**
     * @brief Parse type file
     * @param filename Path to type file
     * @param num_nodes Expected number of nodes
     * @return Vector of node types
     */
    static std::vector<NodeType> parseTypeFile(const std::string& filename,
                                               NodeID num_nodes);
    
    /**
     * @brief Write hypergraph to .hgr file
     * @param hg Hypergraph to write
     * @param filename Output file path
     * @param write_weights Whether to write weights
     */
    static void write(const Hypergraph& hg, const std::string& filename,
                     bool write_weights = true);

private:
    /**
     * @brief Parse header line (standard format)
     * @param line Header line string
     * @param num_nets Output: number of nets
     * @param num_nodes Output: number of nodes
     * @param format_flag Output: format flag
     */
    static void parseHeader(const std::string& line, 
                           EdgeID& num_nets,
                           NodeID& num_nodes,
                           int& format_flag);
    
    /**
     * @brief Parse header line (extended format)
     * @param line Header line string
     * @param mode Output: format mode (1-4)
     * @param num_nodes Output: number of nodes
     * @param num_nets Output: number of nets
     * @param total_pins Output: total number of pins
     */
    static void parseHeaderExtended(const std::string& line,
                                   int& mode,
                                   NodeID& num_nodes,
                                   EdgeID& num_nets,
                                   size_t& total_pins);
    
    /**
     * @brief Optimized parser using two-pass CSR construction
     * @param file Input file stream (already positioned after header)
     * @param mode Format mode
     * @param num_nodes Number of nodes
     * @param num_nets Number of nets
     * @param total_pins Total number of pins
     * @param filename Filename (for logging)
     * @return Parsed hypergraph with direct CSR structure
     */
    static Hypergraph parseOptimized(std::ifstream& file, int mode,
                                    NodeID num_nodes, EdgeID num_nets,
                                    size_t total_pins,
                                    const std::string& filename);
    
    /**
     * @brief Parse net definition line
     * @param line Line string
     * @param has_net_weights Whether nets have weights
     * @param net_weight Output: net weight
     * @param nodes Output: connected nodes (0-indexed)
     */
    static void parseNet(const std::string& line,
                        bool has_net_weights,
                        Weight& net_weight,
                        std::vector<NodeID>& nodes);
    
    /**
     * @brief Parse node weight line
     * @param line Line string
     * @return Node weight
     */
    static Weight parseNodeWeight(const std::string& line);
    
    /**
     * @brief Trim whitespace from string
     */
    static std::string trim(const std::string& str);
};

} // namespace consmlp

#endif // ZXPART_HGR_PARSER_H

