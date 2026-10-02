// GLSL fragment shader loading/compilation with a shared preamble.
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "core/Port.h"
#include "raylib.h"

namespace pf {

// One PulseForge input a shader asks for, discovered by scanning its source.
struct ShaderInput {
    std::string name;     // port label, e.g. "uUser[3]" or "uColorA"
    std::string uniform;  // uniform the value is written to
    PortType type = PortType::Scalar;
};

// Shaders are looked up either as a file path (absolute, project-relative or
// searched in the asset directories) or as "builtin:<name>".
class ShaderLibrary {
public:
    ~ShaderLibrary();

    void reset();

    // Returns a compiled shader or nullptr, filling `error` on failure.
    Shader *get(const std::string &reference, std::string *error = nullptr);
    Shader *builtin(const std::string &name);

    // Recompiles shaders whose file changed on disk (used by the editor).
    int reloadChanged();

    void setSearchPaths(const std::vector<std::string> &paths);
    const std::vector<std::string> &searchPaths() const { return searchPaths_; }
    std::string resolve(const std::string &reference) const;

    const std::string &lastError() const { return lastError_; }

    // Reads a shader's source (file or built-in body) so the ports it needs can
    // be derived from what it actually uses instead of a fixed stub list.
    bool readSource(const std::string &reference, std::string *source,
                    std::string *error = nullptr) const;
    bool describeInputs(const std::string &reference, std::vector<ShaderInput> *inputs,
                        std::string *error = nullptr) const;

    static const char *vertexSource();
    static std::string preamble();
    static std::vector<std::string> builtinNames();
    // Visual effects only: the composite helpers (blend, postfx) are used
    // internally by their own blocks and are not offered as presets.
    static std::vector<std::string> effectNames();
    // Scans GLSL body text for the PulseForge uniforms it references.
    static std::vector<ShaderInput> scanInputs(const std::string &source);
    static bool isBuiltinReference(const std::string &reference);
    static std::string builtinNameFromReference(const std::string &reference);

private:
    struct Entry {
        Shader shader{};
        std::string resolvedPath;
        long long mtime = 0;
        bool valid = false;
        std::string error;
    };

    static bool compile(const std::string &reference, const std::string &source, Entry &entry);
    static long long fileMtime(const std::string &path);
    static unsigned long long hashString(const std::string &text);

    std::unordered_map<std::string, Entry> entries_;
    std::vector<std::string> searchPaths_;
    std::string lastError_;
};

}  // namespace pf
