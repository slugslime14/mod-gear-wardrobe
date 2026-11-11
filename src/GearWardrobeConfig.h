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
        bool sharedPool = false;    // characters of an account share stored items
        bool sharedLinked = true;   // ... and so do playerbots-linked accounts

        void Load();
    };

    Config& Cfg();
}

#endif
