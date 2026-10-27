#include "core/Graph.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <unordered_map>

#include "core/Profile.h"
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
    Registry::applyChannelPorts(node);
    Registry::applySpectrumPreset(node);
    node.outputs.resize(node.outputPorts().size());
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
    if (fromPort < 0 || fromPort >= static_cast<int>(source->outputPorts().size())) {
        if (why) *why = "bad source port";
        return false;
    }
    if (toPort < 0 || toPort >= static_cast<int>(target->inputPorts().size())) {
        if (why) *why = "bad target port";
        return false;
    }
    const PortType from = source->outputPorts()[static_cast<size_t>(fromPort)].type;
    const PortType to = target->inputPorts()[static_cast<size_t>(toPort)].type;
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

namespace {

// A block that only processes Scalar values can run at audio rate inside an
// ADC -> DAC region. Blocks with Audio/Analysis inputs stay window-rate
// producers; their value is held while the region runs.
bool isPureScalarBlock(const Node &node) {
    for (const PortDesc &input : node.inputPorts()) {
        if (input.type != PortType::Scalar && input.type != PortType::None) return false;
    }
    for (const PortDesc &output : node.outputPorts()) {
        if (output.type == PortType::Scalar) return true;
    }
    return false;
}

// Keeps ADC/DAC channel ports in step with their `channels` parameter. Links
// that pointed at ports which no longer exist are dropped.
void syncDynamicChannelPorts(Graph &graph) {
    for (Node &node : graph.nodes) {
        if (node.kind != "dsp.adc" && node.kind != "dsp.dac") continue;
        const int channels = std::clamp(node.pint("channels", 2), 1, 8);
        const bool adc = node.kind == "dsp.adc";
        const size_t current =
            adc ? node.outputPorts().size() : node.inputPorts().size();
        if (current == static_cast<size_t>(channels)) continue;
        Registry::applyChannelPorts(node);
        if (adc) {
            graph.links.erase(
                std::remove_if(graph.links.begin(), graph.links.end(),
                               [&](const Link &link) {
                                   return link.fromNode == node.id && link.fromPort >= channels;
                               }),
                graph.links.end());
            node.outputs.resize(static_cast<size_t>(channels));
        } else {
            graph.links.erase(
                std::remove_if(graph.links.begin(), graph.links.end(),
                               [&](const Link &link) {
                                   return link.toNode == node.id && link.toPort >= channels;
                               }),
                graph.links.end());
            node.inputScratch.clear();
        }
    }
}

struct AudioRatePlan {
    bool active = false;
    std::vector<int> region;  // topological order
    std::vector<int> before;  // frame-rate nodes evaluated first
    std::vector<int> after;   // frame-rate nodes evaluated last
};

// Spectrum exposes different ports and parameters per preset. When the preset
// changes (in the Inspector, or through a project that was saved by an older
// build) the block has to be rebuilt before the graph runs. Links are
// index-based, so the input links are carried over by port name; a link into a
// port the new preset does not have is dropped.
void syncSpectrumPresets(Graph &graph) {
    for (Node &node : graph.nodes) {
        if (node.kind != "render.spectrum") continue;
        const double preset = static_cast<double>(node.pint("preset", 0));
        const auto cached = node.runtimeState.find("spectrum.preset");
        if (cached != node.runtimeState.end() && cached->second == preset) continue;
        node.runtimeState["spectrum.preset"] = preset;

        std::vector<std::string> previous;
        previous.reserve(node.inputPorts().size());
        for (const PortDesc &port : node.inputPorts()) previous.push_back(port.name);
        Registry::applySpectrumPreset(node);
        const std::vector<PortDesc> &ports = node.inputPorts();
        std::vector<int> remap(previous.size(), -1);
        for (size_t old = 0; old < previous.size(); ++old) {
            for (size_t port = 0; port < ports.size(); ++port) {
                if (ports[port].name == previous[old]) {
                    remap[old] = static_cast<int>(port);
                    break;
                }
            }
        }
        graph.links.erase(std::remove_if(graph.links.begin(), graph.links.end(),
                                         [&](const Link &link) {
                                             if (link.toNode != node.id) return false;
                                             return link.toPort < 0 ||
                                                    link.toPort >=
                                                        static_cast<int>(remap.size()) ||
                                                    remap[static_cast<size_t>(link.toPort)] < 0;
                                         }),
                          graph.links.end());
        for (Link &link : graph.links) {
            if (link.toNode == node.id) link.toPort = remap[static_cast<size_t>(link.toPort)];
        }
        node.inputScratch.clear();
    }
}

// Every Scalar descendant of an enabled ADC is audio-rate, with or without a
// downstream DAC. DACs terminate their branch; non-Scalar consumers stop the
// sample-rate propagation and are evaluated later using the final sample.
AudioRatePlan buildAudioRatePlan(const Graph &graph, const std::vector<int> &order) {
    AudioRatePlan plan;
    std::unordered_map<int, int> indexOf;
    indexOf.reserve(graph.nodes.size());
    for (size_t i = 0; i < graph.nodes.size(); ++i) {
        indexOf[graph.nodes[i].id] = static_cast<int>(i);
    }
    const int count = static_cast<int>(graph.nodes.size());
    std::vector<std::vector<int>> scalarOut(static_cast<size_t>(count));
    std::vector<std::vector<int>> scalarIn(static_cast<size_t>(count));
    std::vector<std::vector<int>> allOut(static_cast<size_t>(count));
    for (const Link &link : graph.links) {
        const auto from = indexOf.find(link.fromNode);
        const auto to = indexOf.find(link.toNode);
        if (from == indexOf.end() || to == indexOf.end()) continue;
        const Node &source = graph.nodes[static_cast<size_t>(from->second)];
        const Node &target = graph.nodes[static_cast<size_t>(to->second)];
        if (!source.enabled || !target.enabled) continue;
        allOut[static_cast<size_t>(from->second)].push_back(to->second);
        const std::vector<PortDesc> &outputs = source.outputPorts();
        const std::vector<PortDesc> &inputs = target.inputPorts();
        if (link.fromPort < 0 || link.fromPort >= static_cast<int>(outputs.size()) ||
            link.toPort < 0 || link.toPort >= static_cast<int>(inputs.size()) ||
            outputs[static_cast<size_t>(link.fromPort)].type != PortType::Scalar ||
            inputs[static_cast<size_t>(link.toPort)].type != PortType::Scalar) {
            continue;
        }
        scalarOut[static_cast<size_t>(from->second)].push_back(to->second);
        scalarIn[static_cast<size_t>(to->second)].push_back(from->second);
    }

    std::vector<unsigned char> inRegion(static_cast<size_t>(count), 0);
    std::vector<int> pending;
    for (int i = 0; i < count; ++i) {
        const Node &node = graph.nodes[static_cast<size_t>(i)];
        if (node.enabled && node.kind == "dsp.adc") {
            inRegion[static_cast<size_t>(i)] = 1;
            pending.push_back(i);
        }
    }
    if (pending.empty()) return plan;

    // ADC -> Scalar propagation. DACs terminate nothing structurally, but their
    // output is Audio, so ordinary Scalar propagation stops there naturally.
    for (size_t next = 0; next < pending.size(); ++next) {
        const int current = pending[next];
        for (const int targetIndex : scalarOut[static_cast<size_t>(current)]) {
            if (inRegion[static_cast<size_t>(targetIndex)]) continue;
            const Node &target = graph.nodes[static_cast<size_t>(targetIndex)];
            if (target.kind == "dsp.dac" || isPureScalarBlock(target)) {
                inRegion[static_cast<size_t>(targetIndex)] = 1;
                pending.push_back(targetIndex);
            }
        }
    }

    // Pull in pure scalar producers feeding any audio-rate input.
    pending.clear();
    for (int i = 0; i < count; ++i) {
        if (inRegion[static_cast<size_t>(i)]) pending.push_back(i);
    }
    for (size_t next = 0; next < pending.size(); ++next) {
        const int current = pending[next];
        for (const int sourceIndex : scalarIn[static_cast<size_t>(current)]) {
            if (inRegion[static_cast<size_t>(sourceIndex)]) continue;
            const Node &source = graph.nodes[static_cast<size_t>(sourceIndex)];
            if (isPureScalarBlock(source)) {
                inRegion[static_cast<size_t>(sourceIndex)] = 1;
                pending.push_back(sourceIndex);
            }
        }
    }

    std::vector<unsigned char> downstream(static_cast<size_t>(count), 0);
    pending.clear();
    for (int i = 0; i < count; ++i) {
        if (inRegion[static_cast<size_t>(i)]) pending.push_back(i);
    }
    for (size_t next = 0; next < pending.size(); ++next) {
        const int current = pending[next];
        for (const int targetIndex : allOut[static_cast<size_t>(current)]) {
            if (inRegion[static_cast<size_t>(targetIndex)] ||
                downstream[static_cast<size_t>(targetIndex)]) {
                continue;
            }
            downstream[static_cast<size_t>(targetIndex)] = 1;
            pending.push_back(targetIndex);
        }
    }

    for (const int id : order) {
        const auto index = indexOf.find(id);
        if (index == indexOf.end()) continue;
        const Node &node = graph.nodes[static_cast<size_t>(index->second)];
        if (!node.enabled) continue;
        if (inRegion[static_cast<size_t>(index->second)]) plan.region.push_back(id);
        else if (downstream[static_cast<size_t>(index->second)]) plan.after.push_back(id);
        else plan.before.push_back(id);
    }
    plan.active = !plan.region.empty();
    return plan;
}

void gatherInputs(const Graph &graph, Node &node, std::vector<Value> &inputs) {
    const std::vector<PortDesc> &ports = node.inputPorts();
    inputs.resize(ports.size());
    for (Value &value : inputs) value = Value{};
    for (size_t port = 0; port < ports.size(); ++port) {
        const Link *link = graph.findInputLink(node.id, static_cast<int>(port));
        if (!link) continue;
        const Node *source = graph.find(link->fromNode);
        if (!source || !source->enabled) continue;
        if (link->fromPort >= 0 && link->fromPort < static_cast<int>(source->outputs.size())) {
            // Widening (Vector2 -> Vector3/4) happens here so the type system
            // stays simple everywhere else.
            inputs[port] = convertValue(source->outputs[static_cast<size_t>(link->fromPort)],
                                        ports[port].type);
        }
    }
}

void propagateCarrier(Node &node, const std::vector<Value> &inputs) {
    AudioPtr carrier;
    for (const Value &value : inputs) {
        if (value.type == PortType::Scalar && value.carrier) {
            carrier = value.carrier;
            break;
        }
    }
    if (!carrier) return;
    for (Value &value : node.outputs) {
        if (value.type == PortType::Scalar && !value.carrier) value.carrier = carrier;
    }
}

void evaluateFrameNode(Graph &graph, Node &node, EvalContext &ctx, bool measure) {
    gatherInputs(graph, node, node.inputScratch);
    node.outputs.assign(node.outputPorts().size(), Value{});
    if (!node.def || !node.def->evaluate) return;
    const double start = measure ? GetTime() : 0.0;
    node.def->evaluate(node, ctx, node.inputScratch, node.outputs);
    if (measure) node.lastEvalMs = (GetTime() - start) * 1000.0;
    propagateCarrier(node, node.inputScratch);
}

// Resolved input used by the per-sample ADC/DAC path so the loop does not
// search the link list for every sample.
struct ChannelInput {
    const Node *source = nullptr;
    int port = 0;
};

void sampleAdcNode(Node &node, const EvalContext &ctx, const ChannelInput &input) {
    const int channels = std::max(1, static_cast<int>(node.outputPorts().size()));
    if (static_cast<int>(node.outputs.size()) != channels) {
        node.outputs.assign(static_cast<size_t>(channels), Value{});
    }
    const AudioBuffer *buffer = nullptr;
    if (input.source && input.port >= 0 &&
        input.port < static_cast<int>(input.source->outputs.size())) {
        buffer = input.source->outputs[static_cast<size_t>(input.port)].audio.get();
    }
    // No Audio input means silence: never fall back to the project's imported
    // clip, otherwise an unconnected ADC keeps playing the source.
    const int sourceChannels = buffer ? std::max(1, buffer->channels) : 1;
    long long local = -1;
    if (buffer && buffer->frameCount > 0 && !buffer->samples.empty()) {
        local = std::llround(ctx.audioTime * buffer->sampleRate) - buffer->startFrame;
        if (local < 0 || local >= buffer->frameCount) local = -1;
    }
    for (int c = 0; c < channels; ++c) {
        float value = 0.0f;
        if (local >= 0) {
            const int sourceChannel = std::min(c, sourceChannels - 1);
            const size_t index = static_cast<size_t>(local) *
                                     static_cast<size_t>(sourceChannels) +
                                 static_cast<size_t>(sourceChannel);
            if (index < buffer->samples.size()) value = buffer->samples[index];
        }
        node.outputs[static_cast<size_t>(c)].setScalar(value);
        if (node.publishReadouts) {
            node.runtimeState["value." + std::to_string(c)] = value;
        }
    }
}

void sampleDacNode(Node &node, const EvalContext &ctx,
                   const std::vector<ChannelInput> &inputs) {
    const int channels = static_cast<int>(inputs.size());
    const bool clampOutput = node.pbool("clamp", true);
    float values[8] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    bool haveValue = false;
    float firstValue = 0.0f;
    for (int c = 0; c < channels && c < 8; ++c) {
        const ChannelInput &input = inputs[static_cast<size_t>(c)];
        if (!input.source || input.port < 0 ||
            input.port >= static_cast<int>(input.source->outputs.size())) {
            continue;
        }
        values[c] = input.source->outputs[static_cast<size_t>(input.port)].asScalar(0.0f);
        if (!std::isfinite(values[c])) values[c] = 0.0f;
        if (!haveValue) {
            firstValue = values[c];
            haveValue = true;
        }
    }
    // Legacy single-port chains had no channel concept: an unconnected channel
    // follows the first connected one so the result stays dual-mono instead of
    // silencing the right side.
    for (int c = 0; c < channels && c < 8; ++c) {
        const ChannelInput &input = inputs[static_cast<size_t>(c)];
        const bool connected =
            input.source && input.port >= 0 &&
            input.port < static_cast<int>(input.source->outputs.size());
        if (!connected) values[c] = haveValue ? firstValue : 0.0f;
        if (clampOutput) values[c] = std::clamp(values[c], -1.0f, 1.0f);
    }
    AudioBuffer *buffer = node.audioRenderOutput.get();
    if (node.audioRenderWrite >= 0 && buffer) {
        const int bufferChannels = std::max(1, buffer->channels);
        const size_t base = static_cast<size_t>(node.audioRenderWrite) *
                            static_cast<size_t>(bufferChannels);
        if (base + static_cast<size_t>(bufferChannels) <= buffer->samples.size()) {
            for (int c = 0; c < bufferChannels; ++c) {
                const float value = c < 8 ? values[c] : firstValue;
                buffer->samples[base + static_cast<size_t>(c)] = value;
            }
        }
        ++node.audioRenderWrite;
    }
    if (node.publishReadouts) {
        for (int c = 0; c < channels && c < 8; ++c) {
            node.runtimeState["value." + std::to_string(c)] = values[c];
        }
    }
    (void)ctx;
}

void prepareDacBuffer(Node &node, const EvalContext &ctx, int rate, const void *source,
                      long long sourceEnd, long long startFrame, long long endFrame) {
    const long long frames = std::max<long long>(0, endFrame - startFrame);
    const int channels = std::max(1, static_cast<int>(node.inputPorts().size()));
    char key[224];
    // The source buffer may grow while a Dynamics chain feeds it, so only its
    // identity (not its current length) belongs in the key; otherwise every
    // frame would look like a different render and restart the output buffer.
    std::snprintf(key, sizeof(key), "%p|%d|%d|%d", source, rate, channels,
                  node.pbool("clamp", true) ? 1 : 0);
    const bool reusable = node.audioRenderOutput && node.audioRenderKey == key &&
                          startFrame >= node.audioRenderStart &&
                          endFrame <= node.audioRenderStart + node.audioRenderFrames;
    if (reusable) {
        node.audioRenderWrite = -1;
    } else if (ctx.offline) {
        const bool append = node.audioRenderOutput && node.audioRenderKey == key &&
                            startFrame == node.audioRenderStart + node.audioRenderFrames;
        if (!append) {
            node.audioRenderOutput = std::make_shared<AudioBuffer>();
            node.audioRenderOutput->channels = channels;
            node.audioRenderOutput->sampleRate = rate;
            node.audioRenderOutput->startFrame = startFrame;
            node.audioRenderStart = startFrame;
            node.audioRenderFrames = 0;
        }
        node.audioRenderOutput->samples.resize(
            static_cast<size_t>(node.audioRenderFrames + frames) *
                static_cast<size_t>(channels),
            0.0f);
        node.audioRenderWrite = node.audioRenderFrames;
        node.audioRenderKey = key;
    } else {
        // Interactive playback: pre-rendered monitor buffers are reused, and a
        // fresh small window is built otherwise so live playback never mutates
        // the buffer being streamed.
        node.audioRenderOutput = std::make_shared<AudioBuffer>();
        node.audioRenderOutput->channels = channels;
        node.audioRenderOutput->sampleRate = rate;
        node.audioRenderOutput->startFrame = startFrame;
        node.audioRenderOutput->samples.assign(
            static_cast<size_t>(frames) * static_cast<size_t>(channels), 0.0f);
        node.audioRenderOutput->frameCount = frames;
        node.audioRenderStart = startFrame;
        node.audioRenderFrames = frames;
        node.audioRenderKey = key;
        node.audioRenderWrite = 0;
    }
    node.outputs.assign(1, Value{});
    node.outputs[0].type = PortType::Audio;
    node.outputs[0].audio = node.audioRenderOutput;
}

// ---------------------------------------------------------------------------
// Compiled audio-rate region
// ---------------------------------------------------------------------------
// The per-sample loop used to re-resolve everything for every sample: each node
// with a linear Graph::find, its links with another scan per port, a fresh
// EvalContext copy, and a cleared and refilled Value scratch per node. None of
// that depends on the sample, so it is compiled once per frame into the steps
// below; the loop then only moves resolved values and calls the evaluator.
struct RegionSource {
    const Node *node = nullptr;  // null: nothing linked, or the source is off
    int port = 0;
};

struct RegionStep {
    enum class Kind : int { Block, Adc, Dac };
    Node *node = nullptr;
    Kind kind = Kind::Block;
    std::vector<RegionSource> sources;    // Block: one entry per input port
    ChannelInput adcInput;                // Adc
    std::vector<ChannelInput> dacInputs;  // Dac
#if PF_PROFILE_LEVEL >= 3
    double profileMs = 0.0;
#endif
};

void compileRegion(Graph &graph, const AudioRatePlan &plan, std::vector<RegionStep> *steps) {
    steps->clear();
    steps->reserve(plan.region.size());
    std::unordered_map<int, Node *> nodesById;
    nodesById.reserve(graph.nodes.size());
    for (Node &node : graph.nodes) nodesById[node.id] = &node;
    const auto nodeAt = [&](int id) -> Node * {
        const auto found = nodesById.find(id);
        return found == nodesById.end() ? nullptr : found->second;
    };
    for (const int id : plan.region) {
        Node *node = nodeAt(id);
        if (!node || !node->enabled) continue;
        node->runsAudioRate = true;
        RegionStep step;
        step.node = node;
        if (node->kind == "dsp.adc") step.kind = RegionStep::Kind::Adc;
        else if (node->kind == "dsp.dac") step.kind = RegionStep::Kind::Dac;
        if (step.kind == RegionStep::Kind::Adc) {
            if (const Link *link = graph.findInputLink(id, 0)) {
                step.adcInput.source = nodeAt(link->fromNode);
                step.adcInput.port = link->fromPort;
            }
        } else if (step.kind == RegionStep::Kind::Dac) {
            step.dacInputs.resize(node->inputPorts().size());
            for (size_t port = 0; port < step.dacInputs.size(); ++port) {
                if (const Link *link = graph.findInputLink(id, static_cast<int>(port))) {
                    step.dacInputs[port].source = nodeAt(link->fromNode);
                    step.dacInputs[port].port = link->fromPort;
                }
            }
        } else {
            const std::vector<PortDesc> &ports = node->inputPorts();
            step.sources.resize(ports.size());
            for (size_t port = 0; port < ports.size(); ++port) {
                if (const Link *link = graph.findInputLink(id, static_cast<int>(port))) {
                    const Node *source = nodeAt(link->fromNode);
                    if (source && source->enabled) {
                        step.sources[port].node = source;
                        step.sources[port].port = link->fromPort;
                    }
                }
            }
            // The scratch is cleared once instead of per sample: a port with no
            // resolved source stays None, which is what an evaluator expects for
            // an unconnected input. Outputs are written by the evaluator on every
            // sample, so they only need their size here.
            node->inputScratch.assign(ports.size(), Value{});
            node->outputs.assign(node->outputPorts().size(), Value{});
        }
        steps->push_back(std::move(step));
    }
}

// One pure-Scalar block of the region, for one sample: move the resolved inputs
// into the scratch and evaluate. The carrier propagation of the frame-rate path
// is not needed here - the ADC's Scalars never carry one, so nothing inside the
// region can see it.
void sampleRegionBlock(Node &node, EvalContext &ctx, const std::vector<RegionSource> &sources) {
    const std::vector<PortDesc> &ports = node.inputPorts();
    for (size_t port = 0; port < sources.size() && port < ports.size(); ++port) {
        const RegionSource &source = sources[port];
        if (!source.node || source.port < 0 ||
            source.port >= static_cast<int>(source.node->outputs.size())) {
            continue;
        }
        const Value &input = source.node->outputs[static_cast<size_t>(source.port)];
        if (input.type == PortType::Scalar && ports[port].type == PortType::Scalar) {
            // Prefer the reusable Scalar payload over copying a full Value with
            // its string and shared-pointer members.
            node.inputScratch[port].setScalar(input.scalar);
        } else {
            node.inputScratch[port] = convertValue(input, ports[port].type);
        }
    }
    if (node.def && node.def->evaluate) node.def->evaluate(node, ctx, node.inputScratch, node.outputs);
}

void evaluateAudioRegion(Graph &graph, const AudioRatePlan &plan, EvalContext &ctx) {
    int rate = ctx.audio ? std::max(1, ctx.audio->sampleRate) : 48000;
    const void *source = ctx.audio.get();
    long long sourceEnd = -1;
    for (const int id : plan.region) {
        Node *node = graph.find(id);
        if (!node || node->kind != "dsp.adc") continue;
        const Link *link = graph.findInputLink(id, 0);
        const Node *input = link ? graph.find(link->fromNode) : nullptr;
        if (input && link->fromPort >= 0 &&
            link->fromPort < static_cast<int>(input->outputs.size())) {
            const AudioPtr buffer = input->outputs[static_cast<size_t>(link->fromPort)].audio;
            if (buffer) {
                rate = std::max(1, buffer->sampleRate);
                source = buffer.get();
                sourceEnd = buffer->startFrame + buffer->frameCount;
            }
        } else if (ctx.audio) {
            sourceEnd = ctx.audio->startFrame + ctx.audio->frameCount;
        }
        break;
    }

    const double fps = ctx.fps > 1.0f ? ctx.fps : 60.0f;
    // In preview the display playhead is continuous while the graph is a video
    // frame graph: quantise to the current video frame so a 144 Hz display does
    // not run stateful audio blocks several times for the same frame.
    double clipFrameStart = ctx.audioTime;
    if (!ctx.offline) {
        const double quantized = static_cast<double>(ctx.frame) / fps;
        clipFrameStart = ctx.audioTime + (quantized - ctx.time);
    }
    long long startFrame = std::max<long long>(0, std::llround(clipFrameStart * rate));
    long long endFrame = std::llround((clipFrameStart + 1.0 / fps) * rate);
    if (sourceEnd >= 0) endFrame = std::min(endFrame, sourceEnd);
    if (endFrame < startFrame) endFrame = startFrame;

    // Compile the region once: nodes, their input sources and the ADC/DAC
    // plumbing. The sample loop only moves values afterwards.
    std::vector<RegionStep> steps;
    compileRegion(graph, plan, &steps);

    std::vector<int> dacs;
    // Audio-rate branches without a DAC still have to advance every sample;
    // they may feed a frame-rate visual or be intentional side-effect blocks.
    bool needsRender = !steps.empty();
    for (const RegionStep &step : steps) {
        if (step.kind != RegionStep::Kind::Dac) continue;
        prepareDacBuffer(*step.node, ctx, rate, source, sourceEnd, startFrame, endFrame);
        if (step.node->audioRenderWrite >= 0) needsRender = true;
        dacs.push_back(step.node->id);
    }

    // One context for the whole loop: the evaluators only read it, and copying
    // it per sample was shared_ptr traffic for nothing.
    EvalContext sample = ctx;
    sample.audioRate = true;
    sample.audioSampleRate = rate;
#if PF_PROFILE_LEVEL >= 3
    const double regionStart = GetTime();
#endif
    for (long long frame = startFrame; needsRender && frame < endFrame; ++frame) {
        sample.audioTime = static_cast<double>(frame) / rate;
        sample.time = ctx.time + static_cast<double>(frame - startFrame) / rate;
        const bool lastSample = frame + 1 >= endFrame;
        for (RegionStep &step : steps) {
            // The editor only ever sees the frame's final values, so nothing
            // publishes readouts for the samples before it.
            step.node->publishReadouts = lastSample;
#if PF_PROFILE_LEVEL >= 3
            const double stepStart = GetTime();
#endif
            switch (step.kind) {
                case RegionStep::Kind::Adc:
                    sampleAdcNode(*step.node, sample, step.adcInput);
                    break;
                case RegionStep::Kind::Dac:
                    sampleDacNode(*step.node, sample, step.dacInputs);
                    break;
                default:
                    sampleRegionBlock(*step.node, sample, step.sources);
                    break;
            }
#if PF_PROFILE_LEVEL >= 3
            step.profileMs += (GetTime() - stepStart) * 1000.0;
#endif
        }
    }
#if PF_PROFILE_LEVEL >= 3
    profile::addNode(-1, "Audio-rate region", (GetTime() - regionStart) * 1000.0);
    for (const RegionStep &step : steps) {
        profile::addNode(step.node->id,
                         step.node->displayTitle() + " #" + std::to_string(step.node->id) +
                             " (audio)",
                         step.profileMs);
    }
#endif

    for (const int id : dacs) {
        Node *node = graph.find(id);
        if (!node || !node->audioRenderOutput) continue;
        if (node->audioRenderWrite >= 0) {
            node->audioRenderFrames = node->audioRenderWrite;
            node->audioRenderOutput->frameCount = node->audioRenderFrames;
            node->audioRenderWrite = -1;
        }
        node->outputs.assign(1, Value{});
        node->outputs[0].type = PortType::Audio;
        node->outputs[0].audio = node->audioRenderOutput;
    }
}

}  // namespace

