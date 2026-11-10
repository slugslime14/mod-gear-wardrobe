#ifndef MOD_GEAR_WARDROBE_MGR_H
#define MOD_GEAR_WARDROBE_MGR_H

#include "DatabaseEnvFwd.h"
#include "ObjectGuid.h"
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

class Item;
class Player;

namespace GearWardrobe
{
    enum SlotMode : uint8
    {
        MODE_ITEM   = 0, // wear `item`
        MODE_EMPTY  = 1, // leave the slot empty
        MODE_IGNORE = 2, // protocol only: a slot without an entry is left as it is
    };

    struct SlotEntry
    {
        uint8 mode = MODE_ITEM;
        ObjectGuid::LowType item = 0;
    };

    struct GearSet
    {
        uint32 id = 0;
        std::string name;
        std::string icon;
        std::map<uint8, SlotEntry> slots; // equipment slot -> entry; missing slots are ignored
    };

    // A stored item keeps its item_instance row and loses its character_inventory row, as with mail. The wardrobe
    // loads every stored item once at startup and owns the object from then on: it is not in any inventory or in
    // the world, and it is the only copy. The database is written to but never read back for it while the server
    // runs, so a deposit that is still being saved can't be lost by an immediate withdraw.
    struct StoredItem
    {
        Item* item = nullptr;
        ObjectGuid::LowType depositor = 0; // also item_instance.owner_guid while stored
        std::string link;                  // entry:enchant:gem1:gem2:gem3:gem4:suffix:unique
    };

    // A slot EquipSet left as it was: the item is gone (`missing`), or the server refused it with `result`.
    struct EquipFailure
    {
        uint8 slot = 0;
        bool missing = false;
        uint32 result = 0; // InventoryResult
    };

    // Per online character; the stored items are kept apart from it (`Mgr::_stored`).
    struct Profile
    {
        std::map<uint32, GearSet> sets;
    };

    // Everything runs on the world thread: chat opcodes are thread-unsafe and commands run there too.
    class Mgr
    {
    public:
        static Mgr& Instance();

        // False for random bots; altbots and the real player get their wardrobe.
        static bool IsWardrobeUser(Player* player);

        void OnStartup();
        void OnLogin(Player* player);
        void OnLogout(Player* player);
        void OnDeleteFromDB(CharacterDatabaseTransaction trans, ObjectGuid::LowType guid);

        void HandleAddonMessage(Player* player, std::string const& body);

        bool CreateSet(Player* player, std::string const& name, std::string& error);
        bool RenameSet(Player* player, uint32 setId, std::string const& name, std::string& error);
        bool DeleteSet(Player* player, uint32 setId, std::string& error);
        bool SetIcon(Player* player, uint32 setId, std::string const& icon, std::string& error);
        bool SaveWorn(Player* player, uint32 setId, std::string& error);
        bool AssignSlot(Player* player, uint32 setId, uint8 slot, uint8 mode, ObjectGuid::LowType item,
            std::string& error);
        // `bag`/`slot` are client coordinates: bag -1 is the character sheet (slot = inventory slot id), 0-4 are bags.
        bool AssignSlotFromPos(Player* player, uint32 setId, uint8 slot, int32 bag, int32 bagSlot, std::string& error);
        bool Deposit(Player* player, int32 bag, int32 bagSlot, std::string& error);
        bool Withdraw(Player* player, ObjectGuid::LowType item, std::string& error);
        // Sends nothing; callers report `failures` to the addon (SendEquipResult) or by whisper.
        bool EquipSet(Player* player, uint32 setId, std::vector<EquipFailure>& failures, std::string& error);
        void SendEquipResult(Player* player, uint32 setId, std::vector<EquipFailure> const& failures);

        GearSet const* FindSet(Player* player, std::string const& name);
        Profile const* GetProfile(Player* player) const;
        // Stored items `player` can see and take.
        std::size_t StoredCount(Player* player) const;

        void SendSync(Player* player);

    private:
        Profile* Find(Player* player);
        GearSet* FindSet(Profile& profile, uint32 setId, std::string& error);
        void SaveSet(Player* player, GearSet const& set);

        bool CanSee(Player* player, StoredItem const& stored) const;
        StoredItem const* FindStored(Player* player, ObjectGuid::LowType guid) const;
        // Moves an item the player just took off into the wardrobe (DB writes go into `trans`).
        void StoreDetached(Player* player, Item* item, CharacterDatabaseTransaction trans);
        // Removes a stored item from the wardrobe and makes `player` its owner. The caller either puts it in the
        // player's inventory (and deletes the wardrobe row in its transaction) or hands it back with PutBack.
        Item* TakeStored(Player* player, ObjectGuid::LowType guid, StoredItem& taken);
        void PutBack(ObjectGuid::LowType guid, StoredItem&& taken);
        // Forgets a stored item without touching item_instance.
        void DropStored(ObjectGuid::LowType guid);

        std::unordered_map<ObjectGuid::LowType, Profile> _profiles;
        std::unordered_map<ObjectGuid::LowType, StoredItem> _stored; // item guid -> item, every character's
    };
}

#endif
