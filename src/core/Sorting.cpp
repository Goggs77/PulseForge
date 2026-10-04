#include "core/Sorting.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <unordered_set>

namespace pf {

namespace {

int depthOf(const Graph &graph, int id, std::unordered_map<int, int> &memo,
            std::unordered_set<int> &visiting) {
    const auto cached = memo.find(id);
    if (cached != memo.end()) return cached->second;
    if (!visiting.insert(id).second) return 0;  // a cycle in a hand edited file
    int depth = 0;
    bool fed = false;
    for (const Link &link : graph.links) {
        if (link.toNode != id) continue;
        const int source = depthOf(graph, link.fromNode, memo, visiting) + 1;
        if (!fed || source < depth) depth = source;
        fed = true;
    }
    visiting.erase(id);
    memo[id] = depth;
    return depth;
}

}  // namespace

std::vector<int> parseGroupMembers(const std::string &text) {
    std::vector<int> ids;
    size_t index = 0;
    while (index < text.size()) {
        while (index < text.size() &&
               !std::isdigit(static_cast<unsigned char>(text[index]))) {
            ++index;
        }
        if (index >= text.size()) break;
        const char *start = text.c_str() + index;
        const int id = std::atoi(start);
        while (index < text.size() && std::isdigit(static_cast<unsigned char>(text[index]))) {
            ++index;
        }
        if (id <= 0) continue;
        if (std::find(ids.begin(), ids.end(), id) != ids.end()) continue;
        ids.push_back(id);
    }
    return ids;
}

std::string formatGroupMembers(const std::vector<int> &ids) {
    std::string text;
    for (const int id : ids) {
        if (id <= 0) continue;
        if (!text.empty()) text += ",";
        text += std::to_string(id);
    }
    return text;
}

int blockDepth(const Graph &graph, int id) {
    std::unordered_map<int, int> memo;
    std::unordered_set<int> visiting;
    return depthOf(graph, id, memo, visiting);
}

int connectedPortCount(const Graph &graph, const Node &node) {
    int count = 0;
    for (size_t port = 0; port < node.inputPorts().size(); ++port) {
        if (graph.findInputLink(node.id, static_cast<int>(port))) ++count;
    }
    for (size_t port = 0; port < node.outputPorts().size(); ++port) {
        for (const Link &link : graph.links) {
            if (link.fromNode == node.id && link.fromPort == static_cast<int>(port)) {
                ++count;
                break;
            }
        }
    }
    return count;
}

std::vector<GroupSlot> groupLayerOrder(const Graph &graph, const std::vector<int> &members,
                                       int depthTolerance) {
    std::vector<GroupSlot> slots;
    slots.reserve(members.size());
    for (const int id : members) {
        const Node *node = graph.find(id);
        if (!node) continue;
        if (node->kind == "sort.group") continue;  // no nested groups
        GroupSlot slot;
        slot.id = id;
        slot.depth = blockDepth(graph, id);
        slot.connectedPorts = connectedPortCount(graph, *node);
        slots.push_back(slot);
    }
    // A layer covers `tolerance + 1` consecutive depths, so a tolerance of 1
    // merges depth 0 with 1 and 2 with 3 instead of letting the merge cascade.
    const int band = std::max(0, depthTolerance) + 1;
    // Compact the bands that occur into columns, so a chain that starts at
    // depth 3 still begins at the group's left edge.
    std::vector<int> depths;
    for (const GroupSlot &slot : slots) {
        const int bandIndex = slot.depth / band;
        if (std::find(depths.begin(), depths.end(), bandIndex) == depths.end()) {
            depths.push_back(bandIndex);
        }
    }
    std::sort(depths.begin(), depths.end());
    for (GroupSlot &slot : slots) {
        slot.column = static_cast<int>(
            std::find(depths.begin(), depths.end(), slot.depth / band) - depths.begin());
    }
    // Rows run top to bottom inside a column: smallest depth first (a tie inside
    // one column), then fewest connected ports, then the lowest id.
    std::sort(slots.begin(), slots.end(), [](const GroupSlot &a, const GroupSlot &b) {
        if (a.column != b.column) return a.column < b.column;
        if (a.depth != b.depth) return a.depth < b.depth;
        if (a.connectedPorts != b.connectedPorts) return a.connectedPorts < b.connectedPorts;
        return a.id < b.id;
    });
    int row = 0;
    int column = slots.empty() ? 0 : slots.front().column;
    for (GroupSlot &slot : slots) {
        if (slot.column != column) {
            column = slot.column;
            row = 0;
        }
        slot.row = row++;
    }
    return slots;
}

}  // namespace pf