bool Graph::evaluate(EvalContext &ctx) {
    ctx.evaluation = ++evaluationSerial;
    syncDynamicChannelPorts(*this);
    syncSpectrumPresets(*this);
    for (Node &node : nodes) node.runsAudioRate = false;
    std::vector<int> order;
    if (!topologicalOrder(order, &lastError)) return false;

    for (const auto &node : nodes) {
        // Reset results of nodes that are not part of this pass.
        if (!node.enabled) {
            if (Node *mutableNode = find(node.id)) {
                for (auto &value : mutableNode->outputs) value = Value{};
            }
        }
    }

    const AudioRatePlan plan = buildAudioRatePlan(*this, order);
    if (!plan.active) {
        for (const int id : order) {
            Node *node = find(id);
            if (!node || !node->enabled || !node->def) continue;
            evaluateFrameNode(*this, *node, ctx, true);
        }
        return true;
    }

    for (const int id : plan.before) {
        Node *node = find(id);
        if (node && node->enabled) evaluateFrameNode(*this, *node, ctx, true);
    }
    evaluateAudioRegion(*this, plan, ctx);
    for (const int id : plan.after) {
        Node *node = find(id);
        if (node && node->enabled) evaluateFrameNode(*this, *node, ctx, true);
    }
    return true;
}

namespace {

int firstSinkWithInput(const Graph &graph, PortType type) {
    for (const auto &node : graph.nodes) {
        if (!node.def || !node.def->isSink) continue;
        const std::vector<PortDesc> &inputs = node.inputPorts();
        if (!inputs.empty() && inputs[0].type == type) return node.id;
    }
    return 0;
}

}  // namespace

int Graph::sinkNodeId() const {
    for (const auto &node : nodes) {
        if (node.def && node.def->isSink) return node.id;
    }
    return 0;
}

int Graph::videoSinkNodeId() const { return firstSinkWithInput(*this, PortType::Image); }

int Graph::audioSinkNodeId() const { return firstSinkWithInput(*this, PortType::Audio); }

int Graph::renderCaptureScale() const {
    int scale = 1;
    for (const Node &node : nodes) {
        if (!node.enabled || node.kind != "render.selfref") continue;
        scale = std::max(scale, std::clamp(node.pint("hyper", 1), 1, 4));
    }
    return scale;
}

}  // namespace pf
