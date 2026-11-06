#ifndef MOD_GEAR_WARDROBE_CONFIG_H
#define MOD_GEAR_WARDROBE_CONFIG_H

#include "Define.h"

namespace GearWardrobe
{
    struct Config
    {
        bool enable = true;
        uint32 maxSets = 50;
        uint32 maxItems = 500;

        void Load();
    };

    Config& Cfg();
}

#endif
