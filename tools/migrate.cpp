// PulseForge project upgrader.
//
//   pf_migrate <project.pforge> [...]
//
// Loads each project through the current migrations (the Audio Output terminal,
// renamed blocks) and rewrites the file when the stored format differs. The
// previous revision is kept next to it as <name>.pforge.bak.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "core/Project.h"
#include "render/ShaderLibrary.h"

using namespace pf;

namespace {

std::string readBytes(const std::string &path) {
    std::ifstream file(path.c_str(), std::ios::binary);
    if (!file.good()) return std::string();
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        std::printf("usage: pf_migrate <project.pforge> [...]\n"
                    "Rewrites project files in the current format; keeps a .bak copy.\n");
        return 2;
    }

    int failures = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string path = argv[i];
        const std::string directory = Project::directoryOf(path);
        Project project;
        ShaderLibrary shaders;
        shaders.setSearchPaths({directory + "/assets/shaders", directory + "/assets", directory,
                                "assets/shaders", "assets", "."});
        std::string error;
        if (!project.load(path, &error, &shaders)) {
            std::printf("FAIL %s: %s\n", path.c_str(), error.c_str());
            ++failures;
            continue;
        }

        const std::string before = readBytes(path);
        const std::string temp = path + ".migrate.tmp";
        if (!project.save(temp, &error)) {
            std::printf("FAIL %s: %s\n", path.c_str(), error.c_str());
            ++failures;
            continue;
        }
        const std::string after = readBytes(temp);
        if (!after.empty() && after == before) {
            std::error_code ignored;
            std::filesystem::remove(temp, ignored);
            std::printf("OK   %s (already current)\n", path.c_str());
            continue;
        }

        // Keep the oldest revision as the backup so repeated upgrades cannot
        // overwrite the only copy of the pre-migration file.
        const std::string backup = path + ".bak";
        std::error_code existsError;
        if (!std::filesystem::exists(backup, existsError)) {
            std::error_code copyError;
            std::filesystem::copy_file(path, backup, std::filesystem::copy_options::none,
                                       copyError);
            if (copyError) {
                std::printf("FAIL %s: could not write the backup: %s\n", path.c_str(),
                            copyError.message().c_str());
                ++failures;
                continue;
            }
        }
        std::error_code removeError;
        std::filesystem::remove(path, removeError);
        std::error_code renameError;
        std::filesystem::rename(temp, path, renameError);
        if (renameError) {
            std::printf("FAIL %s: %s\n", path.c_str(), renameError.message().c_str());
            ++failures;
            continue;
        }
        std::printf("OK   %s (upgraded, previous revision in %s)\n", path.c_str(),
                    backup.c_str());
    }
    return failures == 0 ? 0 : 1;
}
