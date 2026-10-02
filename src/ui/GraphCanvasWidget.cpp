// Node graph editor: pan/zoom, block dragging, typed port connections.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "core/Registry.h"
#include "render/Palette.h"
#include "ui/App.h"
#include "ui/NodeVisuals.h"

namespace pf {

namespace {

// Block geometry follows the GUI scaling factor so labels always fit their
// boxes; the canvas zoom then scales the whole layout on top of that.
float nodeWidth() { return ui::s(192.0f); }
float headerHeight() { return ui::s(26.0f); }
float portRowHeight() { return ui::s(18.0f); }
float portDotRadius() { return ui::s(6.0f); }
float footerHeight() { return ui::s(18.0f); }

float nodeTotalHeight(const Node &node) {
    const float rows = std::max(1.0f, std::max(static_cast<float>(node.def->inputs.size()),
                                                static_cast<float>(node.def->outputs.size())));
    // Blocks with live content reserve extra body space between the ports and
    // the footer for it.
    return headerHeight() + rows * portRowHeight() + ui::s(nodeVisualHeight(node)) + footerHeight();
}

// Fraction of the block height taken by the port rows.
float portAreaFraction(const Node &node) {
    const float rows = std::max(1.0f, std::max(static_cast<float>(node.def->inputs.size()),
                                                static_cast<float>(node.def->outputs.size())));
    return (rows * portRowHeight()) / nodeTotalHeight(node);
}

float visualFraction(const Node &node) { return ui::s(nodeVisualHeight(node)) / nodeTotalHeight(node); }

float portRows(const Node &node) {
    const size_t inputs = node.def ? node.def->inputs.size() : 0;
    const size_t outputs = node.def ? node.def->outputs.size() : 0;
    return static_cast<float>(std::max<size_t>(1, std::max(inputs, outputs)));
}

// Port geometry is expressed as a fraction of the node's box so that it scales
// with the canvas zoom exactly like the block itself.
float headerFraction(const Node &node) {
    return headerHeight() / nodeTotalHeight(node);
}

float rowFraction(const Node &node) {
    return portRowHeight() / nodeTotalHeight(node);
}

float footerFraction(const Node &node) {
    return footerHeight() / nodeTotalHeight(node);
}

Vector2 inputPortPosition(Rectangle box, const Node &node, int index) {
    const float y = box.y + box.height * (headerFraction(node) +
                                          rowFraction(node) * (static_cast<float>(index) + 0.5f));
    return Vector2{box.x, y};
}

Vector2 outputPortPosition(Rectangle box, const Node &node, int index) {
    // Outputs are aligned to the bottom of the port area, so the block's live
    // content below them does not push them around.
    const float y = box.y + box.height * (headerFraction(node) + portAreaFraction(node) -
                                          rowFraction(node) * (static_cast<float>(index) + 0.5f));
    return Vector2{box.x + box.width, y};
}

// Radius of a port dot as a fraction of the block width.
float portRadius(const Rectangle &box) { return box.width * (portDotRadius() / nodeWidth()); }

void drawBezier(Vector2 from, Vector2 to, Color color, float thickness) {
    const float handle = std::clamp(std::fabs(to.x - from.x) * 0.5f, 30.0f, 160.0f);
    Vector2 previous = from;
    const int segments = 26;
    for (int i = 1; i <= segments; ++i) {
        const float t = static_cast<float>(i) / segments;
        const float mt = 1.0f - t;
        const Vector2 p0 = from;
        const Vector2 p1{from.x + handle, from.y};
        const Vector2 p2{to.x - handle, to.y};
        const Vector2 p3 = to;
        const Vector2 point{
            mt * mt * mt * p0.x + 3.0f * mt * mt * t * p1.x + 3.0f * mt * t * t * p2.x +
                t * t * t * p3.x,
            mt * mt * mt * p0.y + 3.0f * mt * mt * t * p1.y + 3.0f * mt * t * t * p2.y +
                t * t * t * p3.y};
        DrawLineEx(previous, point, thickness, color);
        previous = point;
    }
}

Color kindAccent(const std::string &category) {
    if (category == "Source") return Color{236, 176, 78, 255};
    if (category == "DSP") return Color{124, 210, 138, 255};
    if (category == "Timing") return Color{120, 196, 255, 255};
    if (category == "Modulation") return Color{196, 148, 255, 255};
    if (category == "Render") return Color{255, 132, 190, 255};
    return Color{150, 156, 176, 255};
}

// Symbol shown in the empty middle of a block, so the operation is readable
// without opening the inspector.
std::string nodeBadge(const Node &node) {
    if (node.kind == "math.arithmetic") {
        static const char *symbols[] = {"+", "\u2212", "\u00D7", "\u00F7",
                                        "\u2264", "\u2265", "%"};
        const int op = std::clamp(node.pint("op", 0), 0, 6);
        return symbols[op];
    }
    if (node.kind == "mod.automation") return node.pbool("bipolar", false) ? "\u00B1" : "0..1";
    if (node.kind == "math.matrix" || node.kind == "math.determinant") {
        static const char *sizes[] = {"2\u00D72", "3\u00D73", "4\u00D74"};
        return sizes[std::clamp(node.pint("size", 1), 0, 2)];
    }
    return std::string();
}

}  // namespace

Rectangle nodeBounds(const Graph &graph, const Node &node) {
    (void)graph;
    return Rectangle{node.x, node.y, nodeWidth(), nodeTotalHeight(node)};
}

void drawGraphCanvas(UiState &state, Rectangle bounds) {
    const ui::Theme &t = ui::theme();
    Graph &graph = state.project.graph;
    ViewState &view = state.project.view;

    ui::panel(bounds);
    ui::beginScroll(Rectangle{bounds.x + 1.0f, bounds.y + 26.0f, bounds.width - 2.0f,
                              bounds.height - 27.0f});

    const Rectangle viewport{bounds.x + 1.0f, bounds.y + 26.0f, bounds.width - 2.0f,
                             bounds.height - 27.0f};
    const Vector2 mouse = GetMousePosition();
    const bool inside = ui::hovered(viewport);

    // ---- background grid -------------------------------------------------
    const float grid = 32.0f * view.zoom;
    if (grid > 6.0f) {
        const float gridOffsetX = std::fmod(view.panX, grid);
        const float gridOffsetY = std::fmod(view.panY, grid);
        for (float x = viewport.x + gridOffsetX; x < viewport.x + viewport.width;
             x += grid) {
            DrawLine(static_cast<int>(x), static_cast<int>(viewport.y), static_cast<int>(x),
                     static_cast<int>(viewport.y + viewport.height),
                     palette::withAlpha(t.border, 0.28f));
        }
        for (float y = viewport.y + gridOffsetY; y < viewport.y + viewport.height;
             y += grid) {
            DrawLine(static_cast<int>(viewport.x), static_cast<int>(y),
                     static_cast<int>(viewport.x + viewport.width), static_cast<int>(y),
                     palette::withAlpha(t.border, 0.28f));
        }
    }
    DrawLine(static_cast<int>(viewport.x), static_cast<int>(viewport.y),
             static_cast<int>(viewport.x + viewport.width), static_cast<int>(viewport.y),
             palette::withAlpha(t.border, 0.9f));

    auto worldToScreen = [&](Vector2 p) {
        return Vector2{viewport.x + (p.x * view.zoom) + view.panX,
                       viewport.y + (p.y * view.zoom) + view.panY};
    };
    auto screenToWorld = [&](Vector2 p) {
        return Vector2{(p.x - viewport.x - view.panX) / view.zoom,
                       (p.y - viewport.y - view.panY) / view.zoom};
    };

    // ---- interaction: pan and zoom ---------------------------------------
    if (inside && !ui::popupOpen()) {
        const float wheel = GetMouseWheelMove();
        if (wheel != 0.0f && !IsKeyDown(KEY_LEFT_CONTROL)) {
            const Vector2 before = screenToWorld(mouse);
            view.zoom = std::clamp(view.zoom * (1.0f + wheel * 0.12f), 0.2f, 3.0f);
            const Vector2 after = screenToWorld(mouse);
            view.panX += (after.x - before.x) * view.zoom;
            view.panY += (after.y - before.y) * view.zoom;
        }
        if (IsMouseButtonPressed(MOUSE_BUTTON_MIDDLE) ||
            (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && IsKeyDown(KEY_SPACE))) {
            state.canvas.panning = true;
        }
    }
    if (state.canvas.panning) {
        if (IsMouseButtonDown(MOUSE_BUTTON_MIDDLE) ||
            IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
            const Vector2 delta = GetMouseDelta();
            view.panX += delta.x;
            view.panY += delta.y;
        } else {
            state.canvas.panning = false;
        }
    }

    // ---- hit testing ------------------------------------------------------
    state.canvas.hoveredNode = -1;
    state.canvas.hoveredPortNode = -1;
    bool overPort = false;
    if (inside) {
        for (auto it = graph.nodes.rbegin(); it != graph.nodes.rend(); ++it) {
            Node &node = *it;
            const Rectangle world = nodeBounds(graph, node);
            const Rectangle box{worldToScreen(Vector2{world.x, world.y}).x,
                                worldToScreen(Vector2{world.x, world.y}).y, world.width * view.zoom,
                                world.height * view.zoom};
            for (size_t i = 0; i < node.def->inputs.size(); ++i) {
                const Vector2 position = inputPortPosition(box, node, static_cast<int>(i));
                if (ui::distance(position, mouse) < portRadius(box) + 4.0f) {
                    state.canvas.hoveredPortNode = node.id;
                    state.canvas.hoveredPortIndex = static_cast<int>(i);
                    state.canvas.hoveredPortIsInput = true;
                    overPort = true;
                    break;
                }
            }
            if (overPort) break;
            for (size_t i = 0; i < node.def->outputs.size(); ++i) {
                const Vector2 position = outputPortPosition(box, node, static_cast<int>(i));
                if (ui::distance(position, mouse) < portRadius(box) + 4.0f) {
                    state.canvas.hoveredPortNode = node.id;
                    state.canvas.hoveredPortIndex = static_cast<int>(i);
                    state.canvas.hoveredPortIsInput = false;
                    overPort = true;
                    break;
                }
            }
            if (overPort) break;
            if (ui::hovered(box)) {
                state.canvas.hoveredNode = node.id;
                break;
            }
        }
    }

    // ---- draw links -------------------------------------------------------
    for (const Link &link : graph.links) {
        const Node *from = graph.find(link.fromNode);
        const Node *to = graph.find(link.toNode);
        if (!from || !to) continue;
        const Rectangle fromBox = nodeBounds(graph, *from);
        const Rectangle toBox = nodeBounds(graph, *to);
        const Vector2 fromScreen = outputPortPosition(
            Rectangle{worldToScreen(Vector2{fromBox.x, fromBox.y}).x,
                      worldToScreen(Vector2{fromBox.x, fromBox.y}).y, fromBox.width * view.zoom,
                      fromBox.height * view.zoom},
            *from, link.fromPort);
        const Vector2 toScreen = inputPortPosition(
            Rectangle{worldToScreen(Vector2{toBox.x, toBox.y}).x,
                      worldToScreen(Vector2{toBox.x, toBox.y}).y, toBox.width * view.zoom,
                      toBox.height * view.zoom},
            *to, link.toPort);
        const PortType type = from->def->outputs[static_cast<size_t>(link.fromPort)].type;
        Color color = portTypeColor(type);
        const bool highlight = state.canvas.hoveredPortNode == to->id &&
                               state.canvas.hoveredPortIndex == link.toPort;
        drawBezier(fromScreen, toScreen, palette::withAlpha(color, highlight ? 0.95f : 0.6f),
                   highlight ? 3.0f : 2.0f);
    }

    // ---- draw nodes -------------------------------------------------------
    for (Node &node : graph.nodes) {
        const Rectangle world = nodeBounds(graph, node);
        const Vector2 origin = worldToScreen(Vector2{world.x, world.y});
        const Rectangle box{origin.x, origin.y, world.width * view.zoom, world.height * view.zoom};
        if (box.x > viewport.x + viewport.width || box.x + box.width < viewport.x ||
            box.y > viewport.y + viewport.height || box.y + box.height < viewport.y) {
            continue;
        }
        const bool selected = node.id == state.selectedNode;
        const Color accent = kindAccent(node.def->category);
        DrawRectangleRounded(box, 0.06f, 6, palette::withAlpha(t.panelRaised, node.enabled ? 0.96f : 0.6f));
        const float headerHeight = box.height * headerFraction(node);
        DrawRectangleRounded(Rectangle{box.x, box.y, box.width, headerHeight}, 0.12f, 6,
                             palette::withAlpha(accent, node.enabled ? 0.85f : 0.4f));
        DrawRectangleRoundedLines(box, 0.06f, 6,
                                  selected ? t.accent : palette::withAlpha(t.border, 0.95f));
        if (selected) {
            DrawRectangleRoundedLines(Rectangle{box.x - 2.0f, box.y - 2.0f, box.width + 4.0f,
                                                box.height + 4.0f},
                                      0.06f, 6, t.accent);
        }
        ui::drawText(Rectangle{box.x + 8.0f * view.zoom, box.y, box.width - 16.0f, headerHeight},
                     node.displayTitle().c_str(), 13.0f * view.zoom, ui::readableOn(accent),
                     ui::Align::Left, true);
        if (!node.enabled) {
            ui::drawText(Rectangle{box.x, box.y, box.width - 6.0f * view.zoom, headerHeight},
                         "off", 11.0f * view.zoom, palette::withAlpha(ui::readableOn(accent), 0.85f),
                         ui::Align::Right);
        }

        // inputs
        for (size_t i = 0; i < node.def->inputs.size(); ++i) {
            const PortDesc &port = node.def->inputs[i];
            const Vector2 position = inputPortPosition(box, node, static_cast<int>(i));
            const bool connected = graph.findInputLink(node.id, static_cast<int>(i)) != nullptr;
            DrawCircleV(position, portRadius(box),
                        connected ? portTypeColor(port.type) : palette::modulate(t.panelAlt, 0.9f));
            DrawCircleLinesV(position, portRadius(box), portTypeColor(port.type));
            if (view.zoom > 0.55f) {
                ui::drawTextClipped(
                    Rectangle{position.x + 10.0f * view.zoom, position.y - portRowHeight() * 0.5f * view.zoom,
                              box.width * 0.55f, portRowHeight() * view.zoom},
                    port.name.c_str(), 10.0f * view.zoom, t.text);
            }
        }
        // outputs (drawn from the bottom up so the first output is highest)
        const float rows = portRows(node);
        for (size_t i = 0; i < node.def->outputs.size(); ++i) {
            const PortDesc &port = node.def->outputs[i];
            const Vector2 position = outputPortPosition(box, node, static_cast<int>(i));
            const bool connected = [&]() {
                for (const Link &link : graph.links) {
                    if (link.fromNode == node.id && link.fromPort == static_cast<int>(i)) return true;
                }
                return false;
            }();
            DrawCircleV(position, portRadius(box),
                        connected ? portTypeColor(port.type) : palette::modulate(t.panelAlt, 0.9f));
            DrawCircleLinesV(position, portRadius(box), portTypeColor(port.type));
            if (view.zoom > 0.55f) {
                ui::drawTextClipped(
                    Rectangle{position.x - 10.0f * view.zoom - box.width * 0.5f,
                              position.y - portRowHeight() * 0.5f * view.zoom, box.width * 0.5f,
                              portRowHeight() * view.zoom},
                    port.name.c_str(), 10.0f * view.zoom, t.text,
                    ui::Align::Right);
            }
        }
        // footer: evaluation time
        const float footerHeight = box.height * footerFraction(node);
        const Rectangle footer{box.x + 6.0f * view.zoom, box.y + box.height - footerHeight,
                               box.width - 12.0f * view.zoom, footerHeight};
        // Live content (spectrum, level bar, value/time curve, waveform).
        if (visualFraction(node) > 0.0f) {
            const Rectangle visual{box.x,
                                   box.y + box.height * (headerFraction(node) + portAreaFraction(node)),
                                   box.width, box.height * visualFraction(node)};
            drawNodeVisual(state, node, visual, view.zoom);
        }
        // Mid-height badge with the operator or range; blocks with live content
        // already show what they are doing.
        const std::string badge = visualFraction(node) > 0.0f ? std::string() : nodeBadge(node);
        if (!badge.empty() && view.zoom > 0.5f) {
            const float badgeSize = std::min(18.0f * view.zoom, box.height * 0.35f);
            const Rectangle badgeRect{box.x, box.y + box.height * 0.30f, box.width,
                                      box.height * 0.36f};
            ui::drawText(badgeRect, badge.c_str(), badgeSize,
                         palette::withAlpha(t.text, 0.95f), ui::Align::Center, true);
        }
        char info[96];
        if (!node.status.empty()) {
            std::snprintf(info, sizeof(info), "%s", node.status.c_str());
            ui::drawTextClipped(footer, info, 10.0f * view.zoom, t.danger);
        } else {
            std::snprintf(info, sizeof(info), "%s  %.2f ms", node.def->category.c_str(),
                          node.lastEvalMs);
            ui::drawTextClipped(footer, info, 10.0f * view.zoom, palette::withAlpha(t.textDim, 0.9f));
        }
        (void)rows;
    }

    // ---- drag / connect ---------------------------------------------------
    if (inside && !ui::popupOpen()) {
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !state.canvas.panning) {
            if (overPort && state.canvas.hoveredPortNode > 0 && !state.canvas.hoveredPortIsInput) {
                state.canvas.draggingLink = true;
                state.canvas.linkFromNode = state.canvas.hoveredPortNode;
                state.canvas.linkFromPort = state.canvas.hoveredPortIndex;
            } else if (state.canvas.hoveredNode > 0) {
                selectNode(state, state.canvas.hoveredNode);
                Node *node = graph.find(state.canvas.hoveredNode);
                if (node) {
                    const Vector2 world = screenToWorld(mouse);
                    state.canvas.draggingNode = true;
                    state.canvas.dragNodeId = node->id;
                    state.canvas.dragOffset = Vector2{world.x - node->x, world.y - node->y};
                }
            } else {
                selectNode(state, -1);
            }
        }
        if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
            if (state.canvas.draggingLink) {
                if (state.canvas.hoveredPortNode > 0 && state.canvas.hoveredPortIsInput) {
                    std::string why;
                    if (!graph.connect(state.canvas.linkFromNode, state.canvas.linkFromPort,
                                       state.canvas.hoveredPortNode, state.canvas.hoveredPortIndex,
                                       &why)) {
                        setStatus(state, why, true);
                    } else {
                        state.project.dirty = true;
                    }
                }
                state.canvas.draggingLink = false;
            }
            state.canvas.draggingNode = false;
        }
        if (state.canvas.draggingNode) {
            Node *node = graph.find(state.canvas.dragNodeId);
            if (node && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
                const Vector2 world = screenToWorld(mouse);
                node->x = world.x - state.canvas.dragOffset.x;
                node->y = world.y - state.canvas.dragOffset.y;
                state.project.dirty = true;
            } else {
                state.canvas.draggingNode = false;
            }
        }
        // right click on a link removes it; right click on an input disconnects
        if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
            if (state.canvas.hoveredPortNode > 0 && state.canvas.hoveredPortIsInput) {
                graph.disconnectInput(state.canvas.hoveredPortNode, state.canvas.hoveredPortIndex);
                state.project.dirty = true;
            } else if (state.canvas.hoveredNode > 0) {
                selectNode(state, state.canvas.hoveredNode);
            }
        }
    }

    if (state.canvas.draggingLink) {
        const Node *from = graph.find(state.canvas.linkFromNode);
        if (from) {
            const Rectangle box = nodeBounds(graph, *from);
            const Vector2 origin = worldToScreen(Vector2{box.x, box.y});
            const Rectangle screenBox{origin.x, origin.y, box.width * view.zoom, box.height * view.zoom};
            const Vector2 start = outputPortPosition(screenBox, *from, state.canvas.linkFromPort);
            const PortType type = from->def->outputs[static_cast<size_t>(state.canvas.linkFromPort)].type;
            drawBezier(start, mouse, palette::withAlpha(portTypeColor(type), 0.9f), 2.5f);
        }
    }

    ui::endScroll();

    // ---- header / hints ---------------------------------------------------
    char header[192];
    std::snprintf(header, sizeof(header), "Pipeline  -  %d blocks, %d connections  (%.0f%%)",
                  graph.nodeCount(), static_cast<int>(graph.links.size()), view.zoom * 100.0f);
    ui::drawText(Rectangle{bounds.x + 10.0f, bounds.y, bounds.width - 20.0f, 26.0f}, header, 14.0f,
                 t.text, ui::Align::Left);

    if (graph.nodeCount() == 0) {
        ui::drawText(viewport,
                     "Add blocks from the palette on the left", 16.0f,
                     palette::withAlpha(t.textDim, 0.9f), ui::Align::Center);
    }

    if (state.canvas.hoveredPortNode > 0 && !state.canvas.draggingLink) {
        const Node *node = graph.find(state.canvas.hoveredPortNode);
        if (node) {
            const bool isInput = state.canvas.hoveredPortIsInput;
            const std::vector<PortDesc> &ports = isInput ? node->def->inputs : node->def->outputs;
            const int index = state.canvas.hoveredPortIndex;
            if (index >= 0 && index < static_cast<int>(ports.size())) {
                char text[160];
                std::snprintf(text, sizeof(text), "%s : %s", ports[static_cast<size_t>(index)].name.c_str(),
                              portTypeName(ports[static_cast<size_t>(index)].type));
                ui::drawText(Rectangle{mouse.x + 14.0f, mouse.y - 6.0f, 240.0f, 20.0f}, text, 12.0f,
                             t.text);
            }
        }
    }
}

void deleteSelectedNode(UiState &state) {
    if (state.selectedNode <= 0) return;
    state.project.graph.removeNode(state.selectedNode);
    selectNode(state, -1);
    state.project.dirty = true;
    setStatus(state, "Block removed");
}

void addNodeFromKind(UiState &state, const std::string &kind) {
    const ui::Theme &t = ui::theme();
    (void)t;
    // Place the new block near the centre of the current view with a small
    // offset so consecutive additions do not overlap exactly.
    const int count = state.project.graph.nodeCount();
    const float x = 80.0f + static_cast<float>(count % 5) * 40.0f;
    const float y = 80.0f + static_cast<float>(count % 7) * ui::s(90.0f);
    Node *node = state.project.graph.addNode(kind, x, y);
    if (!node) {
        setStatus(state, "Unknown block kind: " + kind, true);
        return;
    }
    selectNode(state, node->id);
    state.project.dirty = true;
    setStatus(state, "Added " + node->displayTitle());
}

}  // namespace pf
