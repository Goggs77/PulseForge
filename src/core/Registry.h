// Built-in block catalogue.
#pragma once

#include <string>
#include <vector>

#include "core/Node.h"

namespace pf {

class Registry {
public:
    static Registry &instance();

    // Looks up a block kind. Names used by older projects are mapped to their
    // replacement ("shader.pass" -> "render.spectrum").
    const NodeDef *find(const std::string &kind) const;
    // The kind a saved project should be migrated to (empty when unknown).
    static std::string modernKindFor(const std::string &kind);
    const std::vector<NodeDef> &definitions() const { return definitions_; }
    std::vector<const NodeDef *> byCategory(const std::string &category) const;
    std::vector<std::string> categories() const;
    // Ports of a Shader block follow the uniforms its .glsl file uses.
    static bool applyShaderPorts(Node &node, const class ShaderLibrary &shaders,
                                 std::string *error = nullptr);

private:
    Registry();
    void add(NodeDef def);
    void registerBuiltins();

    std::vector<NodeDef> definitions_;
};

}  // namespace pf
