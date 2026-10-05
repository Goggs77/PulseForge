#include "core/Graph.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <unordered_map>
#include <unordered_set>

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

// Finds every node on an ADC -> Scalar -> DAC path, plus the pure-Scalar
// processors that feed such a path. Those nodes are evaluated once per audio
// sample; everything downstream of the region is evaluated afterwards with the
// last sample as the downsampled value.
AudioRatePlan buildAudioRatePlan(const Graph &graph, const std::vector<int> &order) {
    AudioRatePlan plan;
    std::vector<int> adcs;
    std::vector<int> dacs;
    for (const Node &node : graph.nodes) {
        if (!node.enabled) continue;
        if (node.kind == "dsp.adc") adcs.push_back(node.id);
        else if (node.kind == "dsp.dac") dacs.push_back(node.id);
    }
    if (adcs.empty() || dacs.empty()) return plan;

    const auto scalarLink = [&](const Link &link) {
        const Node *from = graph.find(link.fromNode);
        const Node *to = graph.find(link.toNode);
        if (!from || !to || !from->enabled || !to->enabled) return false;
        const std::vector<PortDesc> &outputs = from->outputPorts();
        const std::vector<PortDesc> &inputs = to->inputPorts();
        if (link.fromPort < 0 || link.fromPort >= static_cast<int>(outputs.size())) return false;
        if (link.toPort < 0 || link.toPort >= static_cast<int>(inputs.size())) return false;
        return outputs[link.fromPort].type == PortType::Scalar &&
               inputs[link.toPort].type == PortType::Scalar;
    };
    const auto reach = [&](int start, bool forward) {
        std::unordered_set<int> seen{start};
        std::vector<int> stack{start};
        while (!stack.empty()) {
            const int current = stack.back();
            stack.pop_back();
            for (const Link &link : graph.links) {
                int next = 0;
                if (forward && link.fromNode == current && scalarLink(link)) next = link.toNode;
                if (!forward && link.toNode == current && scalarLink(link)) next = link.fromNode;
                if (next != 0 && seen.insert(next).second) stack.push_back(next);
            }
        }
        return seen;
    };

    std::unordered_set<int> forwardSet;
    std::unordered_set<int> backwardSet;
    for (const int id : adcs) {
        const std::unordered_set<int> found = reach(id, true);
        forwardSet.insert(found.begin(), found.end());
    }
    for (const int id : dacs) {
        const std::unordered_set<int> found = reach(id, false);
        backwardSet.insert(found.begin(), found.end());
    }
    std::unordered_set<int> region;
    for (const int id : forwardSet) {
        if (backwardSet.count(id)) region.insert(id);
    }
    if (region.empty()) return plan;

    bool changed = true;
    while (changed) {
        changed = false;
        const std::vector<int> current(region.begin(), region.end());
        for (const int id : current) {
            const Node *node = graph.find(id);
            if (!node) continue;
            const std::vector<PortDesc> &inputs = node->inputPorts();
            for (const Link &link : graph.links) {
                if (link.toNode != id) continue;
                if (link.toPort < 0 || link.toPort >= static_cast<int>(inputs.size())) continue;
                if (inputs[link.toPort].type != PortType::Scalar) continue;
                const Node *source = graph.find(link.fromNode);
                if (!source || !source->enabled || region.count(source->id)) continue;
                if (!isPureScalarBlock(*source)) continue;
                region.insert(source->id);
                changed = true;
            }
        }
    }

    std::unordered_set<int> downstream;
    {
        std::vector<int> stack(region.begin(), region.end());
        while (!stack.empty()) {
            const int current = stack.back();
            stack.pop_back();
            for (const Link &link : graph.links) {
                if (link.fromNode != current) continue;
                const Node *target = graph.find(link.toNode);
                if (!target || !target->enabled || region.count(link.toNode)) continue;
                if (downstream.insert(link.toNode).second) stack.push_back(link.toNode);
            }
        }
    }
    for (const int id : order) {
        const Node *node = graph.find(id);
        if (!node || !node->enabled) continue;
        if (region.count(id)) plan.region.push_back(id);
        else if (downstream.count(id)) plan.after.push_back(id);
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
    node.outputs.assign(static_cast<size_t>(channels), Value{});
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
        node.outputs[static_cast<size_t>(c)] = Value::makeScalar(value);
        node.runtimeState["value." + std::to_string(c)] = value;
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
    for (int c = 0; c < channels && c < 8; ++c) {
        node.runtimeState["value." + std::to_string(c)] = values[c];
    }
    node.outputs.assign(1, Value{});
    node.outputs[0].type = PortType::Audio;
    node.outputs[0].audio = node.audioRenderOutput;
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

    // Resolve the links once per frame; the sample loop must not search them
    // per audio sample.
    std::unordered_map<int, ChannelInput> adcInputs;
    std::unordered_map<int, std::vector<ChannelInput>> dacInputs;
    for (const int id : plan.region) {
        Node *node = graph.find(id);
        if (!node) continue;
        if (node->kind == "dsp.adc") {
            ChannelInput input;
            if (const Link *link = graph.findInputLink(id, 0)) {
                input.source = graph.find(link->fromNode);
                input.port = link->fromPort;
            }
            adcInputs[id] = input;
        } else if (node->kind == "dsp.dac") {
            std::vector<ChannelInput> inputs(static_cast<size_t>(node->inputPorts().size()));
            for (size_t port = 0; port < inputs.size(); ++port) {
                if (const Link *link = graph.findInputLink(id, static_cast<int>(port))) {
                    inputs[port].source = graph.find(link->fromNode);
                    inputs[port].port = link->fromPort;
                }
            }
            dacInputs[id] = std::move(inputs);
        }
    }

    std::vector<int> dacs;
    bool needsRender = ctx.offline;
    for (const int id : plan.region) {
        Node *node = graph.find(id);
        if (!node || node->kind != "dsp.dac") continue;
        prepareDacBuffer(*node, ctx, rate, source, sourceEnd, startFrame, endFrame);
        if (node->audioRenderWrite >= 0) needsRender = true;
        dacs.push_back(id);
    }

    for (long long frame = startFrame; needsRender && frame < endFrame; ++frame) {
        EvalContext sample = ctx;
        sample.audioRate = true;
        sample.audioSampleRate = rate;
        sample.audioTime = static_cast<double>(frame) / rate;
        sample.time = ctx.time + static_cast<double>(frame - startFrame) / rate;
        for (const int id : plan.region) {
            Node *node = graph.find(id);
            if (!node || !node->enabled) continue;
            if (node->kind == "dsp.adc") {
                sampleAdcNode(*node, sample, adcInputs[id]);
            } else if (node->kind == "dsp.dac") {
                sampleDacNode(*node, sample, dacInputs[id]);
            } else {
                evaluateFrameNode(graph, *node, sample, false);
            }
        }
    }

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
    syncDynamicChannelPorts(*this);
    syncSpectrumPresets(*this);
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
