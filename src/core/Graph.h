// The flow graph: blocks, typed connections and evaluation.
#pragma once

#include <deque>
#include <string>
#include <vector>

#include "core/Node.h"

namespace pf {

struct Link {
    int fromNode = 0;
    int fromPort = 0;
    int toNode = 0;
    int toPort = 0;
};

class Graph {
public:
    // std::deque keeps references to nodes stable: callers hold Node* while
    // adding more blocks, which a vector would invalidate on reallocation.
    std::deque<Node> nodes;
    std::vector<Link> links;
    int nextId = 1;
    std::string lastError;

    void clear();

    Node *addNode(const std::string &kind, float x, float y);
    Node *addNodeWithId(int id, const std::string &kind);
    void removeNode(int id);
    Node *find(int id);
    const Node *find(int id) const;
    int indexOf(int id) const;

    bool canConnect(int fromNode, int fromPort, int toNode, int toPort, std::string *why = nullptr) const;
    bool connect(int fromNode, int fromPort, int toNode, int toPort, std::string *why = nullptr);
    void disconnectInput(int toNode, int toPort);
    void removeLinksTouching(int nodeId);
    const Link *findInputLink(int toNode, int toPort) const;

    // Topological order of enabled nodes; empty when the graph has a cycle.
    bool topologicalOrder(std::vector<int> &order, std::string *error = nullptr) const;

    // Evaluates every node once for the given context. Returns false and fills
    // lastError if the graph cannot be evaluated.
    bool evaluate(EvalContext &ctx);

    // The node whose Image output drives the export; first sink found wins.
    int sinkNodeId() const;

    int nodeCount() const { return static_cast<int>(nodes.size()); }
};

}  // namespace pf
