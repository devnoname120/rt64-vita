#pragma once
#include "rt64_fast_state.h"
#include "gbi/rt64_gbi.h"

namespace RT64 {
    struct Interpreter {
        State *state = nullptr;
        GBIManager gbiManager;
        GBI *hleGBI = nullptr;
        uint8_t extendedOpCode = 0;
        void setup(State *value);
        void loadUCodeGBI(uint32_t textAddress, uint32_t dataAddress, bool resetFromTask);
        // Throws on unsupported commands or invalid RDRAM rather than continuing
        // with corrupted state. The command budget bounds malformed cyclic lists.
        void processDisplayLists(uint32_t address, DisplayList *start, size_t budget = 1000000);
    };
}
