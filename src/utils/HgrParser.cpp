#include "utils/HgrParser.h"
#include <iostream>
#include <stdexcept>
#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace consmlp {

// Fast integer parsing using strtol
inline NodeID fastParseInt(const char*& ptr) {
    while (*ptr == ' ' || *ptr == '\t') ++ptr;
    if (*ptr == '\0' || *ptr == '\n' || *ptr == '\r') return 0;
    
    char* end;
    long val = std::strtol(ptr, &end, 10);
    if (end == ptr) return 0;
    ptr = end;
    return static_cast<NodeID>(val);
}

// Fast line parsing for nets - avoid istringstream overhead
inline void fastParseNetLine(const char* line, bool has_net_weights,
                             Weight& net_weight, std::vector<NodeID>& nodes) {
    nodes.clear();
    net_weight = 1;
    
    const char* ptr = line;
    
    // Skip leading whitespace
    while (*ptr == ' ' || *ptr == '\t') ++ptr;
    if (*ptr == '\0' || *ptr == '\n' || *ptr == '\r') return;
    
    if (has_net_weights) {
        char* end;
        net_weight = static_cast<Weight>(std::strtol(ptr, &end, 10));
        ptr = end;
    }
    
    // Parse node IDs
    while (*ptr) {
        while (*ptr == ' ' || *ptr == '\t') ++ptr;
        if (*ptr == '\0' || *ptr == '\n' || *ptr == '\r') break;
        
        char* end;
        long node_id = std::strtol(ptr, &end, 10);
        if (end == ptr) break;
        ptr = end;
        
        if (node_id > 0) {
            nodes.push_back(static_cast<NodeID>(node_id - 1));  // Convert to 0-indexed
        }
    }
}

Hypergraph HgrParser::parse(const std::string& filename) {

    std::ifstream file(filename);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open file: " + filename);
    }
    
    // Set larger buffer for faster I/O
    constexpr size_t kBufferSize = 1 << 20;  // 1MB buffer
    std::vector<char> buffer(kBufferSize);
    file.rdbuf()->pubsetbuf(buffer.data(), kBufferSize);
    
    std::string line;
    line.reserve(4096);  // Pre-allocate line buffer
    
    // Parse header - try extended format first
    if (!std::getline(file, line)) {
        throw std::runtime_error("Failed to read header");
    }
    
    // Check if this is the new extended format (mode num_nodes num_nets total_pins)
    int mode = 0;
    NodeID num_nodes = 0;
    EdgeID num_nets = 0;
    size_t total_pins = 0;
    
    // Count numbers in header to detect format
    const char* ptr = line.c_str();
    int num_count = 0;
    while (*ptr) {
        while (*ptr == ' ' || *ptr == '\t') ++ptr;
        if (*ptr >= '0' && *ptr <= '9') {
            num_count++;
            while (*ptr >= '0' && *ptr <= '9') ++ptr;
        } else if (*ptr != '\0' && *ptr != '\n' && *ptr != '\r') {
            ++ptr;
        } else {
            break;
        }
    }
    
    bool is_extended_format = (num_count == 4);
    bool has_net_weights = false;
    bool has_node_weights = false;
    
    if (is_extended_format) {
        // New extended format: mode num_nodes num_nets total_pins
        parseHeaderExtended(line, mode, num_nodes, num_nets, total_pins);
        has_net_weights = (mode == 3 || mode == 4);
        has_node_weights = (mode == 2 || mode == 4);
    } else {
        // Old format: num_nets num_nodes [format_flag]
        int format_flag = 0;
        parseHeader(line, num_nets, num_nodes, format_flag);
        has_net_weights = (format_flag % 10) == 1;
        has_node_weights = (format_flag / 10) == 1;
    }
    
    // Use optimized path if total_pins is known
    if (is_extended_format && total_pins > 0) {
        return parseOptimized(file, mode, num_nodes, num_nets, total_pins, filename);
    }
    
    // Create hypergraph
    Hypergraph hg(num_nodes, num_nets);
    
    // Add nodes first (will set weights later if needed)
    for (NodeID i = 0; i < num_nodes; ++i) {
        hg.addNode(NodeType::LUT, 1);  // Default type and weight
    }
    
    // Reusable buffer for node IDs
    std::vector<NodeID> nodes_buf;
    nodes_buf.reserve(256);

    // Parse nets
    constexpr size_t kMaxNetSize = 100000;  // Skip nets larger than this threshold
    EdgeID skipped_nets = 0;
    
    for (EdgeID net_id = 0; net_id < num_nets; ++net_id) {
        if (!std::getline(file, line)) {
            throw std::runtime_error("Unexpected end of file while reading nets");
        }
        
        Weight net_weight = 1;
        fastParseNetLine(line.c_str(), has_net_weights, net_weight, nodes_buf);
        
        // Skip overly large nets to avoid runtime issues
        if (nodes_buf.size() > kMaxNetSize) {
            skipped_nets++;
            continue;
        }
        
        // Add net to hypergraph
        EdgeID added_net = hg.addNet(net_weight, false);
        
        // Add connections
        for (NodeID node_id : nodes_buf) {
            if (node_id < num_nodes) {
                hg.addNodeToNet(added_net, node_id);
            }
        }
    }
    
    // Parse node weights if present
    if (has_node_weights) {
        for (NodeID node_id = 0; node_id < num_nodes; ++node_id) {
            if (!std::getline(file, line)) {
                break;
            }
            
            const char* ptr = line.c_str();
            while (*ptr == ' ' || *ptr == '\t') ++ptr;
            if (*ptr == '\0' || *ptr == '\n' || *ptr == '\r') continue;
            
            char* end;
            Weight weight = static_cast<Weight>(std::strtol(ptr, &end, 10));
            if (weight > 0) {
                hg.setNodeAttributes(node_id, NodeType::LUT, weight, false, false);
            }
        }
    }
    
    // Finalize hypergraph
    hg.finalize();
    
    file.close();
    return hg;
}

