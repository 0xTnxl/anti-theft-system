// Pulls in the per-pair secrets, with a clear error if they were never generated.
#pragma once
#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "include/secrets.h is missing. Run:  python3 tools/gen_keys.py"
#endif
