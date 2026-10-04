#include <cstdio>

#include "ui/App.h"

// The build step regenerates build_stamp.h with the time the binary was
// produced; a build system that does not (an IDE project, a quick g++ run)
// falls back to when this file was compiled.
#if defined(__has_include)
#if __has_include("build_stamp.h")
#include "build_stamp.h"
#endif
#endif
#ifndef PF_BUILD_STAMP
#define PF_BUILD_STAMP __DATE__ " " __TIME__
#endif

int main(int argc, char **argv) {
    printf("---------------------------------------Pulse Forge v0.0.2---------------------------------------\n");
    printf("A programmable, lightweight, fast, audio-reactive video synthesiser, licensed under BSD 3-Clause\n");
    printf("Built %s\n", PF_BUILD_STAMP);
    printf("------------------------------------------------------------------------------------------------\n");
    return pf::runApp(argc, argv);
}
