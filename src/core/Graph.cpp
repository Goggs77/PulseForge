#include "core/Graph.h"

#include <algorithm>
#include <unordered_map>

#include "core/Registry.h"
#include "raylib.h"

namespace pf {

void Graph::clear() {
    nodes.clear();
    links.clear();
    nextId = 1;
    lastError.clear();
}

Node *Graph::addNode(const std::string &kind, float x, float y) {
    Node *node = addNodeWithId(nextId++, kind);
    if (node) {
        node->x = x;
        node->y = y;
    }
    return node;
}

Node *Graph::addNodeWithId(int id, const std::string &kind) {
    const NodeDef *def = Registry::instance().find(kind);
    if (!def) {
        lastError = "unknown block kind: " + kind;
        return nullptr;
    }
    Node node;
    node.id = id;
    node.kind = kind;
    node.def = def;
    node.title = def->label;
    node.ensureParams(*def);
    node.outputs.resize(def->outputs.size());
    nodes.push_back(std::move(node));
    nextId = std::max(nextId, id + 1);
    return &nodes.back();
}

void Graph::removeNode(int id) {
    removeLinksTouching(id);
    for (size_t i = 0; i < nodes.size(); ++i) {
        if (nodes[i].id == id) {
            nodes.erase(nodes.begin() + static_cast<long>(i));
            return;
        }
    }
}

Node *Graph::find(int id) {
    for (auto &node : nodes) {
        if (node.id == id) return &node;
    }
    return nullptr;
}

const Node *Graph::find(int id) const {
    for (const auto &node : nodes) {
        if (node.id == id) return &node;
    }
    return nullptr;
}

int Graph::indexOf(int id) const {
    for (size_t i = 0; i < nodes.size(); ++i) {
        if (nodes[i].id == id) return static_cast<int>(i);
    }
    return -1;
}

bool Graph::canConnect(int fromNode, int fromPort, int toNode, int toPort, std::string *why) const {
    const Node *source = find(fromNode);
    const Node *target = find(toNode);
    if (!source || !target) {
        if (why) *why = "missing block";
        return false;
    }
    if (fromNode == toNode) {
        if (why) *why = "a block cannot feed itself";
        return false;
    }
    if (!source->def || !target->def) {
        if (why) *why = "block definition missing";
        return false;
    }
    if (fromPort < 0 || fromPort >= static_cast<int>(source->def->outputs.size())) {
        if (why) *why = "bad source port";
        return false;
    }
    if (toPort < 0 || toPort >= static_cast<int>(target->def->inputs.size())) {
        if (why) *why = "bad target port";
        return false;
    }
    const PortType from = source->def->outputs[static_cast<size_t>(fromPort)].type;
    const PortType to = target->def->inputs[static_cast<size_t>(toPort)].type;
    if (!portTypeCompatible(from, to)) {
        if (why) {
            *why = std::string("cannot connect ") + portTypeName(from) + " to " + portTypeName(to);
        }
        return false;
    }
    // Reject connections that would close a cycle. The new edge runs fromNode ->
    // toNode, so a cycle only exists if toNode can already reach fromNode;
    // asking the opposite question would reject any second connection between
    // the same two blocks.
    std::vector<int> stack{toNode};
    std::vector<int> visited;
    while (!stack.empty()) {
        const int current = stack.back();
        stack.pop_back();
        if (current == fromNode) {
            if (why) *why = "that connection would create a cycle";
            return false;
        }
        if (std::find(visited.begin(), visited.end(), current) != visited.end()) continue;
        visited.push_back(current);
        for (const auto &link : links) {
            if (link.fromNode == current) stack.push_back(link.toNode);
        }
    }
    return true;
}

bool Graph::connect(int fromNode, int fromPort, int toNode, int toPort, std::string *why) {
    if (!canConnect(fromNode, fromPort, toNode, toPort, why)) return false;
    disconnectInput(toNode, toPort);
    links.push_back(Link{fromNode, fromPort, toNode, toPort});
    return true;
}

void Graph::disconnectInput(int toNode, int toPort) {
    links.erase(std::remove_if(links.begin(), links.end(),
                               [&](const Link &link) {
                                   return link.toNode == toNode && link.toPort == toPort;
                               }),
                links.end());
}

void Graph::removeLinksTouching(int nodeId) {
    links.erase(std::remove_if(links.begin(), links.end(),
                               [&](const Link &link) {
                                   return link.fromNode == nodeId || link.toNode == nodeId;
                               }),
                links.end());
}

const Link *Graph::findInputLink(int toNode, int toPort) const {
    for (const auto &link : links) {
        if (link.toNode == toNode && link.toPort == toPort) return &link;
    }
    return nullptr;
}

bool Graph::topologicalOrder(std::vector<int> &order, std::string *error) const {
    order.clear();
    std::unordered_map<int, int> indegree;
    for (const auto &node : nodes) indegree[node.id] = 0;
    for (const auto &link : links) {
        if (find(link.fromNode) && find(link.toNode)) indegree[link.toNode]++;
    }
    std::vector<int> ready;
    for (const auto &node : nodes) {
        if (indegree[node.id] == 0) ready.push_back(node.id);
    }
    while (!ready.empty()) {
        const int id = ready.back();
        ready.pop_back();
        order.push_back(id);
        for (const auto &link : links) {
            if (link.fromNode != id) continue;
            auto it = indegree.find(link.toNode);
            if (it == indegree.end()) continue;
            if (--it->second == 0) ready.push_back(link.toNode);
        }
    }
    if (order.size() != nodes.size()) {
        if (error) *error = "the pipeline contains a cycle";
        return false;
    }
    return true;
}

bool Graph::evaluate(EvalContext &ctx) {
    std::vector<int> order;
    if (!topologicalOrder(order, &lastError)) return false;

    for (const auto &node : nodes) {
        // Reset results of nodes that are not part of this pass.
        if (!node.enabled) {
            if (Node *mutableNode = const_cast<Graph *>(this)->find(node.id)) {
                for (auto &value : mutableNode->outputs) value = Value{};
            }
        }
    }

    for (const int id : order) {
        Node *node = find(id);
        if (!node || !node->def) continue;
        if (!node->enabled) continue;

        const NodeDef &def = *node->def;
        std::vector<Value> inputs(def.inputs.size());
        for (size_t port = 0; port < def.inputs.size(); ++port) {
            const Link *link = findInputLink(id, static_cast<int>(port));
            if (!link) continue;
            const Node *source = find(link->fromNode);
            if (!source || !source->enabled) continue;
            if (link->fromPort >= 0 && link->fromPort < static_cast<int>(source->outputs.size())) {
                // Widening (Vector2 -> Vector3/4) happens here so the type system
                // stays simple everywhere else.
                inputs[port] = convertValue(source->outputs[static_cast<size_t>(link->fromPort)],
                                            def.inputs[port].type);
            }
        }

        node->outputs.assign(def.outputs.size(), Value{});
        if (!def.evaluate) continue;

        const double start = GetTime();
        def.evaluate(*node, ctx, inputs, node->outputs);
        node->lastEvalMs = (GetTime() - start) * 1000.0;
    }
    return true;
}

int Graph::sinkNodeId() const {
    for (const auto &node : nodes) {
        if (node.def && node.def->isSink) return node.id;
    }
    return 0;
}

}  // namespace pf
