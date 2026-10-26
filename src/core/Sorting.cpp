#include "core/Sorting.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <unordered_map>
#include <unordered_set>

namespace pf {

namespace {

std::unordered_map<int, int> graphRanks(const Graph &graph) {
    std::unordered_map<int, int> nodeIndex;
    std::vector<int> ids;
    ids.reserve(graph.nodes.size());
    for (const Node &node : graph.nodes) {
        nodeIndex[node.id] = static_cast<int>(ids.size());
        ids.push_back(node.id);
    }
    const int count = static_cast<int>(ids.size());
    std::vector<std::vector<int>> edges(static_cast<size_t>(count));
    for (const Link &link : graph.links) {
        const auto from = nodeIndex.find(link.fromNode);
        const auto to = nodeIndex.find(link.toNode);
        if (from == nodeIndex.end() || to == nodeIndex.end()) continue;
        edges[static_cast<size_t>(from->second)].push_back(to->second);
    }

    // Collapse strongly connected components first. Feedback cycles then take
    // part in the flow order as one unit instead of resetting recursion to 0.
    std::vector<int> discovery(static_cast<size_t>(count), -1);
    std::vector<int> low(static_cast<size_t>(count), 0);
    std::vector<int> component(static_cast<size_t>(count), -1);
    std::vector<bool> onStack(static_cast<size_t>(count), false);
    std::vector<int> stack;
    int nextDiscovery = 0;
    int componentCount = 0;
    std::function<void(int)> visit = [&](int node) {
        discovery[static_cast<size_t>(node)] = low[static_cast<size_t>(node)] =
            nextDiscovery++;
        stack.push_back(node);
        onStack[static_cast<size_t>(node)] = true;
        for (const int next : edges[static_cast<size_t>(node)]) {
            if (discovery[static_cast<size_t>(next)] < 0) {
                visit(next);
                low[static_cast<size_t>(node)] =
                    std::min(low[static_cast<size_t>(node)],
                             low[static_cast<size_t>(next)]);
            } else if (onStack[static_cast<size_t>(next)]) {
                low[static_cast<size_t>(node)] =
                    std::min(low[static_cast<size_t>(node)],
                             discovery[static_cast<size_t>(next)]);
            }
        }
        if (low[static_cast<size_t>(node)] != discovery[static_cast<size_t>(node)]) return;
        while (!stack.empty()) {
            const int member = stack.back();
            stack.pop_back();
            onStack[static_cast<size_t>(member)] = false;
            component[static_cast<size_t>(member)] = componentCount;
            if (member == node) break;
        }
        ++componentCount;
    };
    for (int i = 0; i < count; ++i) {
        if (discovery[static_cast<size_t>(i)] < 0) visit(i);
    }

    std::vector<std::unordered_set<int>> componentEdges(
        static_cast<size_t>(componentCount));
    std::vector<int> indegree(static_cast<size_t>(componentCount), 0);
    for (int from = 0; from < count; ++from) {
        for (const int to : edges[static_cast<size_t>(from)]) {
            const int a = component[static_cast<size_t>(from)];
            const int b = component[static_cast<size_t>(to)];
            if (a == b) continue;
            if (componentEdges[static_cast<size_t>(a)].insert(b).second) {
                ++indegree[static_cast<size_t>(b)];
            }
        }
    }
    std::vector<int> ready;
    for (int i = 0; i < componentCount; ++i) {
        if (indegree[static_cast<size_t>(i)] == 0) ready.push_back(i);
    }
    std::vector<int> rank(static_cast<size_t>(componentCount), 0);
    for (size_t next = 0; next < ready.size(); ++next) {
        const int current = ready[next];
        for (const int target : componentEdges[static_cast<size_t>(current)]) {
            rank[static_cast<size_t>(target)] =
                std::max(rank[static_cast<size_t>(target)],
                         rank[static_cast<size_t>(current)] + 1);
            if (--indegree[static_cast<size_t>(target)] == 0) ready.push_back(target);
        }
    }

    std::unordered_map<int, int> result;
    for (int i = 0; i < count; ++i) {
        result[ids[static_cast<size_t>(i)]] =
            rank[static_cast<size_t>(component[static_cast<size_t>(i)])];
    }
    return result;
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
    const std::unordered_map<int, int> ranks = graphRanks(graph);
    const auto found = ranks.find(id);
    return found == ranks.end() ? 0 : found->second;
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
    std::unordered_map<int, int> slotOf;
    for (const int id : members) {
        if (slotOf.find(id) != slotOf.end()) continue;
        const Node *node = graph.find(id);
        if (!node) continue;
        if (node->kind == "sort.group") continue;  // no nested groups
        slotOf[id] = static_cast<int>(slots.size());
        GroupSlot slot;
        slot.id = id;
        slot.connectedPorts = connectedPortCount(graph, *node);
        slots.push_back(slot);
    }
    if (slots.empty()) return slots;

    // Global SCC ranks follow the whole graph, including links entering and
    // leaving the group. Longest-path ranks keep a real start-to-finish order;
    // cycles are already collapsed into shared ranks by graphRanks().
    const std::unordered_map<int, int> ranks = graphRanks(graph);
    for (GroupSlot &slot : slots) {
        const auto found = ranks.find(slot.id);
        slot.depth = found == ranks.end() ? 0 : found->second;
    }

    std::vector<int> parent(slots.size());
    for (size_t i = 0; i < parent.size(); ++i) parent[i] = static_cast<int>(i);
    std::function<int(int)> rootOf = [&](int index) {
        if (parent[static_cast<size_t>(index)] != index) {
            parent[static_cast<size_t>(index)] = rootOf(parent[static_cast<size_t>(index)]);
        }
        return parent[static_cast<size_t>(index)];
    };
    auto join = [&](int a, int b) {
        a = rootOf(a);
        b = rootOf(b);
        if (a != b) parent[static_cast<size_t>(std::max(a, b))] = std::min(a, b);
    };

    // Internal links define clusters directly.
    for (const Link &link : graph.links) {
        const auto from = slotOf.find(link.fromNode);
        const auto to = slotOf.find(link.toNode);
        if (from != slotOf.end() && to != slotOf.end()) {
            join(from->second, to->second);
        }
    }

    // Trace paths through nodes outside the group. Members connected through
    // the same external branch stay in one visual cluster, which lets the row
    // order group related branches without changing any graph entity IDs.
    std::unordered_map<int, std::vector<int>> outgoing;
    std::unordered_map<int, std::vector<int>> incoming;
    for (const Link &link : graph.links) {
        outgoing[link.fromNode].push_back(link.toNode);
        incoming[link.toNode].push_back(link.fromNode);
    }
    for (const GroupSlot &slot : slots) {
        std::vector<int> pending;
        std::unordered_set<int> visited;
        auto enqueueOutside = [&](const std::vector<int> &neighbours) {
            for (const int id : neighbours) {
                if (visited.insert(id).second) pending.push_back(id);
            }
        };
        const auto out = outgoing.find(slot.id);
        if (out != outgoing.end()) enqueueOutside(out->second);
        const auto in = incoming.find(slot.id);
        if (in != incoming.end()) enqueueOutside(in->second);
        for (size_t next = 0; next < pending.size(); ++next) {
            const int id = pending[next];
            const auto member = slotOf.find(id);
            if (member != slotOf.end()) {
                join(slotOf[slot.id], member->second);
                continue;
            }
            const Node *node = graph.find(id);
            if (node && node->kind == "sort.group") continue;
            const auto nextOut = outgoing.find(id);
            if (nextOut != outgoing.end()) enqueueOutside(nextOut->second);
            const auto nextIn = incoming.find(id);
            if (nextIn != incoming.end()) enqueueOutside(nextIn->second);
        }
    }

    std::unordered_map<int, int> clusterFirst;
    std::unordered_map<int, int> clusterRank;
    for (size_t i = 0; i < slots.size(); ++i) {
        const int root = rootOf(static_cast<int>(i));
        const auto first = clusterFirst.find(root);
        if (first == clusterFirst.end() || static_cast<int>(i) < first->second) {
            clusterFirst[root] = static_cast<int>(i);
        }
        const auto rank = clusterRank.find(root);
        if (rank == clusterRank.end() || slots[i].depth < rank->second) {
            clusterRank[root] = slots[i].depth;
        }
    }
    std::vector<int> clusterRoots;
    for (const auto &entry : clusterFirst) clusterRoots.push_back(entry.first);
    std::sort(clusterRoots.begin(), clusterRoots.end(), [&](int a, int b) {
        if (clusterRank[a] != clusterRank[b]) return clusterRank[a] < clusterRank[b];
        return clusterFirst[a] < clusterFirst[b];
    });
    std::unordered_map<int, int> clusterOrder;
    for (size_t i = 0; i < clusterRoots.size(); ++i) {
        clusterOrder[clusterRoots[i]] = static_cast<int>(i);
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
    std::unordered_map<int, int> stableIndex;
    for (size_t i = 0; i < slots.size(); ++i) stableIndex[slots[i].id] = static_cast<int>(i);
    // Rows keep flow order, then cluster identity, then the original member
    // list order. Breaking a link can therefore expose a tie without shuffling
    // unrelated blocks by id or port count.
    std::sort(slots.begin(), slots.end(), [&](const GroupSlot &a, const GroupSlot &b) {
        if (a.column != b.column) return a.column < b.column;
        const int clusterA = clusterOrder[rootOf(slotOf.at(a.id))];
        const int clusterB = clusterOrder[rootOf(slotOf.at(b.id))];
        if (clusterA != clusterB) return clusterA < clusterB;
        if (a.depth != b.depth) return a.depth < b.depth;
        return stableIndex[a.id] < stableIndex[b.id];
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