Hypergraph HgrParser::parseWithTypes(const std::string& hgr_filename,
                                     const std::string& type_filename) {
    // Constants for ignored net detection
    constexpr double kIgnoredNetRatio = 0.5;  // Net size > 50% of nodes
    constexpr size_t kIgnoredNetMaxSize = 10000;  // Or net size > 10000
    
    std::ifstream file(hgr_filename);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open file: " + hgr_filename);
    }
    
    // Set larger buffer for faster I/O
    constexpr size_t kBufferSize = 1 << 20;  // 1MB buffer
    std::vector<char> buffer(kBufferSize);
    file.rdbuf()->pubsetbuf(buffer.data(), kBufferSize);
    
    std::string line;
    line.reserve(4096);  // Pre-allocate line buffer
    
    EdgeID num_nets = 0;
    NodeID num_nodes = 0;
    int format_flag = 0;
    
    // Parse header
    if (!std::getline(file, line)) {
        throw std::runtime_error("Failed to read header");
    }
    parseHeader(line, num_nets, num_nodes, format_flag);
    
    bool has_net_weights = (format_flag % 10) == 1;
    bool has_node_weights = (format_flag / 10) == 1;
    
    // Calculate ignored net threshold
    size_t ignored_threshold = std::min(
        static_cast<size_t>(num_nodes * kIgnoredNetRatio),
        kIgnoredNetMaxSize
    );
    
    // Parse type file first
    std::vector<NodeType> types = parseTypeFile(type_filename, num_nodes);
    
    // Create hypergraph (estimate fewer nets due to filtering)
    Hypergraph hg(num_nodes, num_nets);
    
    // Add nodes with types
    for (NodeID i = 0; i < num_nodes; ++i) {
        hg.addNode(types[i], 1);  // Use parsed type, default weight
    }
    
    // Reusable buffer for node IDs
    std::vector<NodeID> nodes_buf;
    nodes_buf.reserve(1024);
    
    // Parse and filter nets
    EdgeID ignored_nets = 0;
    
    for (EdgeID net_id = 0; net_id < num_nets; ++net_id) {
        if (!std::getline(file, line)) {
            throw std::runtime_error("Unexpected end of file while reading nets");
        }
        
        Weight net_weight = 1;
        fastParseNetLine(line.c_str(), has_net_weights, net_weight, nodes_buf);
        
        // Check if this net should be ignored (too large)
        constexpr size_t kMaxNetSize = 100000;  // Hard limit to avoid runtime issues
        size_t skip_threshold = std::min(ignored_threshold, kMaxNetSize);
        if (nodes_buf.size() > skip_threshold) {
            ignored_nets++;
            continue;  // Skip this net
        }
        
        // Add net to hypergraph
        EdgeID added_net = hg.addNet(net_weight, false);
        
        // Add connections
        for (NodeID node_id : nodes_buf) {
            if (node_id < num_nodes) {
                hg.addNodeToNet(added_net, node_id);
            }
        }
    }
    
    // Parse node weights if present
    if (has_node_weights) {
        for (NodeID node_id = 0; node_id < num_nodes; ++node_id) {
            if (!std::getline(file, line)) {
                break;
            }
            
            const char* ptr = line.c_str();
            while (*ptr == ' ' || *ptr == '\t') ++ptr;
            if (*ptr == '\0' || *ptr == '\n' || *ptr == '\r') continue;
            
            char* end;
            Weight weight = static_cast<Weight>(std::strtol(ptr, &end, 10));
            if (weight > 0) {
                hg.setNodeAttributes(node_id, types[node_id], weight, false, false);
            }
        }
    }
    
    // Finalize hypergraph
    hg.finalize();
    
    file.close();
    return hg;
}

