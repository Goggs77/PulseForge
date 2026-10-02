// GLSL fragment shader loading/compilation with a shared preamble.
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "raylib.h"

namespace pf {

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

    static const char *vertexSource();
    static std::string preamble();
    static std::vector<std::string> builtinNames();
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
