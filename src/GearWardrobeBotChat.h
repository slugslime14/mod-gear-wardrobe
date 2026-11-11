#ifndef MOD_GEAR_WARDROBE_BOT_CHAT_H
#define MOD_GEAR_WARDROBE_BOT_CHAT_H

#include <string>

class Player;

// Wardrobe commands whispered to a bot: "gw list", "gw equip <set>", "gw save <set>", "gw items [slot]",
// "gw wear <item> [slot]", "gw store <item>" (<item>: one or more item links, or a name). The bot answers by whisper.
// Bots have no addon, so this is the only way to drive an altbot's wardrobe.
namespace GearWardrobe::BotChat
{
    // True when `msg` was a wardrobe command for `bot`. It has then been handled and must not be delivered.
    bool HandleWhisper(Player* sender, Player* bot, std::string const& msg);
}

#endif