std::vector<NodeType> HgrParser::parseTypeFile(const std::string& filename,
                                               NodeID num_nodes) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open type file: " + filename);
    }
    
    // Set larger buffer for faster I/O
    constexpr size_t kBufferSize = 1 << 18;  // 256KB buffer
    std::vector<char> buffer(kBufferSize);
    file.rdbuf()->pubsetbuf(buffer.data(), kBufferSize);
    
    std::vector<NodeType> types;
    types.reserve(num_nodes);
    
    std::string line;
    line.reserve(64);
    
    while (std::getline(file, line) && types.size() < num_nodes) {
        // Fast trim - find first non-whitespace
        const char* start = line.c_str();
        while (*start == ' ' || *start == '\t') ++start;
        if (*start == '\0' || *start == '\n' || *start == '\r') continue;
        
        // Find end of token
        const char* end = start;
        while (*end && *end != ' ' && *end != '\t' && *end != '\n' && *end != '\r') ++end;
        
        // Fast type matching using first character
        NodeType type = NodeType::OTHER;
        size_t len = end - start;
        
        if (len >= 2) {
            char c0 = start[0];
            char c1 = start[1];
            if (c0 == 'L' && len == 3) type = NodeType::LUT;           // LUT
            else if (c0 == 'F' && c1 == 'F') type = NodeType::FF;     // FF
            else if (c0 == 'M' && c1 == 'U') type = NodeType::MUX;    // MUX
            else if (c0 == 'C' && c1 == 'A') type = NodeType::CARRY;  // CARRY
            else if (c0 == 'B' && c1 == 'R') type = NodeType::BRAM;   // BRAM
            else if (c0 == 'I' && c1 == 'O') type = NodeType::IO;     // IO
            else if (c0 == 'D' && c1 == 'S') type = NodeType::DSP;    // DSP
            else if (c0 == 'O') type = NodeType::OTHER;               // OTHER
        }
        
        types.push_back(type);
    }
    
    // Fill remaining with default type if file is shorter
    while (types.size() < num_nodes) {
        types.push_back(NodeType::LUT);
    }
    
    file.close();
    return types;
}

void HgrParser::parseHeader(const std::string& line,
                            EdgeID& num_nets,
                            NodeID& num_nodes,
                            int& format_flag) {
    std::istringstream iss(line);
    
    if (!(iss >> num_nets >> num_nodes)) {
        throw std::runtime_error("Invalid header format");
    }
    
    // Format flag is optional
    if (!(iss >> format_flag)) {
        format_flag = 0;
    }
}

// New extended format parser: mode num_nodes num_nets total_pins
void HgrParser::parseHeaderExtended(const std::string& line,
                                    int& mode,
                                    NodeID& num_nodes,
                                    EdgeID& num_nets,
                                    size_t& total_pins) {
    const char* ptr = line.c_str();
    
    // Skip leading whitespace
    while (*ptr == ' ' || *ptr == '\t') ++ptr;
    
    // Parse mode
    char* end;
    mode = static_cast<int>(std::strtol(ptr, &end, 10));
    if (end == ptr) {
        throw std::runtime_error("Invalid header format: cannot parse mode");
    }
    ptr = end;
    
    // Parse num_nodes
    while (*ptr == ' ' || *ptr == '\t') ++ptr;
    num_nodes = static_cast<NodeID>(std::strtol(ptr, &end, 10));
    if (end == ptr) {
        throw std::runtime_error("Invalid header format: cannot parse num_nodes");
    }
    ptr = end;
    
    // Parse num_nets
    while (*ptr == ' ' || *ptr == '\t') ++ptr;
    num_nets = static_cast<EdgeID>(std::strtol(ptr, &end, 10));
    if (end == ptr) {
        throw std::runtime_error("Invalid header format: cannot parse num_nets");
    }
    ptr = end;
    
    // Parse total_pins (optional, defaults to 0)
    while (*ptr == ' ' || *ptr == '\t') ++ptr;
    if (*ptr != '\0' && *ptr != '\n' && *ptr != '\r') {
        total_pins = static_cast<size_t>(std::strtol(ptr, &end, 10));
    } else {
        total_pins = 0;  // Unknown, use old path
    }
}

