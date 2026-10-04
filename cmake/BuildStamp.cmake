# Writes the header the startup banner prints its build time from. Run by the
# "build_stamp" target on every build, so the stamp is the time the binary was
# really produced and not just when main.cpp last changed.

if (NOT DEFINED PF_STAMP_OUT)
    message(FATAL_ERROR "PF_STAMP_OUT is not set")
endif ()

string(TIMESTAMP stamp "%Y-%m-%d %H:%M:%S")
file(WRITE "${PF_STAMP_OUT}" "#pragma once\n#define PF_BUILD_STAMP \"${stamp}\"\n")
