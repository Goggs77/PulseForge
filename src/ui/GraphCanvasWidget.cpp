// Node graph editor: pan/zoom, block dragging, typed port connections.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <unordered_set>

#include "core/Registry.h"
#include "core/Sorting.h"
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
    const float rows = std::max(1.0f, std::max(static_cast<float>(node.inputPorts().size()),
                                                static_cast<float>(node.outputPorts().size())));
    // Blocks with live content reserve extra body space between the ports and
    // the footer for it.
    return headerHeight() + rows * portRowHeight() + ui::s(nodeVisualHeight(node)) + footerHeight();
}

// Fraction of the block height taken by the port rows.
float portAreaFraction(const Node &node) {
    const float rows = std::max(1.0f, std::max(static_cast<float>(node.inputPorts().size()),
                                                static_cast<float>(node.outputPorts().size())));
    return (rows * portRowHeight()) / nodeTotalHeight(node);
}

float visualFraction(const Node &node) { return ui::s(nodeVisualHeight(node)) / nodeTotalHeight(node); }

float portRows(const Node &node) {
    const size_t inputs = node.def ? node.inputPorts().size() : 0;
    const size_t outputs = node.def ? node.outputPorts().size() : 0;
    return static_cast<float>(std::max<size_t>(1, std::max(inputs, outputs)));
}

// Port geometry is expressed as a fraction of the node's box so that it scales
// with the canvas zoom exactly like the block itself.
float headerFraction(const Node &node) {
    return headerHeight() / nodeTotalHeight(node);
}