void HgrParser::parseNet(const std::string& line,
                         bool has_net_weights,
                         Weight& net_weight,
                         std::vector<NodeID>& nodes) {
    nodes.clear();
    net_weight = 1;
    
    std::string trimmed = trim(line);
    if (trimmed.empty()) {
        return;  // Empty net
    }
    
    std::istringstream iss(trimmed);
    
    // Read first value
    int first_value;
    if (!(iss >> first_value)) {
        return;  // Empty line
    }
    
    if (has_net_weights) {
        net_weight = first_value;
        
        // Read node IDs (convert from 1-indexed to 0-indexed)
        NodeID node_id;
        while (iss >> node_id) {
            if (node_id > 0) {
                nodes.push_back(node_id - 1);  // Convert to 0-indexed
            }
        }
    } else {
        // First value is a node ID
        if (first_value > 0) {
            nodes.push_back(first_value - 1);
        }
        
        NodeID node_id;
        while (iss >> node_id) {
            if (node_id > 0) {
                nodes.push_back(node_id - 1);
            }
        }
    }
}

Weight HgrParser::parseNodeWeight(const std::string& line) {
    std::string trimmed = trim(line);
    if (trimmed.empty()) {
        return 1;
    }
    
    std::istringstream iss(trimmed);
    Weight weight;
    if (iss >> weight) {
        return weight;
    }
    
    return 1;
}

std::string HgrParser::trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, last - first + 1);
}

void HgrParser::write(const Hypergraph& hg, const std::string& filename,
                     bool write_weights) {
    std::ofstream file(filename);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open file for writing: " + filename);
    }
    
    // Determine format flag
    int format_flag = 0;
    if (write_weights) {
        format_flag = 11;  // Both net and node weights
    }
    
    // Write header
    file << hg.getNumNets() << " " << hg.getNumNodes() << " " 
         << format_flag << std::endl;
    
    // Write nets
    for (EdgeID net_id = 0; net_id < hg.getNumNets(); ++net_id) {
        if (write_weights) {
            file << hg.getNetWeight(net_id) << " ";
        }
        
        auto nodes = hg.getNetNodes(net_id);
        for (const NodeID* it = nodes.first; it != nodes.second; ++it) {
            file << (*it + 1) << " ";  // Convert to 1-indexed
        }
        file << std::endl;
    }
    
    // Write node weights
    if (write_weights) {
        for (NodeID node_id = 0; node_id < hg.getNumNodes(); ++node_id) {
            file << hg.getNodeWeight(node_id) << std::endl;
        }
    }
    
    file.close();
}

