#include "GearWardrobeMgr.h"
#include "Bag.h"
#include "Battleground.h"
#include "DatabaseEnv.h"
#include "GearWardrobeComms.h"
#include "GearWardrobeConfig.h"
#include "Item.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "StringConvert.h"
#include "StringFormat.h"
#include "Tokenize.h"
#include "WorldSession.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <vector>

#if __has_include("PlayerbotAIConfig.h")
#include "PlayerbotAIConfig.h"
#define GEAR_WARDROBE_PLAYERBOTS
#endif

namespace GearWardrobe
{
    namespace
    {
        constexpr std::size_t MAX_NAME = 32;
        constexpr std::size_t MAX_ICON = 100;

        using Enchants = std::array<uint32, MAX_ENCHANTMENT_SLOT>;

        // Matches the client's item string after "item:"; the 4th gem field is the prismatic socket.
        std::string BuildLink(uint32 entry, Enchants const& ench, int32 randomProperty, uint32 suffixFactor)
        {
            return Acore::StringFormat("{}:{}:{}:{}:{}:{}:{}:{}", entry, ench[PERM_ENCHANTMENT_SLOT],
                ench[SOCK_ENCHANTMENT_SLOT], ench[SOCK_ENCHANTMENT_SLOT_2], ench[SOCK_ENCHANTMENT_SLOT_3],
                ench[PRISMATIC_ENCHANTMENT_SLOT], randomProperty, suffixFactor);
        }

        std::string LinkOf(Item const* item)
        {
            Enchants ench{};
            for (uint8 i = 0; i < MAX_ENCHANTMENT_SLOT; ++i)
                ench[i] = item->GetEnchantmentId(EnchantmentSlot(i));
            return BuildLink(item->GetEntry(), ench, item->GetItemRandomPropertyId(), item->GetItemSuffixFactor());
        }

        // Protocol separators and chat escapes never make it into a stored name.
        constexpr std::string_view FORBIDDEN = ",;\t\n\r|'\"\\";

        std::string CleanName(std::string_view text)
        {
            std::string out;
            for (char c : text)
                if (FORBIDDEN.find(c) == std::string_view::npos)
                    out.push_back(c);

            out.erase(0, out.find_first_not_of(' '));
            out.erase(out.find_last_not_of(' ') + 1);
            if (out.size() > MAX_NAME)
            {
                out.resize(MAX_NAME);
                // Do not leave half a UTF-8 sequence behind.
                while (!out.empty() && (static_cast<unsigned char>(out.back()) & 0xC0) == 0x80)
                    out.pop_back();
                if (!out.empty() && (static_cast<unsigned char>(out.back()) & 0x80))
                    out.pop_back();
            }
            return out;
        }

        std::string CleanIcon(std::string_view text)
        {
            std::string out;
            for (char c : text)
                if (std::isalnum(static_cast<unsigned char>(c)) || c == '_')
                    out.push_back(c);
            if (out.size() > MAX_ICON)
                out.resize(MAX_ICON);
            return out;
        }

        bool ToServerPos(int32 bag, int32 slot, uint8& outBag, uint8& outSlot)
        {
            if (bag == -1)
            {
                if (slot < 1 || slot > EQUIPMENT_SLOT_END)
                    return false;
                outBag = INVENTORY_SLOT_BAG_0;
                outSlot = uint8(slot - 1);
                return true;
            }
            if (bag == 0)
            {
                if (slot < 1 || slot > INVENTORY_SLOT_ITEM_END - INVENTORY_SLOT_ITEM_START)
                    return false;
                outBag = INVENTORY_SLOT_BAG_0;
                outSlot = uint8(INVENTORY_SLOT_ITEM_START + slot - 1);
                return true;
            }
            if (bag >= 1 && bag <= INVENTORY_SLOT_BAG_END - INVENTORY_SLOT_BAG_START
                && slot >= 1 && slot <= MAX_BAG_SIZE)
            {
                outBag = uint8(INVENTORY_SLOT_BAG_START + bag - 1);
                outSlot = uint8(slot - 1);
                return true;
            }
            return false;
        }

        Item* FindCarried(Player* player, ObjectGuid::LowType guid)
        {
            Item* item = player->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(guid));
            if (item && Player::IsBankPos(item->GetPos()))
                return nullptr;
            return item;
        }

