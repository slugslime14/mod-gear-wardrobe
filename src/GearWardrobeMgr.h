#ifndef MOD_GEAR_WARDROBE_MGR_H
#define MOD_GEAR_WARDROBE_MGR_H

#include "DatabaseEnvFwd.h"
#include "ObjectGuid.h"
#include "Optional.h"
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

class Item;
class Player;
struct ItemTemplate;

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
        uint32 account = 0;                // the depositor's account
        std::string link;                  // entry:enchant:gem1:gem2:gem3:gem4:suffix:unique
    };

    // Shared wardrobe: an item a character took out of the wardrobe. The other characters of its group still see it,
    // as unavailable, until it is stored again or is no longer found on the holder.
    struct TakenItem
    {
        ObjectGuid::LowType holder = 0;
        uint32 account = 0; // the holder's account
        std::string link;   // as in StoredItem; refreshed from the item while the holder is online
    };

    // A slot EquipSet left as it was: the item is gone (`missing`), or the server refused it with `result`.
    struct EquipFailure
    {
        uint8 slot = 0;
        bool missing = false;
        uint32 result = 0;  // InventoryResult
        std::string holder; // shared wardrobe: the character that has the missing item, if one does
    };

    // Per online character; the stored items are kept apart from it (`Mgr::_stored`).
    struct Profile
    {
        uint32 account = 0;
        std::map<uint32, GearSet> sets;
    };

    // The character a set command is for: an online one (`player`), or an offline character that shares the wardrobe
    // (`player` null; sets can be edited, nothing can be worn or equipped).
    struct Subject
    {
        ObjectGuid::LowType guid = 0;
        uint32 account = 0;
        Player* player = nullptr;
        Profile* profile = nullptr;
        std::string name;
        uint8 cls = 0;   // offline only
        uint8 race = 0;  // offline only
        uint8 level = 0; // offline only
    };

    // An offline character's worn or carried item, read from the database.
    struct OfflineItem
    {
        ObjectGuid::LowType guid = 0;
        char loc = 'B';  // E worn, B bags, K bank
        uint8 slot = 0;  // equipment slot when worn
        ItemTemplate const* proto = nullptr;
        std::string link;
    };

    // CanWear: mod-individual-progression keeps the item for an expansion the character hasn't reached.
    constexpr uint32 WEAR_LOCKED_EXPANSION = 1000;

    // What `Mgr::Wear` did: the slot it used, and the items that came off and went to the wardrobe. When the server
    // refused the item, `failure` says why (and nothing moved).
    struct WearResult
    {
        uint8 slot = 0;
        std::vector<ObjectGuid::LowType> removed;
        Optional<EquipFailure> failure;
    };

    // An item `player` can reach: worn or in its bags (not the bank), or stored in a wardrobe it can see.
    struct ReachableItem
    {
        Item* item = nullptr; // a stored item is the wardrobe's object: read it, never keep the pointer
        bool stored = false;
    };

    // "Head", "Main Hand"... for an equipment slot.
    char const* SlotName(uint8 slot);

    // The item string after "item:" in a client link: entry:enchant:gem1:gem2:gem3:gem4:suffix:unique.
    std::string LinkOf(Item const* item);
    // `item`'s name in `reader`'s language, with its random suffix ("of the Bear").
    std::string ItemName(Item const* item, Player* reader);
    // A clickable chat link for `item`, named in `reader`'s language.
    std::string ChatLink(Item const* item, Player* reader);

    // Everything runs on the world thread: chat opcodes are thread-unsafe and commands run there too.
    class Mgr
    {
    public:
        static Mgr& Instance();

        // False for random bots; altbots and the real player get their wardrobe.
        static bool IsWardrobeUser(Player* player);
        // Whether `master` may drive `bot`'s wardrobe: its playerbots master, or (without playerbots) the same account.
        static bool MayCommand(Player* master, Player* bot);

        void OnStartup();
        void OnLogin(Player* player);
        void OnLogout(Player* player);
        // Deleting from the character list (either CharDelete.Method); runs before Player::DeleteFromDB.
        void OnDelete(ObjectGuid::LowType guid);
        // Removing for good (any caller: character list, console erase, purging old deleted characters).
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
        // Reports `target`'s equip to `viewer`'s addon.
        void SendEquipResult(Player* viewer, Player* target, uint32 setId, std::vector<EquipFailure> const& failures);
        // Removes stored items for good and pays their vendor price to `player`. Clears them from every gear set.
        bool Sell(Player* player, std::vector<ObjectGuid::LowType> const& guids, std::string& error);

        GearSet const* FindSet(Player* player, std::string const& name);
        Profile const* GetProfile(Player* player) const;
        // Stored items `player` can see and take.
        std::size_t StoredCount(Player* player) const;

        void SendSync(Player* player) { SendSync(player, player); }
        // `target`'s sets and wardrobe, sent to `viewer`'s addon (the viewer itself, or an altbot it controls).
        void SendSync(Player* viewer, Player* target);

        // Bot whispers (gw wear / gw store / gw items).
        // Worn and carried items first, in bag order, then stored ones, oldest first.
        std::vector<ReachableItem> Reachable(Player* player) const;
        // Puts on one item from the bags or the wardrobe; what it replaces goes to the wardrobe. `allowed` limits the
        // slots it may go in (empty: any), and a slot in `keep` is only replaced when there is no other choice.
        // False with `error` when it can't be tried, or with `result.failure` when the server refused it.
        bool Wear(Player* player, ObjectGuid::LowType guid, std::vector<uint8> const& allowed,
            std::set<uint8> const& keep, WearResult& result, std::string& error);
        // A stored item `player` can see, or nullptr.
        Item const* GetStored(Player* player, ObjectGuid::LowType guid) const;
        // Moves one worn or carried item into the wardrobe.
        bool Store(Player* player, ObjectGuid::LowType guid, std::string& error);
        // Whether `player` could wear `item` (class, skill, level, reputation, other modules' rules), wherever the
        // item is now. EQUIP_ERR_OK (0) if so.
        uint32 CanWear(Player* player, Item* item) const;
        // The first equipment slot `item` fits for `player`, or NULL_SLOT.
        static uint8 FirstSlot(Player* player, Item const* item);

    private:
        Profile* Find(Player* player);
        Subject Online(Player* player);
        // The character an addon message is for ("@guid|" prefix): `viewer`, an online altbot it controls, or an
        // offline character sharing its wardrobe.
        bool ResolveTarget(Player* viewer, ObjectGuid::LowType guid, Subject& out, std::string& error);
        bool LoadOffline(Player* viewer, ObjectGuid::LowType guid, Subject& out, std::string& error);
        // Accounts whose offline characters `viewer` may edit: its own, plus the shared wardrobe's (no random bots).
        std::vector<uint32> OfflineAccounts(Player* viewer) const;
        void LoadSets(ObjectGuid::LowType owner, Profile& profile);
        std::vector<OfflineItem> LoadOfflineInventory(ObjectGuid::LowType owner) const;
        // A best guess at whether an offline character can wear `proto`: class, race, armor and weapon types, level.
        static uint32 GuessWear(Subject const& subject, ItemTemplate const* proto);
        // Whether `proto` goes in `slot`, without a character to ask (dual wield and Titan Grip unknown).
        static bool FitsSlot(ItemTemplate const* proto, uint8 slot);

        // Whether `viewer`'s deposits and withdrawals change the wardrobe `target` sees.
        bool SamePool(Player* viewer, Subject const& target) const;
        // Stores an item from `viewer`'s bags or worn gear and puts it in a set slot of `target` (another character).
        bool AssignFromViewer(Player* viewer, Subject const& target, uint32 setId, uint8 slot, int32 bag, int32 bagSlot,
            std::string& error);
        // "item,slot,character,set" for every gear set slot, of any character sharing the wardrobe of character
        // `owner` (offline ones too), that names one of `items`.
        std::vector<std::string> UsedBy(ObjectGuid::LowType owner, uint32 account,
            std::set<ObjectGuid::LowType> const& items) const;
        // "guid,name,class,level,offline": `viewer` first, then the online altbots it controls, then offline
        // characters sharing its wardrobe.
        std::vector<std::string> Characters(Player* viewer) const;
        void SendSync(Player* viewer, Subject const& target);
        GearSet* FindSet(Profile& profile, uint32 setId, std::string& error);
        static GearSet* FindNamed(Profile& profile, std::string const& name);
        void SaveSet(Player* player, GearSet const& set);
        void SaveSet(ObjectGuid::LowType owner, GearSet const& set);

        bool CreateSet(Subject const& subject, std::string const& name, std::string& error);
        bool RenameSet(Subject const& subject, uint32 setId, std::string const& name, std::string& error);
        bool DeleteSet(Subject const& subject, uint32 setId, std::string& error);
        bool SetIcon(Subject const& subject, uint32 setId, std::string const& icon, std::string& error);
        bool AssignSlot(Subject const& subject, uint32 setId, uint8 slot, uint8 mode, ObjectGuid::LowType item,
            std::string& error);

        bool DepositItem(Player* player, Item* item, std::string& error);
        // Takes off what changes and puts on `targets` (MODE_ITEM entries the player can reach, or MODE_EMPTY), in one
        // transaction. Whatever came off and wasn't put back goes to the wardrobe and is added to `removed`.
        void ApplyTargets(Player* player, std::map<uint8, SlotEntry> const& targets,
            std::vector<EquipFailure>& failures, std::vector<ObjectGuid::LowType>& removed);

        bool CanSee(Player* player, StoredItem const& stored) const;
        bool CanSee(ObjectGuid::LowType owner, uint32 account, StoredItem const& stored) const;
        StoredItem const* FindStored(Player* player, ObjectGuid::LowType guid) const;
        StoredItem const* FindStored(ObjectGuid::LowType owner, uint32 account, ObjectGuid::LowType guid) const;
        // Moves an item the player just took off into the wardrobe (DB writes go into `trans`).
        void StoreDetached(Player* player, Item* item, CharacterDatabaseTransaction trans);
        // Removes a stored item from the wardrobe and makes `player` its owner. The caller either puts it in the
        // player's inventory (and deletes the wardrobe row in its transaction) or hands it back with PutBack.
        Item* TakeStored(Player* player, ObjectGuid::LowType guid, StoredItem& taken);
        void PutBack(ObjectGuid::LowType guid, StoredItem&& taken);
        // Forgets a stored item without touching item_instance.
        void DropStored(ObjectGuid::LowType guid);

        // Shared wardrobe: accounts sharing one pool have the same group id (the lowest account id among them).
        void LoadGroups();
        uint32 GroupOf(uint32 account) const;
        std::vector<uint32> GroupAccounts(uint32 account) const;
        // Gives the stored items of a character that is being deleted to another character of its group, so they
        // stay in the shared wardrobe. Returns false (and changes nothing) when there is no one to give them to.
        bool HandOff(ObjectGuid::LowType deleted, CharacterDatabaseTransaction trans);
        // The name of the character in `player`'s group that carries item `guid`, or "".
        std::string HolderOf(Player* player, ObjectGuid::LowType guid) const;

        // Copies: a set slot whose item `player` can't get (another character has it, it is banked or gone) may use
        // an identical copy instead: same item, enchant, gems and random suffix. One the character carries first,
        // else the oldest stored one. Never one `set` already uses or one in `claimed`. 0 if there is none.
        ObjectGuid::LowType FindSpare(Player* player, GearSet const& set, ObjectGuid::LowType wanted,
            std::set<ObjectGuid::LowType> const& claimed) const;
        // The link of any item, wherever it is now (stored, taken out, or from item_instance); "" if it is gone.
        std::string KnownLink(ObjectGuid::LowType guid) const;

        // Shared wardrobe bookkeeping of taken-out items (TakenItem); DB writes go into `trans`.
        void RecordTaken(Player* taker, Item* item, CharacterDatabaseTransaction trans);
        void ForgetTaken(ObjectGuid::LowType guid, CharacterDatabaseTransaction trans);
        void ForgetHeldBy(ObjectGuid::LowType holder, CharacterDatabaseTransaction trans);
        // Checks where the taken-out items of `viewer`'s group are now (forgetting those no longer found) and returns
        // the ones another character has, in guid order.
        std::vector<ObjectGuid::LowType> ResolveTaken(Player* viewer);
        std::vector<ObjectGuid::LowType> ResolveTaken(ObjectGuid::LowType self, uint32 account);

        std::unordered_map<ObjectGuid::LowType, Profile> _profiles;
        // Sets of offline characters that were shown or logged out since startup. Kept, not re-read: the rows of a
        // recent change may still be on their way to the database. Moved back into _profiles at login.
        std::unordered_map<ObjectGuid::LowType, Profile> _offline;
        std::unordered_map<ObjectGuid::LowType, StoredItem> _stored; // item guid -> item, every character's
        std::unordered_map<ObjectGuid::LowType, TakenItem> _taken;   // item guid -> who took it out
        std::unordered_map<uint32, uint32> _groupOf;                 // linked account -> group id
    };
}

#endif
