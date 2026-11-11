#ifndef MOD_GEAR_WARDROBE_PROGRESSION_H
#define MOD_GEAR_WARDROBE_PROGRESSION_H

class Player;
struct ItemTemplate;

// mod-individual-progression, when it is compiled in.
namespace GearWardrobe::Progression
{
    bool Present();
    // Its expansion lock, without the chat message its equip hook sends.
    bool IsLockedExpansionGear(Player* player, ItemTemplate const* proto);
}

#endif