        // Shared by every switch, deposit and withdraw; equipping checks the rest per item.
        bool CanChangeGear(Player* player, std::string& error)
        {
            if (!player->IsAlive())
                error = "You can't change gear while dead.";
            else if (player->IsInCombat())
                error = "You can't change gear in combat.";
            else if (player->IsNonMeleeSpellCast(false))
                error = "You can't change gear while casting.";
            else if (player->GetTradeData())
                error = "You can't change gear while trading.";
            else if (!player->GetLootGUID().IsEmpty())
                error = "Close the loot window first.";
            else if (player->IsBeingTeleported())
                error = "You can't change gear right now.";
            else if (Battleground* bg = player->GetBattleground();
                bg && bg->isArena() && bg->GetStatus() == STATUS_IN_PROGRESS)
                error = "You can't change gear during an arena match.";
            else
                return true;
            return false;
        }

        bool IsWardrobeGear(Item const* item, std::string& error)
        {
            ItemTemplate const* proto = item->GetTemplate();
            if (proto->Class != ITEM_CLASS_ARMOR && proto->Class != ITEM_CLASS_WEAPON)
                error = "Only armor and weapons fit in the wardrobe.";
            else if (item->IsBag() || proto->Duration || item->IsInTrade() || item->m_lootGenerated)
                error = "That item can't be stored in the wardrobe.";
            else
                return true;
            return false;
        }
    }

    Mgr& Mgr::Instance()
    {
        static Mgr instance;
        return instance;
    }

    bool Mgr::IsWardrobeUser(Player* player)
    {
        if (!Cfg().enable || !player || !player->GetSession())
            return false;

#ifdef GEAR_WARDROBE_PLAYERBOTS
        // Random bots (addclass bots too) live on the rndbot accounts and never get a wardrobe. Altbots are on the
        // player's own account and keep the sets saved on that character.
        if (sPlayerbotAIConfig.IsInRandomAccountList(player->GetSession()->GetAccountId()))
            return false;
#endif

        return true;
    }

    void Mgr::OnStartup()
    {
        // Synchronous on purpose: the wardrobe is read once, right after these fixes, and never again.

        // Rows whose item was deleted some other way would otherwise show as ghosts, and rows of a character that
        // no longer exists point at an item that went with it.
        CharacterDatabase.DirectExecute("DELETE w FROM gear_wardrobe_item w "
            "LEFT JOIN item_instance ii ON ii.guid = w.item_guid WHERE ii.guid IS NULL");
        CharacterDatabase.DirectExecute("DELETE w FROM gear_wardrobe_item w "
            "LEFT JOIN characters c ON c.guid = w.owner_guid WHERE c.guid IS NULL");

        // An item can never be both carried and stored; if it ever is, the inventory copy wins and owns the item.
        CharacterDatabase.DirectExecute("UPDATE item_instance ii "
            "INNER JOIN gear_wardrobe_item w ON w.item_guid = ii.guid "
            "INNER JOIN character_inventory ci ON ci.item = ii.guid SET ii.owner_guid = ci.guid");
        CharacterDatabase.DirectExecute("DELETE w FROM gear_wardrobe_item w "
            "INNER JOIN character_inventory ci ON ci.item = w.item_guid");

        // The first 11 columns are the ones Item::LoadFromDB reads, in its order (as CHAR_SEL_MAILITEMS has them).
        QueryResult result = CharacterDatabase.Query("SELECT ii.creatorGuid, ii.giftCreatorGuid, ii.count, "
            "ii.duration, ii.charges, ii.flags, ii.enchantments, ii.randomPropertyId, ii.durability, ii.playedTime, "
            "ii.text, w.item_guid, ii.itemEntry, w.owner_guid "
            "FROM gear_wardrobe_item w INNER JOIN item_instance ii ON ii.guid = w.item_guid");
        if (!result)
            return;

        do
        {
            Field* fields = result->Fetch();
            ObjectGuid::LowType const guid = fields[11].Get<uint32>();
            uint32 const entry = fields[12].Get<uint32>();
            ObjectGuid::LowType const depositor = fields[13].Get<uint32>();

            // An unknown entry stays in the table, out of sight, as before.
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
            if (!proto)
                continue;

            Item* item = NewItemOrBag(proto);
            if (!item->LoadFromDB(guid, ObjectGuid::Create<HighGuid::Player>(depositor), fields, entry))
            {
                delete item;
                continue;
            }
            _stored[guid] = { item, depositor, LinkOf(item) };
        } while (result->NextRow());

        LOG_INFO("module", "mod-gear-wardrobe: {} stored items loaded.", _stored.size());
    }

    void Mgr::OnLogin(Player* player)
    {
        ObjectGuid::LowType const owner = player->GetGUID().GetCounter();
        Profile& profile = _profiles[owner];
        profile = Profile();

        // An item can never be both carried and stored; if it ever is, the inventory copy wins.
        for (auto itr = _stored.begin(); itr != _stored.end();)
        {
            ObjectGuid::LowType const guid = itr->first;
            ++itr;
            if (player->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(guid)))
            {
                CharacterDatabase.Execute("DELETE FROM gear_wardrobe_item WHERE item_guid = {}", guid);
                DropStored(guid);
            }
        }

        if (QueryResult result = CharacterDatabase.Query(
            "SELECT set_id, name, icon FROM gear_wardrobe_set WHERE owner_guid = {}", owner))
        {
            do
            {
                Field* fields = result->Fetch();
                GearSet& set = profile.sets[fields[0].Get<uint32>()];
                set.id = fields[0].Get<uint32>();
                set.name = fields[1].Get<std::string>();
                set.icon = fields[2].Get<std::string>();
            } while (result->NextRow());
        }

        if (QueryResult result = CharacterDatabase.Query(
            "SELECT set_id, slot, mode, item_guid FROM gear_wardrobe_slot WHERE owner_guid = {}", owner))
        {
            do
            {
                Field* fields = result->Fetch();
                auto set = profile.sets.find(fields[0].Get<uint32>());
                uint8 const slot = fields[1].Get<uint8>();
                if (set != profile.sets.end() && slot < EQUIPMENT_SLOT_END)
                    set->second.slots[slot] = { fields[2].Get<uint8>(), fields[3].Get<uint32>() };
            } while (result->NextRow());
        }

        // Bot sessions are headless and have no addon to talk to.
        if (!player->GetSession()->IsHeadless())
            SendSync(player);
    }

    void Mgr::OnLogout(Player* player)
    {
        _profiles.erase(player->GetGUID().GetCounter());
    }

    void Mgr::OnDeleteFromDB(CharacterDatabaseTransaction trans, ObjectGuid::LowType guid)
    {
        // The stored items themselves go with the rest of item_instance (deleted by owner_guid).
        trans->Append("DELETE FROM gear_wardrobe_item WHERE owner_guid = {}", guid);
        trans->Append("DELETE FROM gear_wardrobe_slot WHERE owner_guid = {}", guid);
        trans->Append("DELETE FROM gear_wardrobe_set WHERE owner_guid = {}", guid);
        _profiles.erase(guid);

        for (auto itr = _stored.begin(); itr != _stored.end();)
        {
            ObjectGuid::LowType const itemGuid = itr->first;
            bool const theirs = itr->second.depositor == guid;
            ++itr;
            if (theirs)
                DropStored(itemGuid);
        }
    }

    Profile* Mgr::Find(Player* player)
    {
        auto itr = _profiles.find(player->GetGUID().GetCounter());
        return itr != _profiles.end() ? &itr->second : nullptr;
    }

    bool Mgr::CanSee(Player* player, StoredItem const& stored) const
    {
        return stored.depositor == player->GetGUID().GetCounter();
    }

    StoredItem const* Mgr::FindStored(Player* player, ObjectGuid::LowType guid) const
    {
        auto itr = _stored.find(guid);
        return itr != _stored.end() && CanSee(player, itr->second) ? &itr->second : nullptr;
    }

    std::size_t Mgr::StoredCount(Player* player) const
    {
        return std::count_if(_stored.begin(), _stored.end(),
            [this, player](auto const& entry) { return CanSee(player, entry.second); });
    }

    Item* Mgr::TakeStored(Player* player, ObjectGuid::LowType guid, StoredItem& taken)
    {
        auto itr = _stored.find(guid);
        if (itr == _stored.end() || !CanSee(player, itr->second))
            return nullptr;

        taken = std::move(itr->second);
        _stored.erase(itr);

        // Binding checks compare the owner with the character, so from here on the item is the taker's.
        taken.item->SetOwnerGUID(player->GetGUID());
        return taken.item;
    }

    void Mgr::PutBack(ObjectGuid::LowType guid, StoredItem&& taken)
    {
        taken.item->SetOwnerGUID(ObjectGuid::Create<HighGuid::Player>(taken.depositor));
        _stored[guid] = std::move(taken);
    }

    void Mgr::DropStored(ObjectGuid::LowType guid)
    {
        auto itr = _stored.find(guid);
        if (itr == _stored.end())
            return;
        delete itr->second.item;
        _stored.erase(itr);
    }

    Profile const* Mgr::GetProfile(Player* player) const
    {
        auto itr = _profiles.find(player->GetGUID().GetCounter());
        return itr != _profiles.end() ? &itr->second : nullptr;
    }

    GearSet* Mgr::FindSet(Profile& profile, uint32 setId, std::string& error)
    {
        auto itr = profile.sets.find(setId);
        if (itr == profile.sets.end())
        {
            error = "That gear set no longer exists.";
            return nullptr;
        }
        return &itr->second;
    }

    GearSet const* Mgr::FindSet(Player* player, std::string const& name)
    {
        Profile* profile = Find(player);
        if (!profile)
            return nullptr;

        std::string wanted = name;
        std::transform(wanted.begin(), wanted.end(), wanted.begin(), ::tolower);
        for (auto const& [id, set] : profile->sets)
        {
            std::string candidate = set.name;
            std::transform(candidate.begin(), candidate.end(), candidate.begin(), ::tolower);
            if (candidate == wanted)
                return &set;
        }
        return nullptr;
    }

    void Mgr::SaveSet(Player* player, GearSet const& set)
    {
        ObjectGuid::LowType const owner = player->GetGUID().GetCounter();
        std::string name = set.name;
        CharacterDatabase.EscapeString(name);

        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        trans->Append("REPLACE INTO gear_wardrobe_set (owner_guid, set_id, name, icon) VALUES ({}, {}, '{}', '{}')",
            owner, set.id, name, set.icon);
        trans->Append("DELETE FROM gear_wardrobe_slot WHERE owner_guid = {} AND set_id = {}", owner, set.id);
        for (auto const& [slot, entry] : set.slots)
            trans->Append("INSERT INTO gear_wardrobe_slot (owner_guid, set_id, slot, mode, item_guid) "
                "VALUES ({}, {}, {}, {}, {})",
                owner, set.id, slot, entry.mode, entry.item);
        CharacterDatabase.CommitTransaction(trans);
    }

    bool Mgr::CreateSet(Player* player, std::string const& rawName, std::string& error)
    {
        Profile* profile = Find(player);
        if (!profile)
            return false;

        std::string const name = CleanName(rawName);
        if (name.empty())
        {
            error = "Give the set a name.";
            return false;
        }
        if (profile->sets.size() >= Cfg().maxSets)
        {
            error = Acore::StringFormat("You already have {} gear sets.", Cfg().maxSets);
            return false;
        }
        if (FindSet(player, name))
        {
            error = "You already have a set with that name.";
            return false;
        }

        uint32 const id = profile->sets.empty() ? 1 : profile->sets.rbegin()->first + 1;
        GearSet& set = profile->sets[id];
        set.id = id;
        set.name = name;

        // A new set starts as what you are wearing; empty slots are left ignored.
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
            if (Item* worn = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                set.slots[slot] = { MODE_ITEM, worn->GetGUID().GetCounter() };

        SaveSet(player, set);
        return true;
    }

    bool Mgr::RenameSet(Player* player, uint32 setId, std::string const& rawName, std::string& error)
    {
        Profile* profile = Find(player);
        if (!profile)
            return false;

        GearSet* set = FindSet(*profile, setId, error);
        if (!set)
            return false;

        std::string const name = CleanName(rawName);
        if (name.empty())
        {
            error = "Give the set a name.";
            return false;
        }
        if (GearSet const* other = FindSet(player, name); other && other->id != setId)
        {
            error = "You already have a set with that name.";
            return false;
        }

        set->name = name;
        SaveSet(player, *set);
        return true;
    }

    bool Mgr::DeleteSet(Player* player, uint32 setId, std::string& error)
    {
        Profile* profile = Find(player);
        if (!profile || !FindSet(*profile, setId, error))
            return false;

        // The set's items stay where they are; stored ones remain in the wardrobe.
        ObjectGuid::LowType const owner = player->GetGUID().GetCounter();
        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        trans->Append("DELETE FROM gear_wardrobe_slot WHERE owner_guid = {} AND set_id = {}", owner, setId);
        trans->Append("DELETE FROM gear_wardrobe_set WHERE owner_guid = {} AND set_id = {}", owner, setId);
        CharacterDatabase.CommitTransaction(trans);
        profile->sets.erase(setId);
        return true;
    }

    bool Mgr::SetIcon(Player* player, uint32 setId, std::string const& icon, std::string& error)
    {
        Profile* profile = Find(player);
        if (!profile)
            return false;

        GearSet* set = FindSet(*profile, setId, error);
        if (!set)
            return false;

        set->icon = CleanIcon(icon);
        SaveSet(player, *set);
        return true;
    }

    bool Mgr::SaveWorn(Player* player, uint32 setId, std::string& error)
    {
        Profile* profile = Find(player);
        if (!profile)
            return false;

        GearSet* set = FindSet(*profile, setId, error);
        if (!set)
            return false;

        // Ignored slots stay ignored; every other slot takes what is worn there now.
        for (auto& [slot, entry] : set->slots)
        {
            if (Item* worn = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                entry = { MODE_ITEM, worn->GetGUID().GetCounter() };
            else
                entry = { MODE_EMPTY, 0 };
        }

        SaveSet(player, *set);
        return true;
    }

    bool Mgr::AssignSlot(Player* player, uint32 setId, uint8 slot, uint8 mode, ObjectGuid::LowType itemGuid,
        std::string& error)
    {
        Profile* profile = Find(player);
        if (!profile)
            return false;

        GearSet* set = FindSet(*profile, setId, error);
        if (!set)
            return false;

        if (slot >= EQUIPMENT_SLOT_END)
        {
            error = "Invalid slot.";
            return false;
        }

        if (mode == MODE_IGNORE)
            set->slots.erase(slot);
        else if (mode == MODE_EMPTY)
            set->slots[slot] = { MODE_EMPTY, 0 };
        else if (mode == MODE_ITEM)
        {
            ItemTemplate const* proto = nullptr;
            if (Item* carried = FindCarried(player, itemGuid))
                proto = carried->GetTemplate();
            else if (StoredItem const* stored = FindStored(player, itemGuid))
                proto = stored->item->GetTemplate();

            if (!proto)
            {
                error = "That item isn't on you or in your wardrobe.";
                return false;
            }
            if (player->FindEquipSlot(proto, slot, true) != slot)
            {
                error = "That item doesn't go in that slot.";
                return false;
            }

            // One item can only fill one slot of a set (rings and trinkets).
            for (auto itr = set->slots.begin(); itr != set->slots.end();)
            {
                if (itr->first != slot && itr->second.mode == MODE_ITEM && itr->second.item == itemGuid)
                    itr = set->slots.erase(itr);
                else
                    ++itr;
            }
            set->slots[slot] = { MODE_ITEM, itemGuid };
        }
        else
        {
            error = "Invalid slot mode.";
            return false;
        }

        SaveSet(player, *set);
        return true;
    }

    bool Mgr::AssignSlotFromPos(Player* player, uint32 setId, uint8 slot, int32 bag, int32 bagSlot, std::string& error)
    {
        uint8 serverBag = 0;
        uint8 serverSlot = 0;
        Item* item = ToServerPos(bag, bagSlot, serverBag, serverSlot)
            ? player->GetItemByPos(serverBag, serverSlot) : nullptr;
        if (!item)
        {
            error = "There is no item there.";
            return false;
        }
        return AssignSlot(player, setId, slot, MODE_ITEM, item->GetGUID().GetCounter(), error);
    }

    void Mgr::StoreDetached(Player* player, Item* item, CharacterDatabaseTransaction trans)
    {
        // Same steps as sending an item by mail (MailHandler): out of character_inventory, item_instance kept.
        ObjectGuid::LowType const guid = item->GetGUID().GetCounter();
        ObjectGuid::LowType const depositor = player->GetGUID().GetCounter();
        item->DeleteFromInventoryDB(trans);
        if (item->GetState() == ITEM_UNCHANGED)
            item->FSetState(ITEM_CHANGED);
        item->SetOwnerGUID(player->GetGUID());
        item->SaveToDB(trans);
        trans->Append("REPLACE INTO gear_wardrobe_item (item_guid, owner_guid, deposited) "
            "VALUES ({}, {}, UNIX_TIMESTAMP())", guid, depositor);

        // The wardrobe keeps the object (see StoredItem).
        StoredItem& stored = _stored[guid];
        if (stored.item && stored.item != item)
            delete stored.item;
        stored = { item, depositor, LinkOf(item) };
    }

    bool Mgr::Deposit(Player* player, int32 bag, int32 bagSlot, std::string& error)
    {
        Profile* profile = Find(player);
        if (!profile)
            return false;

        uint8 serverBag = 0;
        uint8 serverSlot = 0;
        Item* item = ToServerPos(bag, bagSlot, serverBag, serverSlot)
            ? player->GetItemByPos(serverBag, serverSlot) : nullptr;
        if (!item)
        {
            error = "There is no item there.";
            return false;
        }
        if (!IsWardrobeGear(item, error))
            return false;
        if (StoredCount(player) >= Cfg().maxItems)
        {
            error = Acore::StringFormat("Your wardrobe is full ({} items).", Cfg().maxItems);
            return false;
        }

        if (item->IsEquipped())
        {
            if (!CanChangeGear(player, error))
                return false;
            InventoryResult res = player->CanUnequipItem(item->GetPos(), false);
            if (res != EQUIP_ERR_OK)
            {
                player->SendEquipError(res, item);
                error = "You can't take that off right now.";
                return false;
            }
        }

        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        item->SetNotRefundable(player);
        item->ClearSoulboundTradeable(player);
        player->MoveItemFromInventory(serverBag, serverSlot, true);
        StoreDetached(player, item, trans);
        player->SaveInventoryAndGoldToDB(trans);
        CharacterDatabase.CommitTransaction(trans);
        return true;
    }

    bool Mgr::Withdraw(Player* player, ObjectGuid::LowType guid, std::string& error)
    {
        if (!Find(player))
            return false;

        if (!FindStored(player, guid))
        {
            error = "That item isn't in your wardrobe.";
            return false;
        }

        if (player->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(guid)))
        {
            // Already carried: the wardrobe entry is stale, never hand out a second copy.
            CharacterDatabase.Execute("DELETE FROM gear_wardrobe_item WHERE item_guid = {}", guid);
            DropStored(guid);
            return true;
        }

        StoredItem taken;
        Item* item = TakeStored(player, guid, taken);

        ItemPosCountVec dest;
        InventoryResult res = player->CanStoreItem(NULL_BAG, NULL_SLOT, dest, item, false);
        if (res != EQUIP_ERR_OK)
        {
            player->SendEquipError(res, item);
            PutBack(guid, std::move(taken));
            error = "Your bags are full.";
            return false;
        }

        // Same steps as taking an item from mail (HandleMailTakeItem).
        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        player->MoveItemToInventory(dest, item, true);
        trans->Append("DELETE FROM gear_wardrobe_item WHERE item_guid = {}", guid);
        player->SaveInventoryAndGoldToDB(trans);
        CharacterDatabase.CommitTransaction(trans);
        return true;
    }

    bool Mgr::EquipSet(Player* player, uint32 setId, std::vector<EquipFailure>& failures, std::string& error)
    {
        Profile* profile = Find(player);
        if (!profile)
            return false;

        GearSet* set = FindSet(*profile, setId, error);
        if (!set || !CanChangeGear(player, error))
            return false;

        std::map<uint8, SlotEntry> targets = set->slots;

        // A two-hander in the main hand pushes out whatever the set leaves in the off hand.
        auto mh = targets.find(EQUIPMENT_SLOT_MAINHAND);
        if (mh != targets.end() && mh->second.mode == MODE_ITEM && !player->CanTitanGrip())
        {
            ItemTemplate const* proto = nullptr;
            if (Item* carried = FindCarried(player, mh->second.item))
                proto = carried->GetTemplate();
            else if (StoredItem const* stored = FindStored(player, mh->second.item))
                proto = stored->item->GetTemplate();

            if (proto && proto->InventoryType == INVTYPE_2HWEAPON)
                targets[EQUIPMENT_SLOT_OFFHAND] = { MODE_EMPTY, 0 };
        }

        std::vector<std::pair<uint8, ObjectGuid::LowType>> toEquip;
        std::vector<uint8> toClear;
        for (auto const& [slot, entry] : targets)
        {
            Item* worn = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (entry.mode == MODE_EMPTY)
            {
                if (worn)
                    toClear.push_back(slot);
                continue;
            }
            if (worn && worn->GetGUID().GetCounter() == entry.item)
                continue;
            if (!FindCarried(player, entry.item) && !FindStored(player, entry.item))
            {
                // Sold, destroyed or banked: leave the slot alone.
                failures.push_back({ slot, true, 0 });
                continue;
            }
            if (worn)
                toClear.push_back(slot);
            toEquip.emplace_back(slot, entry.item);
        }

        // Phase A: take off everything that changes. The items stay in memory so a piece that only moves to
        // another slot never touches the wardrobe.
        std::map<ObjectGuid::LowType, Item*> detached;
        for (uint8 slot : toClear)
        {
            Item* worn = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (!worn)
                continue;
            worn->SetNotRefundable(player);
            worn->ClearSoulboundTradeable(player);
            player->MoveItemFromInventory(INVENTORY_SLOT_BAG_0, slot, true);
            detached[worn->GetGUID().GetCounter()] = worn;
        }

        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        bool shownError = false;

        // Phase B: slot order already puts the main hand before the off hand.
        for (auto const& [slot, guid] : toEquip)
        {
            Item* item = nullptr;
            StoredItem taken;
            bool fromWardrobe = false;
            if (auto itr = detached.find(guid); itr != detached.end())
            {
                item = itr->second;
                detached.erase(itr);
            }
            else if (Item* carried = FindCarried(player, guid))
            {
                // Worn in a slot the set leaves alone, or in the bags.
                carried->SetNotRefundable(player);
                carried->ClearSoulboundTradeable(player);
                player->MoveItemFromInventory(carried->GetBagSlot(), carried->GetSlot(), true);
                item = carried;
            }
            else if ((item = TakeStored(player, guid, taken)))
                fromWardrobe = true;

            if (!item)
            {
                failures.push_back({ slot, true, 0 });
                continue;
            }

            uint16 dest = 0;
            InventoryResult res = player->CanEquipItem(slot, dest, item, false);
            if (res == EQUIP_ERR_OK && (dest & 255) != slot)
                res = EQUIP_ERR_ITEM_DOESNT_GO_TO_SLOT;
            if (res != EQUIP_ERR_OK)
            {
                failures.push_back({ slot, false, uint32(res) });
                if (!shownError)
                {
                    player->SendEquipError(res, item);
                    shownError = true;
                }
                if (fromWardrobe)
                    PutBack(guid, std::move(taken)); // never left the wardrobe
                else
                    StoreDetached(player, item, trans);
                continue;
            }

            player->EquipItem(dest, item, true);
            if (fromWardrobe)
                trans->Append("DELETE FROM gear_wardrobe_item WHERE item_guid = {}", guid);
        }

        // Phase C: whatever came off and wasn't put back goes to the wardrobe.
        for (auto const& [guid, item] : detached)
            StoreDetached(player, item, trans);

        player->AutoUnequipOffhandIfNeed();
        player->SaveInventoryAndGoldToDB(trans);
        CharacterDatabase.CommitTransaction(trans);
        return true;
    }

    void Mgr::SendEquipResult(Player* player, uint32 setId, std::vector<EquipFailure> const& failures)
    {
        Profile* profile = Find(player);
        if (!profile)
            return;
        auto set = profile->sets.find(setId);
        if (set == profile->sets.end())
            return;

        std::vector<std::string> entries; // "slot,reason" for the addon
        for (EquipFailure const& failure : failures)
            entries.push_back(failure.missing ? Acore::StringFormat("{},missing", failure.slot)
                : Acore::StringFormat("{},{}", failure.slot, failure.result));

        Comms::SendList(player, "EQRES", entries);
        if (failures.empty())
            Comms::SendInfo(player, Acore::StringFormat("Equipped {}.", set->second.name));
        else
            Comms::SendInfo(player, Acore::StringFormat("Equipped {} with {} slot(s) skipped.", set->second.name,
                failures.size()));
    }

    void Mgr::SendSync(Player* player)
    {
        Profile* profile = Find(player);
        if (!profile)
            return;

        Comms::Send(player, "BEGIN");

        std::vector<std::string> sets;
        std::vector<std::string> slots;
        for (auto const& [id, set] : profile->sets)
        {
            sets.push_back(Acore::StringFormat("{},{},{}", id, set.icon, set.name));
            for (auto const& [slot, entry] : set.slots)
            {
                // Where the item is now: E worn, B bags, K bank, W wardrobe, M missing.
                char loc = 'M';
                std::string link;
                if (entry.mode == MODE_ITEM)
                {
                    if (Item* item = player->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(entry.item)))
                    {
                        loc = item->IsEquipped() ? 'E' : Player::IsBankPos(item->GetPos()) ? 'K' : 'B';
                        link = LinkOf(item);
                    }
                    else if (StoredItem const* stored = FindStored(player, entry.item))
                    {
                        loc = 'W';
                        link = stored->link;
                    }
                }
                else
                    loc = '-';
                slots.push_back(Acore::StringFormat("{},{},{},{},{},{}", id, slot, entry.mode, entry.item, loc, link));
            }
        }

        // Oldest item guid first, as the addon got them before the wardrobe was kept in memory.
        std::vector<ObjectGuid::LowType> visible;
        for (auto const& [guid, stored] : _stored)
            if (CanSee(player, stored))
                visible.push_back(guid);
        std::sort(visible.begin(), visible.end());

        std::vector<std::string> items;
        for (ObjectGuid::LowType guid : visible)
            items.push_back(Acore::StringFormat("{},{}", guid, _stored.at(guid).link));

        Comms::SendList(player, "SETS", sets);
        Comms::SendList(player, "SLOTS", slots);
        Comms::SendList(player, "ITEMS", items);
        Comms::Send(player, "END", Acore::StringFormat("{},{}", Cfg().maxSets, Cfg().maxItems));
    }

    void Mgr::HandleAddonMessage(Player* player, std::string const& body)
    {
        if (!Cfg().enable || !Find(player))
            return;

        std::string_view const view(body);
        std::size_t const colon = view.find(':');
        std::string_view const command = view.substr(0, colon);
        std::string_view const payload = colon == std::string_view::npos ? std::string_view() : view.substr(colon + 1);
        std::vector<std::string_view> args = Acore::Tokenize(payload, ',', true);

        auto num = [&args](std::size_t i) -> int32
        {
            return i < args.size() ? Acore::StringTo<int32>(args[i]).value_or(-999) : -999;
        };
        auto guidArg = [&args](std::size_t i) -> ObjectGuid::LowType
        {
            return i < args.size() ? Acore::StringTo<ObjectGuid::LowType>(args[i]).value_or(0) : 0;
        };
        // Free text is everything after the first field, commas included (they are stripped later).
        auto tail = [payload]() -> std::string
        {
            std::size_t const comma = payload.find(',');
            return comma == std::string_view::npos ? std::string() : std::string(payload.substr(comma + 1));
        };

        std::string error;
        bool changed = false;
        if (command == "SYNC")
        {
            SendSync(player);
            return;
        }
        else if (command == "NEW")
            changed = CreateSet(player, std::string(payload), error);
        else if (command == "REN")
            changed = RenameSet(player, uint32(num(0)), tail(), error);
        else if (command == "DEL")
            changed = DeleteSet(player, uint32(num(0)), error);
        else if (command == "ICON")
            changed = SetIcon(player, uint32(num(0)), tail(), error);
        else if (command == "SAVE")
            changed = SaveWorn(player, uint32(num(0)), error);
        else if (command == "SLOT")
            changed = AssignSlot(player, uint32(num(0)), uint8(num(1)), uint8(num(2)), guidArg(3), error);
        else if (command == "SLOTPOS")
            changed = AssignSlotFromPos(player, uint32(num(0)), uint8(num(1)), num(2), num(3), error);
        else if (command == "DEP")
            changed = Deposit(player, num(0), num(1), error);
        else if (command == "WD")
            changed = Withdraw(player, guidArg(0), error);
        else if (command == "EQUIP")
        {
            std::vector<EquipFailure> failures;
            if ((changed = EquipSet(player, uint32(num(0)), failures, error)))
                SendEquipResult(player, uint32(num(0)), failures);
        }
        else
            return;

        if (!error.empty())
            Comms::SendError(player, error);
        if (changed)
            SendSync(player);
    }
}
