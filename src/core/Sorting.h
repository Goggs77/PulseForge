// Sorting blocks: the Sticky Note and the Group. A group owns a list of block
// ids (the `members` text parameter) and arranges them in layers that follow
// the signal chain; this header is the graph half of that, the canvas owns the
// geometry.
#pragma once

#include <string>
#include <vector>

#include "core/Graph.h"

namespace pf {

// Reads/writes the comma separated block id list stored in a group's `members`
// parameter. Unknown or duplicate ids are dropped when parsing.
std::vector<int> parseGroupMembers(const std::string &text);
std::string formatGroupMembers(const std::vector<int> &ids);

// Longest-path flow depth across the whole graph. Strongly connected components
// are collapsed first, so feedback becomes one stable step instead of recursive
// cycles. External upstream/downstream links therefore influence group layout.
int blockDepth(const Graph &graph, int id);

// Number of the block's ports that carry at least one link, inputs and outputs
// together. Used to order a group column.
int connectedPortCount(const Graph &graph, const Node &node);

// One block's slot inside a group. `column` is the compacted depth (left to
// right), `row` the position inside the column.
struct GroupSlot {
    int id = 0;
    int column = 0;
    int row = 0;
    int depth = 0;
    int connectedPorts = 0;
};

// Layered order for a group. Columns are compacted global flow-depth bands.
// Branches connected internally or through an outside path form a cluster; rows
// sort by depth, cluster order and finally original member order. The final
// stable key means deleting a link exposes a tie without reshuffling unrelated
// blocks. Layout is visual only and never rewrites IDs. A layer covers
// `depthTolerance + 1` consecutive depths.
std::vector<GroupSlot> groupLayerOrder(const Graph &graph, const std::vector<int> &members,
                                       int depthTolerance = 0);

}  // namespace pf