// Screen rect of a block's live content, shared by the drawing and hit testing.
Rectangle visualRect(const Node &node, const Rectangle &box) {
    if (visualFraction(node) <= 0.0f) return Rectangle{};
    return Rectangle{box.x, box.y + box.height * (headerFraction(node) + portAreaFraction(node)),
                     box.width, box.height * visualFraction(node)};
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

// ---------------------------------------------------------------------------
// Sorting: sticky notes and groups
// ---------------------------------------------------------------------------

bool isSortingNode(const Node &node) { return node.def && node.def->category == "Sorting"; }
bool isGroupNode(const Node &node) { return node.kind == "sort.group"; }

float groupHeaderHeight() { return ui::s(26.0f); }
float groupPadding(const Node &group) {
    return ui::s(std::max(6.0f, group.pfloat("padding", 26.0f)));
}
float groupSpacing(const Node &group) {
    return ui::s(std::max(4.0f, group.pfloat("spacing", 22.0f)));
}

std::vector<int> groupMembersOf(const Node &group) {
    return parseGroupMembers(group.pstr("members"));
}

// Frame of a group: the members' slots decide its size, so it grows and shrinks
// with what it owns instead of needing a manual resize.
Rectangle groupFrame(const Graph &graph, const Node &group) {
    const float padding = groupPadding(group);
    const float spacing = groupSpacing(group);
    const std::vector<GroupSlot> slots = groupLayerOrder(graph, groupMembersOf(group));
    int columns = 0;
    std::vector<float> columnHeights;
    for (const GroupSlot &slot : slots) {
        const Node *member = graph.find(slot.id);
        if (!member) continue;
        columns = std::max(columns, slot.column + 1);
        if (static_cast<int>(columnHeights.size()) < slot.column + 1) {
            columnHeights.resize(static_cast<size_t>(slot.column) + 1, 0.0f);
        }
        columnHeights[static_cast<size_t>(slot.column)] += nodeTotalHeight(*member) + spacing;
    }
    if (columns <= 0) {
        // An empty group stays a sensible drop target.
        return Rectangle{group.x, group.y, nodeWidth() * 1.7f,
                         groupHeaderHeight() + padding * 2.0f + ui::s(56.0f)};
    }
    float tallest = 0.0f;
    for (const float height : columnHeights) tallest = std::max(tallest, height - spacing);
    const float width = padding * 2.0f + static_cast<float>(columns) * nodeWidth() +
                        static_cast<float>(columns - 1) * spacing;
    const float height = groupHeaderHeight() + padding + tallest + padding;
    return Rectangle{group.x, group.y, width, height};
}

// Writes the layered arrangement into the members' positions. Columns follow
// the compacted depth, rows are top aligned and ordered by the layout keys.
void layoutGroup(Graph &graph, Node &group) {
    const std::vector<GroupSlot> slots = groupLayerOrder(graph, groupMembersOf(group));
    if (slots.empty()) return;
    const float originX = group.x + groupPadding(group);
    const float originY = group.y + groupHeaderHeight() + groupPadding(group);
    const float spacing = groupSpacing(group);
    std::vector<float> columnY;
    for (const GroupSlot &slot : slots) {
        Node *member = graph.find(slot.id);
        if (!member) continue;
        if (static_cast<int>(columnY.size()) <= slot.column) {
            columnY.resize(static_cast<size_t>(slot.column) + 1, originY);
        }
        member->x = originX + static_cast<float>(slot.column) * (nodeWidth() + spacing);
        member->y = columnY[static_cast<size_t>(slot.column)];
        columnY[static_cast<size_t>(slot.column)] += nodeTotalHeight(*member) + spacing;
    }
}

// Re-arranges every group and heals its member list (a deleted block, a nested
// group or an id claimed by an earlier group is dropped). A group whose member
// is being dragged is skipped so the drag follows the mouse.
void syncGroups(Graph &graph, int draggingNode) {
    std::unordered_set<int> claimed;
    for (Node &group : graph.nodes) {
        if (!isGroupNode(group)) continue;
        const std::vector<int> members = groupMembersOf(group);
        std::vector<int> live;
        live.reserve(members.size());
        bool changed = false;
        for (const int id : members) {
            const Node *member = graph.find(id);
            if (!member || isGroupNode(*member) || !claimed.insert(id).second) {
                changed = true;
                continue;
            }
            live.push_back(id);
        }
        if (changed) group.setText("members", formatGroupMembers(live));
        if (draggingNode > 0 &&
            std::find(live.begin(), live.end(), draggingNode) != live.end()) {
            continue;
        }
        layoutGroup(graph, group);
    }
}

// The group under a world-space point, ignoring one block (the dragged one).
int groupAtPoint(const Graph &graph, Vector2 point, int excludeId) {
    for (auto it = graph.nodes.rbegin(); it != graph.nodes.rend(); ++it) {
        const Node &node = *it;
        if (!isGroupNode(node) || node.id == excludeId) continue;
        if (CheckCollisionPointRec(point, groupFrame(graph, node))) return node.id;
    }
    return 0;
}

// Adds a dropped block to the group under it, or removes it from the group it
// left. A block belongs to at most one group and groups never nest. Returns
// true when a membership list changed.
bool updateGroupMembership(Graph &graph, int nodeId, int targetGroupId) {
    const Node *dropped = graph.find(nodeId);
    if (!dropped || isGroupNode(*dropped)) return false;
    bool changed = false;
    for (Node &group : graph.nodes) {
        if (!isGroupNode(group) || group.id == nodeId) continue;
        std::vector<int> members = groupMembersOf(group);
        const auto found = std::find(members.begin(), members.end(), nodeId);
        const bool member = found != members.end();
        const bool wanted = group.id == targetGroupId;
        if (member == wanted) continue;
        if (wanted) {
            members.push_back(nodeId);
        } else {
            members.erase(found);
        }
        group.setText("members", formatGroupMembers(members));
        changed = true;
    }
    if (changed) {
        for (Node &group : graph.nodes) {
            if (isGroupNode(group)) layoutGroup(graph, group);
        }
    }
    return changed;
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
    if (isGroupNode(node)) return groupFrame(graph, node);
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
    // A dialog or popup owns the input; without this the wheel zoomed the graph
    // while one was open.
    if (inside && !ui::inputBlocked()) {
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
    state.canvas.hoveredVisualNode = -1;
    // Groups re-arrange their members before anything looks at their positions,
    // so hit testing and drawing see the same layout.
    syncGroups(graph, state.canvas.draggingNode ? state.canvas.dragNodeId : 0);
    bool overPort = false;
    if (inside) {
        for (auto it = graph.nodes.rbegin(); it != graph.nodes.rend(); ++it) {
            Node &node = *it;
            // A group frame is only hit where no block sits; the fallback pass
            // below picks it up once every block has been tried.
            if (isGroupNode(node)) continue;
            const Rectangle world = nodeBounds(graph, node);
            const Rectangle box{worldToScreen(Vector2{world.x, world.y}).x,
                                worldToScreen(Vector2{world.x, world.y}).y, world.width * view.zoom,
                                world.height * view.zoom};
            for (size_t i = 0; i < node.inputPorts().size(); ++i) {
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
            for (size_t i = 0; i < node.outputPorts().size(); ++i) {
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
            // Blocks with an interactive pivot (Signal Filter) claim the mouse
            // over their live content before the node drag does.
            if (nodeVisualHasPivot(node)) {
                const Rectangle visual = visualRect(node, box);
                if (visual.width > 0.0f && ui::hovered(visual)) {
                    state.canvas.hoveredVisualNode = node.id;
                    break;
                }
            }
            if (ui::hovered(box)) {
                state.canvas.hoveredNode = node.id;
                break;
            }
        }
        // Nothing on top: a group frame under the cursor becomes the hit, which
        // is where clicking its header or padding selects it.
        if (!overPort && state.canvas.hoveredNode <= 0 && state.canvas.hoveredVisualNode <= 0) {
            state.canvas.hoveredNode = groupAtPoint(graph, screenToWorld(mouse), 0);
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
        const PortType type = from->outputPorts()[static_cast<size_t>(link.fromPort)].type;
        Color color = portTypeColor(type);
        const bool highlight = state.canvas.hoveredPortNode == to->id &&
                               state.canvas.hoveredPortIndex == link.toPort;
        drawBezier(fromScreen, toScreen, palette::withAlpha(color, highlight ? 0.95f : 0.6f),
                   highlight ? 3.0f : 2.0f);
    }

    // ---- draw groups (behind their members) -------------------------------
    int dropTarget = 0;
    if (state.canvas.draggingNode) {
        const Node *dragged = graph.find(state.canvas.dragNodeId);
        if (dragged && !isGroupNode(*dragged)) {
            const Rectangle draggedBox = nodeBounds(graph, *dragged);
            const Vector2 centre{draggedBox.x + draggedBox.width * 0.5f,
                                 draggedBox.y + draggedBox.height * 0.5f};
            dropTarget = groupAtPoint(graph, centre, dragged->id);
        }
    }
    for (Node &group : graph.nodes) {
        if (!isGroupNode(group)) continue;
        const Rectangle world = nodeBounds(graph, group);
        const Vector2 origin = worldToScreen(Vector2{world.x, world.y});
        const Rectangle box{origin.x, origin.y, world.width * view.zoom,
                            world.height * view.zoom};
        if (box.x > viewport.x + viewport.width || box.x + box.width < viewport.x ||
            box.y > viewport.y + viewport.height || box.y + box.height < viewport.y) {
            continue;
        }
        const SortingColours colours = sortingColours(group.enabled ? 1.0f : 0.55f);
        const float header = std::min(box.height, groupHeaderHeight() * view.zoom);
        const bool selected = group.id == state.selectedNode;
        const bool highlighted = dropTarget == group.id;
        DrawRectangleRounded(box, 0.03f, 6, colours.body);
        DrawRectangleRounded(Rectangle{box.x, box.y, box.width, header}, 0.06f, 6,
                             colours.header);
        DrawRectangleRoundedLines(box, 0.03f, 6,
                                  highlighted ? t.accent
                                              : (selected ? t.accent : colours.border));
        if (highlighted) {
            DrawRectangleRoundedLines(Rectangle{box.x - 2.0f, box.y - 2.0f, box.width + 4.0f,
                                                box.height + 4.0f},
                                      0.03f, 6, palette::withAlpha(t.accent, 0.65f));
        }
        std::vector<int> members = groupMembersOf(group);
        size_t live = 0;
        for (const int id : members) {
            if (graph.find(id)) ++live;
        }
        char title[160];
        if (live > 0) {
            std::snprintf(title, sizeof(title), "%s  -  %zu blocks",
                          group.displayTitle().c_str(), live);
        } else {
            std::snprintf(title, sizeof(title), "%s  -  drop blocks inside",
                          group.displayTitle().c_str());
        }
        ui::drawTextClipped(Rectangle{box.x + 10.0f * view.zoom, box.y,
                                      box.width - 20.0f * view.zoom, header},
                            title, 12.5f * view.zoom, colours.ink, ui::Align::Left, true);
    }

    // ---- draw nodes -------------------------------------------------------
    for (Node &node : graph.nodes) {
        // Groups are frames drawn behind the blocks above; their own box would
        // only add a second border.
        if (isGroupNode(node)) continue;
        const Rectangle world = nodeBounds(graph, node);
        const Vector2 origin = worldToScreen(Vector2{world.x, world.y});
        const Rectangle box{origin.x, origin.y, world.width * view.zoom, world.height * view.zoom};
        if (box.x > viewport.x + viewport.width || box.x + box.width < viewport.x ||
            box.y > viewport.y + viewport.height || box.y + box.height < viewport.y) {
            continue;
        }
        const bool selected = node.id == state.selectedNode;
        const bool sorting = isSortingNode(node);
        const SortingColours colours = sortingColours(node.enabled ? 1.0f : 0.55f);
        const Color accent = sorting ? colours.ink : kindAccent(node.def->category);
        DrawRectangleRounded(box, 0.06f, 6,
                             sorting ? colours.body
                                     : palette::withAlpha(t.panelRaised,
                                                          node.enabled ? 0.96f : 0.6f));
        const float headerHeight = box.height * headerFraction(node);
        DrawRectangleRounded(Rectangle{box.x, box.y, box.width, headerHeight}, 0.12f, 6,
                             sorting ? colours.header
                                     : palette::withAlpha(accent, node.enabled ? 0.85f : 0.4f));
        DrawRectangleRoundedLines(box, 0.06f, 6,
                                  selected ? t.accent
                                           : (sorting ? colours.border
                                                      : palette::withAlpha(t.border, 0.95f)));
        if (selected) {
            DrawRectangleRoundedLines(Rectangle{box.x - 2.0f, box.y - 2.0f, box.width + 4.0f,
                                                box.height + 4.0f},
                                      0.06f, 6, t.accent);
        }
        ui::drawText(Rectangle{box.x + 8.0f * view.zoom, box.y, box.width - 16.0f, headerHeight},
                     node.displayTitle().c_str(), 13.0f * view.zoom,
                     sorting ? colours.ink : ui::readableOn(accent), ui::Align::Left, true);
        if (!node.enabled) {
            ui::drawText(Rectangle{box.x, box.y, box.width - 6.0f * view.zoom, headerHeight},
                         "off", 11.0f * view.zoom,
                         palette::withAlpha(sorting ? colours.ink : ui::readableOn(accent), 0.85f),
                         ui::Align::Right);
        }

        // inputs
        for (size_t i = 0; i < node.inputPorts().size(); ++i) {
            const PortDesc &port = node.inputPorts()[i];
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
        for (size_t i = 0; i < node.outputPorts().size(); ++i) {
            const PortDesc &port = node.outputPorts()[i];
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
            drawNodeVisual(state, node, visualRect(node, box), viewport, view.zoom);
            // drawNodeVisual scissorstamps its own body and ends the scissor
            // when it is done, so the canvas clip has to be re-established or
            // the next block would be free to draw over other panels.
            BeginScissorMode(static_cast<int>(viewport.x), static_cast<int>(viewport.y),
                             static_cast<int>(viewport.width),
                             static_cast<int>(viewport.height));
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
        // Sorting blocks carry nothing to evaluate, so their footer stays empty
        // instead of reporting the category and 0.00 ms.
        if (!sorting) {
            char info[96];
            if (!node.status.empty()) {
                std::snprintf(info, sizeof(info), "%s", node.status.c_str());
                ui::drawTextClipped(footer, info, 10.0f * view.zoom, t.danger);
            } else {
                std::snprintf(info, sizeof(info), "%s  %.2f ms", node.def->category.c_str(),
                              node.lastEvalMs);
                ui::drawTextClipped(footer, info, 10.0f * view.zoom,
                                    palette::withAlpha(t.textDim, 0.9f));
            }
        }
        (void)rows;
    }

    // ---- drag / connect ---------------------------------------------------
    if (inside && !ui::inputBlocked()) {
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !state.canvas.panning) {
            if (overPort && state.canvas.hoveredPortNode > 0 && !state.canvas.hoveredPortIsInput) {
                state.canvas.draggingLink = true;
                state.canvas.linkFromNode = state.canvas.hoveredPortNode;
                state.canvas.linkFromPort = state.canvas.hoveredPortIndex;
            } else if (state.canvas.hoveredVisualNode > 0) {
                // Pivot drag inside the block: move the parameter, not the block.
                Node *node = graph.find(state.canvas.hoveredVisualNode);
                if (node) {
                    selectNode(state, node->id);
                    state.canvas.visualDragNode = node->id;
                    const Rectangle world = nodeBounds(graph, *node);
                    const Rectangle box{worldToScreen(Vector2{world.x, world.y}).x,
                                        worldToScreen(Vector2{world.x, world.y}).y,
                                        world.width * view.zoom, world.height * view.zoom};
                    nodeVisualPivotDrag(state, *node, visualRect(*node, box), mouse);
                }
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
            // A dropped block joins the group under it, or leaves the group it
            // was in; the group then re-arranges its members.
            if (state.canvas.draggingNode) {
                if (Node *node = graph.find(state.canvas.dragNodeId)) {
                    if (!isGroupNode(*node)) {
                        const Rectangle box = nodeBounds(graph, *node);
                        const Vector2 centre{box.x + box.width * 0.5f,
                                             box.y + box.height * 0.5f};
                        if (updateGroupMembership(graph, node->id,
                                                  groupAtPoint(graph, centre, node->id))) {
                            state.project.dirty = true;
                        }
                    }
                }
            }
            state.canvas.draggingNode = false;
            state.canvas.visualDragNode = -1;
        }
        if (state.canvas.visualDragNode > 0) {
            Node *node = graph.find(state.canvas.visualDragNode);
            if (node && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
                const Rectangle world = nodeBounds(graph, *node);
                const Rectangle box{worldToScreen(Vector2{world.x, world.y}).x,
                                    worldToScreen(Vector2{world.x, world.y}).y,
                                    world.width * view.zoom, world.height * view.zoom};
                nodeVisualPivotDrag(state, *node, visualRect(*node, box), mouse);
            } else {
                state.canvas.visualDragNode = -1;
            }
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
            const PortType type = from->outputPorts()[static_cast<size_t>(state.canvas.linkFromPort)].type;
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
            const std::vector<PortDesc> &ports = isInput ? node->inputPorts() : node->outputPorts();
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
    //Keep new nodes in view.
    const float x = - state.project.view.panX + 250.0f + static_cast<float>(count % 31) * 4.0f;
    const float y = - state.project.view.panY + 160.0f + static_cast<float>(count % 11) * ui::s(24.0f);
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
