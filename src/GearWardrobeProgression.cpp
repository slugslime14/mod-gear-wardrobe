#include "GearWardrobeProgression.h"

// Apart from GearWardrobeMgr.cpp: IndividualProgression.h and the playerbots headers define the same enumerators.
#if __has_include("IndividualProgression.h")
#include "IndividualProgression.h"
#define GEAR_WARDROBE_INDIVIDUAL_PROGRESSION
#endif

namespace GearWardrobe::Progression
{
    bool Present()
    {
#ifdef GEAR_WARDROBE_INDIVIDUAL_PROGRESSION
        return true;
#else
        return false;
#endif
    }

    bool IsLockedExpansionGear([[maybe_unused]] Player* player, [[maybe_unused]] ItemTemplate const* proto)
    {
#ifdef GEAR_WARDROBE_INDIVIDUAL_PROGRESSION
        return sIndividualProgression->isLockedExpansionGear(player, proto);
#else
        return false;
#endif
    }
}