// Optimized parser using two-pass CSR construction when total_pins is known
Hypergraph HgrParser::parseOptimized(std::ifstream& file, int mode,
                                     NodeID num_nodes, EdgeID num_nets,
                                     size_t total_pins,
                                     const std::string& filename) {
    
    bool has_net_weights = (mode == 3 || mode == 4);
    bool has_node_weights = (mode == 2 || mode == 4);
    
    // Allocate buffers for first pass
    std::vector<Index> net_sizes(num_nets);
    std::vector<Index> node_degrees(num_nodes, 0);
    std::vector<Weight> net_weights_vec(num_nets, 1);
    std::vector<Weight> node_weights_vec(num_nodes, 1);
    
    std::string line;
    line.reserve(4096);
    std::vector<NodeID> nodes_buf;
    nodes_buf.reserve(256);
    
    // ========== FIRST PASS: Collect net sizes and node degrees ==========
    constexpr size_t kMaxNetSize = 100000;  // Skip nets larger than this threshold
    size_t actual_pins = 0;
    EdgeID skipped_nets = 0;
    
    for (EdgeID net_id = 0; net_id < num_nets; ++net_id) {
        if (!std::getline(file, line)) {
            throw std::runtime_error("Unexpected end of file while reading nets (first pass)");
        }
        
        Weight net_weight = 1;
        fastParseNetLine(line.c_str(), has_net_weights, net_weight, nodes_buf);
        
        // Skip overly large nets to avoid runtime issues
        if (nodes_buf.size() > kMaxNetSize) {
            skipped_nets++;
            net_weights_vec[net_id] = net_weight;
            net_sizes[net_id] = 0;  // Mark as skipped
            continue;
        }
        
        net_weights_vec[net_id] = net_weight;
        net_sizes[net_id] = nodes_buf.size();
        actual_pins += nodes_buf.size();
        
        // Update node degrees
        for (NodeID node : nodes_buf) {
            if (node < num_nodes) {
                node_degrees[node]++;
            }
        }
    }
    
    
    // Parse node weights if present
    if (has_node_weights) {
        for (NodeID node_id = 0; node_id < num_nodes; ++node_id) {
            if (!std::getline(file, line)) {
                break;
            }
            
            const char* ptr = line.c_str();
            while (*ptr == ' ' || *ptr == '\t') ++ptr;
            if (*ptr == '\0' || *ptr == '\n' || *ptr == '\r') continue;
            
            char* end;
            Weight weight = static_cast<Weight>(std::strtol(ptr, &end, 10));
            if (weight > 0) {
                node_weights_vec[node_id] = weight;
            }
        }
    }
    
    // ========== BUILD CSR STRUCTURE DIRECTLY ==========
    Hypergraph hg;
    hg.num_nodes_ = num_nodes;
    hg.num_nets_ = num_nets;
    hg.finalized_ = true;  // Mark as finalized since we're building CSR directly
    
    // Allocate node attributes with per-type weights
    hg.node_type_weights_.resize(num_nodes);
    for (NodeID i = 0; i < num_nodes; ++i) {
        // Initialize with all weights in LUT type (will be updated by parseTypesFile)
        TypeWeights tw = {};
        tw[static_cast<size_t>(NodeType::LUT)] = node_weights_vec[i];
        hg.node_type_weights_[i] = tw;
    }
    hg.node_primary_types_.resize(num_nodes, NodeType::LUT);
    hg.node_weights_ = std::move(node_weights_vec);
    hg.node_ignored_.resize(num_nodes, false);
    hg.node_fixed_.resize(num_nodes, false);
    
    // Allocate net attributes
    hg.net_weights_ = std::move(net_weights_vec);
    hg.net_global_.resize(num_nets, false);
    
    // Build Net -> Nodes CSR structure
    hg.net_offsets_.resize(num_nets + 1);
    hg.net_offsets_[0] = 0;
    for (EdgeID i = 0; i < num_nets; ++i) {
        hg.net_offsets_[i + 1] = hg.net_offsets_[i] + net_sizes[i];
    }
    hg.net_nodes_.resize(actual_pins);  // Exact size!
    
    // Build Node -> Nets CSR structure
    hg.node_offsets_.resize(num_nodes + 1);
    hg.node_offsets_[0] = 0;
    for (NodeID i = 0; i < num_nodes; ++i) {
        hg.node_offsets_[i + 1] = hg.node_offsets_[i] + node_degrees[i];
    }
    hg.node_nets_.resize(actual_pins);  // Exact size!
    
    // ========== SECOND PASS: Fill CSR arrays ==========
    // Reset file to beginning of net data
    file.clear();
    file.seekg(0);
    
    // Skip header
    std::getline(file, line);
    
    // Track current write positions
    std::vector<Index> net_write_pos = hg.net_offsets_;
    std::vector<Index> node_write_pos = hg.node_offsets_;
    
    for (EdgeID net_id = 0; net_id < num_nets; ++net_id) {
        if (!std::getline(file, line)) {
            throw std::runtime_error("Unexpected end of file while reading nets (second pass)");
        }
        
        // Skip nets that were skipped in first pass (net_sizes[net_id] == 0)
        if (net_sizes[net_id] == 0) {
            continue;
        }
        
        Weight net_weight = 1;
        fastParseNetLine(line.c_str(), has_net_weights, net_weight, nodes_buf);
        
        // Sort nodes for better cache locality
        std::sort(nodes_buf.begin(), nodes_buf.end());
        
        // Write directly to CSR arrays
        Index net_pos = net_write_pos[net_id];
        for (NodeID node : nodes_buf) {
            if (node < num_nodes) {
                // Add to Net -> Nodes
                hg.net_nodes_[net_pos++] = node;
                
                // Add to Node -> Nets
                Index node_pos = node_write_pos[node]++;
                hg.node_nets_[node_pos] = net_id;
            }
        }
    }
    
    return hg;
}

} // namespace consmlp

