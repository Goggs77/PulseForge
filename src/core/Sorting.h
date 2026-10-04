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

// Depth of a block: 0 when nothing feeds it (no input links), otherwise
// 1 + the smallest depth among the blocks feeding it. The walk follows the
// whole graph, so a member wired from outside its group is still placed after
// the block that feeds it. Cycles (only possible in a hand edited file) count
// as depth 0 instead of recursing forever.
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

// Layered order for a group: columns are the depth bands that actually occur
// (compacted so an unused band does not leave an empty column), and the rows
// inside a column are sorted by depth, then by the number of connected ports,
// then by block id. A layer covers `depthTolerance + 1` consecutive depths, so
// a tolerance of 1 puts depth 0 and 1 in the first column and 2 and 3 in the
// second. Missing blocks and nested groups are skipped.
std::vector<GroupSlot> groupLayerOrder(const Graph &graph, const std::vector<int> &members,
                                       int depthTolerance = 0);

}  // namespace pf
