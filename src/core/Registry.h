// Built-in block catalogue.
#pragma once

#include <string>
#include <vector>

#include "core/Node.h"

namespace pf {

class Registry {
public:
    static Registry &instance();

    const NodeDef *find(const std::string &kind) const;
    const std::vector<NodeDef> &definitions() const { return definitions_; }
    std::vector<const NodeDef *> byCategory(const std::string &category) const;
    std::vector<std::string> categories() const;

private:
    Registry();
    void add(NodeDef def);
    void registerBuiltins();

    std::vector<NodeDef> definitions_;
};

}  // namespace pf
