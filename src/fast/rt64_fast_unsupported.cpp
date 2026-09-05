#include "rt64_fast_interpreter.h"
#include "gbi/rt64_gbi_extended.h"
#include "gbi/rt64_gbi_f3dpd.h"
#include "gbi/rt64_gbi_s2dex.h"
#include "gbi/rt64_gbi_s2dex2.h"
#include "gbi/rt64_gbi_l3dex2.h"

// These entry points keep the common microcode database intact. Each unsupported
// family fails explicitly until its vertex/sprite semantics are implemented.
namespace RT64 {
    namespace GBI_F3DPD { void setup(GBI *) { throw std::runtime_error("RT64 Fast: F3DPD not implemented"); } }
    namespace GBI_S2DEX { void setup(GBI *) { throw std::runtime_error("RT64 Fast: S2DEX not implemented"); } }
    namespace GBI_S2DEX2 { void setup(GBI *) { throw std::runtime_error("RT64 Fast: S2DEX2 not implemented"); } }
    namespace GBI_L3DEX2 { void setup(GBI *) { throw std::runtime_error("RT64 Fast: L3DEX2 not implemented"); } }
    namespace GBI_EXTENDED {
        void initialize() {}
        void noOpHook(State *, DisplayList **dl) {
            if ((*dl)->w0 & 0xffffff) throw std::runtime_error("RT64 Fast: extended GBI hook not implemented");
        }
        void extendedOp(State *, DisplayList **) { throw std::runtime_error("RT64 Fast: extended GBI not implemented"); }
    }
}
