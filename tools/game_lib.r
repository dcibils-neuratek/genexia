/* The cfrg resource that makes a game's code an OS 9 shared library. */
#include "CodeFragments.r"
resource 'cfrg' (0) {
    {
        kPowerPCCFragArch, kIsCompleteCFrag, kNoVersionNum, kNoVersionNum,
        kDefaultStackSize, kNoAppSubFolder,
        kImportLibraryCFrag, kDataForkCFragLocator, kZeroOffset, kCFragGoesToEOF,
        "GenexiaGame"
    }
};
