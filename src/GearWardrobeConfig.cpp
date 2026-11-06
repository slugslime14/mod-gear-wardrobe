#include "GearWardrobeConfig.h"
#include "Config.h"

namespace GearWardrobe
{
    Config& Cfg()
    {
        static Config config;
        return config;
    }

    void Config::Load()
    {
        enable = sConfigMgr->GetOption<bool>("GearWardrobe.Enable", true);
        maxSets = sConfigMgr->GetOption<uint32>("GearWardrobe.MaxSets", 50);
        maxItems = sConfigMgr->GetOption<uint32>("GearWardrobe.MaxItems", 500);
    }
}
