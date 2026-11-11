#include "GearWardrobeMgr.h"
#include "Bag.h"
#include "Battleground.h"
#include "CharacterCache.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "GearWardrobeComms.h"
#include "GearWardrobeConfig.h"
#include "GearWardrobeProgression.h"
#include "Item.h"
#include "ItemEnchantmentMgr.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "StringConvert.h"
#include "StringFormat.h"
#include "Tokenize.h"
#include "WorldSession.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <vector>

#if __has_include("PlayerbotAIConfig.h")
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#define GEAR_WARDROBE_PLAYERBOTS
#endif


#if __has_include("PlayerbotsDatabase.h")
#include "PlayerbotsDatabase.h"
#define GEAR_WARDROBE_PLAYERBOTS_DB
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

        // The same link from an item_instance row; `enchantments` holds "id duration charges" for every slot.
        std::string LinkOf(uint32 entry, std::string_view enchantments, int32 randomProperty)
        {
            Enchants ench{};
            std::vector<std::string_view> tokens = Acore::Tokenize(enchantments, ' ', false);
            for (uint8 i = 0; i < MAX_ENCHANTMENT_SLOT && i * MAX_ENCHANTMENT_OFFSET < tokens.size(); ++i)
                ench[i] = Acore::StringTo<uint32>(tokens[i * MAX_ENCHANTMENT_OFFSET]).value_or(0);
            uint32 const suffixFactor = randomProperty < 0 ? GenerateEnchSuffixFactor(entry) : 0;
            return BuildLink(entry, ench, randomProperty, suffixFactor);
        }

        // "1,2,3" for an SQL IN list.
        std::string JoinIds(std::vector<uint32> const& ids)
        {
            std::string out;
            for (uint32 id : ids)
                out += (out.empty() ? "" : ",") + std::to_string(id);
            return out;
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

        // A two-hander that leaves no room for an off-hand item (same rule as Player::CanEquipItem).
        bool NeedsBothHands(Player* player, ItemTemplate const* proto)
        {
            if (proto->InventoryType != INVTYPE_2HWEAPON)
                return false;
            return !player->CanTitanGrip() || proto->SubClass == ITEM_SUBCLASS_WEAPON_POLEARM
                || proto->SubClass == ITEM_SUBCLASS_WEAPON_STAFF
                || proto->SubClass == ITEM_SUBCLASS_WEAPON_FISHING_POLE;
        }

        // A main-hand weapon that keeps the off hand empty (Player::CanEquipItem also refuses an off hand next to any
        // polearm, staff or fishing pole).
        bool BlocksOffHand(Player* player, Item const* mainHand)
        {
            if (!mainHand)
                return false;
            ItemTemplate const* proto = mainHand->GetTemplate();
            return NeedsBothHands(player, proto) || (proto->Class == ITEM_CLASS_WEAPON
                && (proto->SubClass == ITEM_SUBCLASS_WEAPON_POLEARM || proto->SubClass == ITEM_SUBCLASS_WEAPON_STAFF
                    || proto->SubClass == ITEM_SUBCLASS_WEAPON_FISHING_POLE));
        }
    }

    std::string LinkOf(Item const* item)
    {
        Enchants ench{};
        for (uint8 i = 0; i < MAX_ENCHANTMENT_SLOT; ++i)
            ench[i] = item->GetEnchantmentId(EnchantmentSlot(i));
        return BuildLink(item->GetEntry(), ench, item->GetItemRandomPropertyId(), item->GetItemSuffixFactor());
    }

    std::string ItemName(Item const* item, Player* reader)
    {
        ItemTemplate const* proto = item->GetTemplate();
        WorldSession* session = reader->GetSession();

        std::string name = proto->Name1;
        if (ItemLocale const* locale = sObjectMgr->GetItemLocale(proto->ItemId))
            ObjectMgr::GetLocaleString(locale->Name, session->GetSessionDbLocaleIndex(), name);

        // "of the Bear": the client shows a link's text as sent, so the suffix has to be in it.
        char const* suffix = nullptr;
        int32 const randomProperty = item->GetItemRandomPropertyId();
        if (randomProperty > 0)
        {
            if (ItemRandomPropertiesEntry const* entry = sItemRandomPropertiesStore.LookupEntry(randomProperty))
                suffix = entry->Name[session->GetSessionDbcLocale()];
        }
        else if (randomProperty < 0)
        {
            if (ItemRandomSuffixEntry const* entry = sItemRandomSuffixStore.LookupEntry(-randomProperty))
                suffix = entry->Name[session->GetSessionDbcLocale()];
        }
        if (suffix && *suffix)
            name = Acore::StringFormat("{} {}", name, suffix);
        return name;
    }

    std::string ChatLink(Item const* item, Player* reader)
    {
        ItemTemplate const* proto = item->GetTemplate();
        uint32 const color = proto->Quality < MAX_ITEM_QUALITY ? ItemQualityColors[proto->Quality] : 0xffffffff;
        return Acore::StringFormat("|c{:08x}|Hitem:{}:{}|h[{}]|h|r", color, LinkOf(item), reader->GetLevel(),
            ItemName(item, reader));
    }

    char const* SlotName(uint8 slot)
    {
        static std::array<char const*, EQUIPMENT_SLOT_END> const names =
        {
            "Head", "Neck", "Shoulder", "Shirt", "Chest", "Waist", "Legs", "Feet", "Wrist", "Hands",
            "Finger 1", "Finger 2", "Trinket 1", "Trinket 2", "Back", "Main Hand", "Off Hand", "Ranged", "Tabard"
        };
        return slot < names.size() ? names[slot] : "?";
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

    bool Mgr::MayCommand(Player* master, Player* bot)
    {
        if (master == bot)
            return true;
#ifdef GEAR_WARDROBE_PLAYERBOTS
        // Altbots can be on the player's account, a linked account or a guildmate's, so ask playerbots who is in
        // charge of the bot.
        if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
            return botAI->GetMaster() == master;
#endif
        return master->GetSession()->GetAccountId() == bot->GetSession()->GetAccountId();
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
            "ii.text, w.item_guid, ii.itemEntry, w.owner_guid, c.account "
            "FROM gear_wardrobe_item w INNER JOIN item_instance ii ON ii.guid = w.item_guid "
            "INNER JOIN characters c ON c.guid = w.owner_guid");

        LoadGroups();
        if (!result)
            return;

        do
        {
            Field* fields = result->Fetch();
            ObjectGuid::LowType const guid = fields[11].Get<uint32>();
            uint32 const entry = fields[12].Get<uint32>();
            ObjectGuid::LowType const depositor = fields[13].Get<uint32>();
            uint32 const account = fields[14].Get<uint32>(); // 0 for a character deleted with CharDelete.Method 1

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
            _stored[guid] = { item, depositor, account, LinkOf(item) };
        } while (result->NextRow());

        LOG_INFO("module", "mod-gear-wardrobe: {} stored items loaded.", _stored.size());

        // Taken-out items: drop rows whose item or holder is gone, or whose item is stored again.
        CharacterDatabase.DirectExecute("DELETE o FROM gear_wardrobe_out o "
            "LEFT JOIN item_instance ii ON ii.guid = o.item_guid LEFT JOIN characters c ON c.guid = o.holder_guid "
            "WHERE ii.guid IS NULL OR c.guid IS NULL");
        CharacterDatabase.DirectExecute("DELETE o FROM gear_wardrobe_out o "
            "INNER JOIN gear_wardrobe_item w ON w.item_guid = o.item_guid");
        if (QueryResult taken = CharacterDatabase.Query("SELECT o.item_guid, o.holder_guid, c.account, o.link "
            "FROM gear_wardrobe_out o INNER JOIN characters c ON c.guid = o.holder_guid"))
        {
            do
            {
                Field* fields = taken->Fetch();
                _taken[fields[0].Get<uint32>()] = { fields[1].Get<uint32>(), fields[2].Get<uint32>(),
                    fields[3].Get<std::string>() };
            } while (taken->NextRow());
        }
    }

    void Mgr::OnLogin(Player* player)
    {
        ObjectGuid::LowType const owner = player->GetGUID().GetCounter();
        Profile& profile = _profiles[owner];
        profile = Profile();
        profile.account = player->GetSession()->GetAccountId();

        // Account links change rarely and only from a logged-in character, so a login is a good time to re-read them.
        LoadGroups();

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

        // Sets edited while offline (or kept since the last logout) are newer than what the database may hold yet.
        if (auto cached = _offline.find(owner); cached != _offline.end())
        {
            profile.sets = std::move(cached->second.sets);
            _offline.erase(cached);
        }
        else
            LoadSets(owner, profile);

        // Bot sessions are headless and have no addon to talk to.
        if (!player->GetSession()->IsHeadless())
            SendSync(player);
    }

    void Mgr::LoadSets(ObjectGuid::LowType owner, Profile& profile)
    {
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
    }

    void Mgr::OnLogout(Player* player)
    {
        // Kept for the offline view: the last saves may still be on their way to the database.
        auto itr = _profiles.find(player->GetGUID().GetCounter());
        if (itr == _profiles.end())
            return;
        _offline[itr->first] = std::move(itr->second);
        _profiles.erase(itr);
    }

    void Mgr::OnDelete(ObjectGuid::LowType guid)
    {
        // A soft delete (CharDelete.Method = 1) never reaches OnDeleteFromDB and would leave the items on a character
        // nobody can see, so the shared wardrobe hands them off here, before Player::DeleteFromDB builds its own
        // transaction. Committed synchronously: an async commit could run after the core's async delete of the
        // character's item_instance rows.
        if (!Cfg().sharedPool)
            return;

        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        HandOff(guid, trans);
        // What this character took out stays on it (or goes with it); nobody else will get it back.
        ForgetHeldBy(guid, trans);
        CharacterDatabase.DirectCommitTransaction(trans);
    }

    void Mgr::OnDeleteFromDB(CharacterDatabaseTransaction trans, ObjectGuid::LowType guid)
    {
        _profiles.erase(guid);
        _offline.erase(guid);

        // Items still stored under this character (e.g. after a console erase, which skips OnDelete) are saved again
        // for another character of the group; `trans` already deletes the character's item_instance rows and runs
        // these statements after that. Without a shared wardrobe, or with no one to give them to, they go with the
        // character as before.
        if (!Cfg().sharedPool || !HandOff(guid, trans))
        {
            for (auto itr = _stored.begin(); itr != _stored.end();)
            {
                ObjectGuid::LowType const itemGuid = itr->first;
                bool const theirs = itr->second.depositor == guid;
                ++itr;
                if (theirs)
                    DropStored(itemGuid);
            }
        }

        ForgetHeldBy(guid, trans);

        // After HandOff, whose rows now name the new owner.
        trans->Append("DELETE FROM gear_wardrobe_item WHERE owner_guid = {}", guid);
        trans->Append("DELETE FROM gear_wardrobe_slot WHERE owner_guid = {}", guid);
        trans->Append("DELETE FROM gear_wardrobe_set WHERE owner_guid = {}", guid);
    }

    bool Mgr::HandOff(ObjectGuid::LowType deleted, CharacterDatabaseTransaction trans)
    {
        uint32 account = 0;
        std::vector<ObjectGuid::LowType> theirs;
        for (auto const& [guid, stored] : _stored)
        {
            if (stored.depositor == deleted)
            {
                theirs.push_back(guid);
                account = stored.account;
            }
        }
        // Account 0 is a character that was already soft-deleted: its items were never visible to anyone.
        if (theirs.empty() || !account)
            return false;

        // Prefer a character on the same account, then the oldest. Soft-deleted characters have account 0.
        std::vector<uint32> const accounts = GroupAccounts(account);
        QueryResult result = CharacterDatabase.Query("SELECT guid, account FROM characters "
            "WHERE account IN ({}) AND guid <> {} ORDER BY account = {} DESC, guid LIMIT 1",
            JoinIds(accounts), deleted, account);
        if (!result)
            return false;

        ObjectGuid::LowType const heir = result->Fetch()[0].Get<uint32>();
        uint32 const heirAccount = result->Fetch()[1].Get<uint32>();
        for (ObjectGuid::LowType guid : theirs)
        {
            StoredItem& stored = _stored.at(guid);
            stored.depositor = heir;
            stored.account = heirAccount;
            stored.item->SetOwnerGUID(ObjectGuid::Create<HighGuid::Player>(heir));
            // ITEM_NEW saves with REPLACE, which also works after `trans` has deleted the row.
            stored.item->FSetState(ITEM_NEW);
            stored.item->SaveToDB(trans);
            trans->Append("UPDATE gear_wardrobe_item SET owner_guid = {} WHERE item_guid = {}", heir, guid);
        }

        LOG_INFO("module", "mod-gear-wardrobe: {} stored items of deleted character {} given to character {}.",
            theirs.size(), deleted, heir);
        return true;
    }

    void Mgr::LoadGroups()
    {
        _groupOf.clear();

#ifdef GEAR_WARDROBE_PLAYERBOTS_DB
        if (!Cfg().sharedPool || !Cfg().sharedLinked)
            return;

        QueryResult result = PlayerbotsDatabase.Query(
            "SELECT account_id, linked_account_id FROM playerbots_account_links");
        if (!result)
            return;

        // Union-find; the lowest account id of a group is its root, so group ids don't depend on row order.
        auto root = [this](uint32 account)
        {
            uint32 top = account;
            for (auto itr = _groupOf.find(top); itr != _groupOf.end() && itr->second != top; itr = _groupOf.find(top))
                top = itr->second;
            return top;
        };

        do
        {
            Field* fields = result->Fetch();
            uint32 const a = fields[0].Get<uint32>();
            uint32 const b = fields[1].Get<uint32>();
            _groupOf.emplace(a, a);
            _groupOf.emplace(b, b);
            uint32 const ra = root(a);
            uint32 const rb = root(b);
            if (ra != rb)
                _groupOf[std::max(ra, rb)] = std::min(ra, rb);
        } while (result->NextRow());

        // Every key exists already, so this only changes values.
        for (auto& [account, group] : _groupOf)
            group = root(account);
#endif
    }

    uint32 Mgr::GroupOf(uint32 account) const
    {
        auto itr = _groupOf.find(account);
        return itr != _groupOf.end() ? itr->second : account;
    }

    std::vector<uint32> Mgr::GroupAccounts(uint32 account) const
    {
        std::vector<uint32> accounts = { account };
        uint32 const group = GroupOf(account);
        for (auto const& [other, otherGroup] : _groupOf)
            if (otherGroup == group && other != account)
                accounts.push_back(other);
        return accounts;
    }

    std::string Mgr::HolderOf(Player* player, ObjectGuid::LowType guid) const
    {
        if (!Cfg().sharedPool)
            return {};

        std::vector<uint32> const accounts = GroupAccounts(player->GetSession()->GetAccountId());
        ObjectGuid const itemGuid = ObjectGuid::Create<HighGuid::Item>(guid);

        // Online characters: their inventory in memory is the truth, the database may lag behind.
        if (QueryResult result = CharacterDatabase.Query("SELECT guid, name FROM characters "
            "WHERE account IN ({}) AND online <> 0", JoinIds(accounts)))
        {
            do
            {
                Field* fields = result->Fetch();
                ObjectGuid const charGuid = ObjectGuid::Create<HighGuid::Player>(fields[0].Get<uint32>());
                if (charGuid == player->GetGUID())
                    continue;
                if (Player* other = ObjectAccessor::FindConnectedPlayer(charGuid))
                    if (other->GetItemByGuid(itemGuid))
                        return fields[1].Get<std::string>();
            } while (result->NextRow());
        }

        if (QueryResult result = CharacterDatabase.Query("SELECT c.name FROM character_inventory ci "
            "INNER JOIN characters c ON c.guid = ci.guid WHERE ci.item = {} AND c.account IN ({}) AND c.online = 0",
            guid, JoinIds(accounts)))
            return result->Fetch()[0].Get<std::string>();

        return {};
    }

    void Mgr::RecordTaken(Player* taker, Item* item, CharacterDatabaseTransaction trans)
    {
        if (!Cfg().sharedPool)
            return;

        ObjectGuid::LowType const guid = item->GetGUID().GetCounter();
        TakenItem& taken = _taken[guid];
        taken = { taker->GetGUID().GetCounter(), taker->GetSession()->GetAccountId(), LinkOf(item) };
        // The link is digits, ':' and '-' only (BuildLink), so it needs no escaping.
        trans->Append("REPLACE INTO gear_wardrobe_out (item_guid, holder_guid, link, taken) "
            "VALUES ({}, {}, '{}', UNIX_TIMESTAMP())", guid, taken.holder, taken.link);
    }

    void Mgr::ForgetTaken(ObjectGuid::LowType guid, CharacterDatabaseTransaction trans)
    {
        if (_taken.erase(guid))
            trans->Append("DELETE FROM gear_wardrobe_out WHERE item_guid = {}", guid);
    }

    void Mgr::ForgetHeldBy(ObjectGuid::LowType holder, CharacterDatabaseTransaction trans)
    {
        bool any = false;
        for (auto itr = _taken.begin(); itr != _taken.end();)
        {
            if (itr->second.holder == holder)
            {
                itr = _taken.erase(itr);
                any = true;
            }
            else
                ++itr;
        }
        if (any)
            trans->Append("DELETE FROM gear_wardrobe_out WHERE holder_guid = {}", holder);
    }

    std::vector<ObjectGuid::LowType> Mgr::ResolveTaken(Player* viewer)
    {
        return ResolveTaken(viewer->GetGUID().GetCounter(), viewer->GetSession()->GetAccountId());
    }

    std::vector<ObjectGuid::LowType> Mgr::ResolveTaken(ObjectGuid::LowType self, uint32 account)
    {
        std::vector<ObjectGuid::LowType> shown;
        if (!Cfg().sharedPool)
            return shown;

        uint32 const group = GroupOf(account);
        auto inGroup = [this, group](uint32 account) { return account && GroupOf(account) == group; };

        std::vector<ObjectGuid::LowType> offline;
        std::vector<ObjectGuid::LowType> gone;
        for (auto& [guid, taken] : _taken)
        {
            if (!inGroup(taken.account))
                continue;

            // Online: the inventory in memory is the truth (the database may lag behind).
            ObjectGuid const holderGuid = ObjectGuid::Create<HighGuid::Player>(taken.holder);
            if (Player* holder = ObjectAccessor::FindConnectedPlayer(holderGuid))
            {
                if (Item* item = holder->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(guid)))
                {
                    taken.link = LinkOf(item);
                    if (taken.holder != self)
                        shown.push_back(guid);
                }
                else
                    gone.push_back(guid); // sold, destroyed, mailed or traded away
            }
            else
                offline.push_back(guid);
        }

        if (!offline.empty())
        {
            std::unordered_map<ObjectGuid::LowType, ObjectGuid::LowType> where; // item -> character
            if (QueryResult result = CharacterDatabase.Query(
                "SELECT item, guid FROM character_inventory WHERE item IN ({})", JoinIds(offline)))
            {
                do
                {
                    Field* fields = result->Fetch();
                    where[fields[0].Get<uint32>()] = fields[1].Get<uint32>();
                } while (result->NextRow());
            }

            for (ObjectGuid::LowType guid : offline)
            {
                auto found = where.find(guid);
                if (found == where.end())
                {
                    gone.push_back(guid);
                    continue;
                }

                TakenItem& taken = _taken.at(guid);
                if (found->second != taken.holder)
                {
                    // Passed on while its holder was offline (mail, trade): follow it.
                    taken.holder = found->second;
                    taken.account = sCharacterCache->GetCharacterAccountIdByGuid(
                        ObjectGuid::Create<HighGuid::Player>(found->second));
                    CharacterDatabase.Execute("UPDATE gear_wardrobe_out SET holder_guid = {} WHERE item_guid = {}",
                        taken.holder, guid);
                    if (!inGroup(taken.account))
                        continue;
                }
                if (taken.holder != self)
                    shown.push_back(guid);
            }
        }

        for (ObjectGuid::LowType guid : gone)
        {
            _taken.erase(guid);
            CharacterDatabase.Execute("DELETE FROM gear_wardrobe_out WHERE item_guid = {}", guid);
        }

        std::sort(shown.begin(), shown.end());
        return shown;
    }

    std::string Mgr::KnownLink(ObjectGuid::LowType guid) const
    {
        if (auto stored = _stored.find(guid); stored != _stored.end())
            return stored->second.link;
        if (auto taken = _taken.find(guid); taken != _taken.end())
            return taken->second.link;

        // An item that never went through the wardrobe, or was taken before taken-out items were tracked.
        if (QueryResult result = CharacterDatabase.Query(
            "SELECT itemEntry, enchantments, randomPropertyId FROM item_instance WHERE guid = {}", guid))
        {
            Field* fields = result->Fetch();
            return LinkOf(fields[0].Get<uint32>(), fields[1].Get<std::string_view>(), fields[2].Get<int32>());
        }
        return {};
    }

    ObjectGuid::LowType Mgr::FindSpare(Player* player, GearSet const& set, ObjectGuid::LowType wanted,
        std::set<ObjectGuid::LowType> const& claimed) const
    {
        std::string const link = KnownLink(wanted);
        if (link.empty())
            return 0;

        // Two identical rings in one set must stay two items.
        auto usable = [&set, &claimed, wanted](ObjectGuid::LowType guid)
        {
            return guid != wanted && !claimed.count(guid)
                && std::none_of(set.slots.begin(), set.slots.end(), [guid](auto const& slot)
                {
                    return slot.second.mode == MODE_ITEM && slot.second.item == guid;
                });
        };

        // A copy on the character itself first (worn or in the bags, not the bank): nothing else has to move.
        uint32 const entry = Acore::StringTo<uint32>(std::string_view(link).substr(0, link.find(':'))).value_or(0);
        auto carried = [&](Item* item)
        {
            return item && item->GetEntry() == entry && usable(item->GetGUID().GetCounter()) && LinkOf(item) == link;
        };
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot); carried(item))
                return item->GetGUID().GetCounter();
        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
            if (Bag* bag = player->GetBagByPos(bagSlot))
                for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                    if (Item* item = bag->GetItemByPos(uint8(slot)); carried(item))
                        return item->GetGUID().GetCounter();

        ObjectGuid::LowType best = 0;
        for (auto const& [guid, stored] : _stored)
            if (stored.link == link && usable(guid) && CanSee(player, stored) && (!best || guid < best))
                best = guid;
        return best;
    }

    Profile* Mgr::Find(Player* player)
    {
        auto itr = _profiles.find(player->GetGUID().GetCounter());
        return itr != _profiles.end() ? &itr->second : nullptr;
    }

    Subject Mgr::Online(Player* player)
    {
        Subject subject;
        subject.guid = player->GetGUID().GetCounter();
        subject.account = player->GetSession()->GetAccountId();
        subject.player = player;
        subject.profile = Find(player);
        subject.name = player->GetName();
        return subject;
    }

    bool Mgr::CanSee(Player* player, StoredItem const& stored) const
    {
        return CanSee(player->GetGUID().GetCounter(), player->GetSession()->GetAccountId(), stored);
    }

    bool Mgr::CanSee(ObjectGuid::LowType owner, uint32 account, StoredItem const& stored) const
    {
        if (!Cfg().sharedPool)
            return stored.depositor == owner;

        // Account 0: stored by a character that was soft-deleted before the wardrobe was shared.
        return stored.account && GroupOf(account) == GroupOf(stored.account);
    }

    StoredItem const* Mgr::FindStored(Player* player, ObjectGuid::LowType guid) const
    {
        return FindStored(player->GetGUID().GetCounter(), player->GetSession()->GetAccountId(), guid);
    }

    StoredItem const* Mgr::FindStored(ObjectGuid::LowType owner, uint32 account, ObjectGuid::LowType guid) const
    {
        auto itr = _stored.find(guid);
        return itr != _stored.end() && CanSee(owner, account, itr->second) ? &itr->second : nullptr;
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
        return profile ? FindNamed(*profile, name) : nullptr;
    }

    GearSet* Mgr::FindNamed(Profile& profile, std::string const& name)
    {
        std::string wanted = name;
        std::transform(wanted.begin(), wanted.end(), wanted.begin(), ::tolower);
        for (auto& [id, set] : profile.sets)
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
        SaveSet(player->GetGUID().GetCounter(), set);
    }

    void Mgr::SaveSet(ObjectGuid::LowType owner, GearSet const& set)
    {
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
        return CreateSet(Online(player), rawName, error);
    }

    bool Mgr::CreateSet(Subject const& subject, std::string const& rawName, std::string& error)
    {
        Profile* profile = subject.profile;
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
        if (FindNamed(*profile, name))
        {
            error = "You already have a set with that name.";
            return false;
        }

        uint32 const id = profile->sets.empty() ? 1 : profile->sets.rbegin()->first + 1;
        GearSet& set = profile->sets[id];
        set.id = id;
        set.name = name;

        // A new set starts as what the character is wearing; empty slots are left ignored.
        if (Player* player = subject.player)
        {
            for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
                if (Item* worn = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                    set.slots[slot] = { MODE_ITEM, worn->GetGUID().GetCounter() };
        }
        else
        {
            for (OfflineItem const& item : LoadOfflineInventory(subject.guid))
                if (item.loc == 'E')
                    set.slots[item.slot] = { MODE_ITEM, item.guid };
        }

        SaveSet(subject.guid, set);
        return true;
    }

    bool Mgr::RenameSet(Player* player, uint32 setId, std::string const& rawName, std::string& error)
    {
        return RenameSet(Online(player), setId, rawName, error);
    }

    bool Mgr::RenameSet(Subject const& subject, uint32 setId, std::string const& rawName, std::string& error)
    {
        Profile* profile = subject.profile;
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
        if (GearSet const* other = FindNamed(*profile, name); other && other->id != setId)
        {
            error = "You already have a set with that name.";
            return false;
        }

        set->name = name;
        SaveSet(subject.guid, *set);
        return true;
    }

    bool Mgr::DeleteSet(Player* player, uint32 setId, std::string& error)
    {
        return DeleteSet(Online(player), setId, error);
    }

    bool Mgr::DeleteSet(Subject const& subject, uint32 setId, std::string& error)
    {
        Profile* profile = subject.profile;
        if (!profile || !FindSet(*profile, setId, error))
            return false;

        // The set's items stay where they are; stored ones remain in the wardrobe.
        ObjectGuid::LowType const owner = subject.guid;
        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        trans->Append("DELETE FROM gear_wardrobe_slot WHERE owner_guid = {} AND set_id = {}", owner, setId);
        trans->Append("DELETE FROM gear_wardrobe_set WHERE owner_guid = {} AND set_id = {}", owner, setId);
        CharacterDatabase.CommitTransaction(trans);
        profile->sets.erase(setId);
        return true;
    }

    bool Mgr::SetIcon(Player* player, uint32 setId, std::string const& icon, std::string& error)
    {
        return SetIcon(Online(player), setId, icon, error);
    }

    bool Mgr::SetIcon(Subject const& subject, uint32 setId, std::string const& icon, std::string& error)
    {
        Profile* profile = subject.profile;
        if (!profile)
            return false;

        GearSet* set = FindSet(*profile, setId, error);
        if (!set)
            return false;

        set->icon = CleanIcon(icon);
        SaveSet(subject.guid, *set);
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
        return AssignSlot(Online(player), setId, slot, mode, itemGuid, error);
    }

    bool Mgr::AssignSlot(Subject const& subject, uint32 setId, uint8 slot, uint8 mode, ObjectGuid::LowType itemGuid,
        std::string& error)
    {
        Profile* profile = subject.profile;
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
            // Worn or in the bags (not the bank), or stored where the character can see it.
            ItemTemplate const* proto = nullptr;
            if (StoredItem const* stored = FindStored(subject.guid, subject.account, itemGuid))
                proto = stored->item->GetTemplate();
            else if (subject.player)
            {
                if (Item* carried = FindCarried(subject.player, itemGuid))
                    proto = carried->GetTemplate();
            }
            else
            {
                for (OfflineItem const& item : LoadOfflineInventory(subject.guid))
                    if (item.guid == itemGuid && item.loc != 'K')
                        proto = item.proto;
            }

            if (!proto)
            {
                error = "That item isn't on the character or in the wardrobe.";
                return false;
            }
            if (subject.player ? subject.player->FindEquipSlot(proto, slot, true) != slot : !FitsSlot(proto, slot))
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

        SaveSet(subject.guid, *set);
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
        stored = { item, depositor, player->GetSession()->GetAccountId(), LinkOf(item) };
        ForgetTaken(guid, trans);
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
        return DepositItem(player, item, error);
    }

    bool Mgr::DepositItem(Player* player, Item* item, std::string& error)
    {
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
        player->MoveItemFromInventory(item->GetBagSlot(), item->GetSlot(), true);
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
        RecordTaken(player, item, trans);
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

        // A slot whose own item is out of reach takes an identical copy (FindSpare), and the set keeps that copy.
        std::set<ObjectGuid::LowType> claimed;
        bool repointed = false;
        for (auto& [slot, entry] : targets)
        {
            if (entry.mode != MODE_ITEM || FindCarried(player, entry.item) || FindStored(player, entry.item))
                continue;
            if (ObjectGuid::LowType const spare = FindSpare(player, *set, entry.item, claimed))
            {
                claimed.insert(spare);
                entry.item = spare;
                set->slots[slot].item = spare;
                repointed = true;
            }
        }
        if (repointed)
            SaveSet(player, *set);

        // A two-hander in the main hand pushes out whatever the set leaves in the off hand.
        auto mh = targets.find(EQUIPMENT_SLOT_MAINHAND);
        if (mh != targets.end() && mh->second.mode == MODE_ITEM)
        {
            ItemTemplate const* proto = nullptr;
            if (Item* carried = FindCarried(player, mh->second.item))
                proto = carried->GetTemplate();
            else if (StoredItem const* stored = FindStored(player, mh->second.item))
                proto = stored->item->GetTemplate();

            if (proto && NeedsBothHands(player, proto))
                targets[EQUIPMENT_SLOT_OFFHAND] = { MODE_EMPTY, 0 };
        }

        std::vector<ObjectGuid::LowType> removed;
        ApplyTargets(player, targets, failures, removed);
        return true;
    }

    void Mgr::ApplyTargets(Player* player, std::map<uint8, SlotEntry> const& targets,
        std::vector<EquipFailure>& failures, std::vector<ObjectGuid::LowType>& removed)
    {
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
                // Sold, destroyed, banked, or (shared wardrobe) taken by another character: leave the slot alone.
                failures.push_back({ slot, true, 0, HolderOf(player, entry.item) });
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
            bool fromBags = false;
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
                fromBags = true;
            }
            else if ((item = TakeStored(player, guid, taken)))
                fromWardrobe = true;

            if (!item)
            {
                failures.push_back({ slot, true, 0, {} });
                continue;
            }

            uint16 dest = 0;
            InventoryResult res = player->CanEquipItem(slot, dest, item, false);
            if (res == EQUIP_ERR_OK && (dest & 255) != slot)
                res = EQUIP_ERR_ITEM_DOESNT_GO_TO_SLOT;
            if (res != EQUIP_ERR_OK)
            {
                failures.push_back({ slot, false, uint32(res), {} });
                if (!shownError)
                {
                    player->SendEquipError(res, item);
                    shownError = true;
                }
                if (fromWardrobe)
                {
                    PutBack(guid, std::move(taken)); // never left the wardrobe
                    continue;
                }

                // Taken from the bags (or another slot): back into the bags if it fits, so a refused item stays put.
                ItemPosCountVec back;
                if (fromBags && player->CanStoreItem(NULL_BAG, NULL_SLOT, back, item, false) == EQUIP_ERR_OK)
                    player->MoveItemToInventory(back, item, true, true);
                else
                {
                    StoreDetached(player, item, trans);
                    removed.push_back(guid);
                }
                continue;
            }

            player->EquipItem(dest, item, true);
            if (fromWardrobe)
            {
                trans->Append("DELETE FROM gear_wardrobe_item WHERE item_guid = {}", guid);
                RecordTaken(player, item, trans);
            }
        }

        // Phase C: whatever came off and wasn't put back goes to the wardrobe.
        for (auto const& [guid, item] : detached)
        {
            StoreDetached(player, item, trans);
            removed.push_back(guid);
        }

        player->AutoUnequipOffhandIfNeed();
        player->SaveInventoryAndGoldToDB(trans);
        CharacterDatabase.CommitTransaction(trans);
    }

    std::vector<ReachableItem> Mgr::Reachable(Player* player) const
    {
        std::vector<ReachableItem> items;
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
            if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                items.push_back({ item, false });
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                items.push_back({ item, false });
        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
            if (Bag* bag = player->GetBagByPos(bagSlot))
                for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                    if (Item* item = bag->GetItemByPos(uint8(slot)))
                        items.push_back({ item, false });

        std::vector<ObjectGuid::LowType> visible;
        for (auto const& [guid, stored] : _stored)
            if (CanSee(player, stored))
                visible.push_back(guid);
        std::sort(visible.begin(), visible.end());
        for (ObjectGuid::LowType guid : visible)
            items.push_back({ _stored.at(guid).item, true });
        return items;
    }

    uint8 Mgr::FirstSlot(Player* player, Item const* item)
    {
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
            if (player->FindEquipSlot(item->GetTemplate(), slot, true) == slot)
                return slot;
        return NULL_SLOT;
    }

    uint32 Mgr::CanWear(Player* player, Item* item) const
    {
        uint8 const slot = FirstSlot(player, item);
        if (slot == NULL_SLOT)
            return EQUIP_ERR_ITEM_CANT_BE_EQUIPPED; // another class's relic, say
        ItemTemplate const* proto = item->GetTemplate();
        if (proto->InventoryType == INVTYPE_WEAPONOFFHAND && !player->CanDualWield())
            return EQUIP_ERR_CANT_DUAL_WIELD;

        // A stored item still belongs to its depositor (see TakeStored), and binding checks compare the owner.
        ObjectGuid const owner = item->GetOwnerGUID();
        item->SetOwnerGUID(player->GetGUID());
        uint32 res = player->CanUseItem(item, false);

        // Other modules' rules. mod-individual-progression's expansion lock is asked directly: through the hook it
        // would tell the character "You have not reached the expansion..." for every item checked.
        bool notLoading = true; // the hook's "check everything" mode
        if (Progression::Present())
        {
            if (res == EQUIP_ERR_OK && Progression::IsLockedExpansionGear(player, proto))
                res = WEAR_LOCKED_EXPANSION;
            notLoading = false; // IP then skips its lock (checked above) and keeps its other checks
        }
        uint16 dest = 0;
        if (res == EQUIP_ERR_OK && !sScriptMgr->OnPlayerCanEquipItem(player, slot, dest, item, true, notLoading))
            res = EQUIP_ERR_CANT_DO_RIGHT_NOW;
        item->SetOwnerGUID(owner);
        return res;
    }

    bool Mgr::Wear(Player* player, ObjectGuid::LowType guid, std::vector<uint8> const& allowed,
        std::set<uint8> const& keep, WearResult& result, std::string& error)
    {
        if (!Find(player) || !CanChangeGear(player, error))
            return false;

        Item* item = FindCarried(player, guid);
        if (!item)
            if (StoredItem const* stored = FindStored(player, guid))
                item = stored->item;
        if (!item)
        {
            error = "That item isn't on me or in the wardrobe.";
            return false;
        }
        ItemTemplate const* proto = item->GetTemplate();

        std::vector<uint8> slots;
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
            if ((allowed.empty() || std::find(allowed.begin(), allowed.end(), slot) != allowed.end())
                && player->FindEquipSlot(proto, slot, true) == slot)
                slots.push_back(slot);
        if (slots.empty())
        {
            error = FirstSlot(player, item) == NULL_SLOT ? "I can't wear that." : "That doesn't go in that slot.";
            return false;
        }
        if (item->IsEquipped() && std::find(slots.begin(), slots.end(), item->GetSlot()) != slots.end())
        {
            error = "I'm already wearing that.";
            return false;
        }

        // A free slot first, then one this whisper hasn't just filled, then the first one.
        auto isFree = [player](uint8 slot)
        {
            return !player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot)
                && (slot != EQUIPMENT_SLOT_OFFHAND || !player->IsTwoHandUsed());
        };
        auto pick = std::find_if(slots.begin(), slots.end(), isFree);
        if (pick == slots.end())
            pick = std::find_if(slots.begin(), slots.end(), [&keep](uint8 slot) { return !keep.count(slot); });
        uint8 const slot = pick != slots.end() ? *pick : slots.front();
        result.slot = slot;

        Item* mainHand = player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
        if (slot == EQUIPMENT_SLOT_OFFHAND && mainHand != item && BlocksOffHand(player, mainHand))
        {
            error = "My main-hand weapon leaves no room for that. Have me wear a one-hander first.";
            return false;
        }

        // Checked before anything moves: once the old item is off, a refusal would leave the slot empty. Same check
        // as the client's own swap, except that a two-hander's off hand goes to the wardrobe, not the bags.
        ObjectGuid const owner = item->GetOwnerGUID();
        item->SetOwnerGUID(player->GetGUID()); // binding checks compare the owner (see CanWear)
        uint16 dest = 0;
        InventoryResult res = player->CanEquipItem(slot, dest, item, true, true);
        item->SetOwnerGUID(owner);
        bool const pushesOffHand = slot == EQUIPMENT_SLOT_MAINHAND && NeedsBothHands(player, proto)
            && player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
        if (pushesOffHand && (res == EQUIP_ERR_ITEMS_CANT_BE_SWAPPED || res == EQUIP_ERR_INVENTORY_FULL))
            res = EQUIP_ERR_OK;
        if (res != EQUIP_ERR_OK)
        {
            result.failure = EquipFailure{ slot, false, uint32(res), {} };
            return false;
        }

        std::map<uint8, SlotEntry> targets;
        targets[slot] = { MODE_ITEM, guid };
        if (pushesOffHand)
            targets[EQUIPMENT_SLOT_OFFHAND] = { MODE_EMPTY, 0 };

        std::vector<EquipFailure> failures;
        ApplyTargets(player, targets, failures, result.removed);
        for (EquipFailure const& failure : failures)
        {
            if (failure.slot == slot)
            {
                result.failure = failure;
                return false;
            }
        }
        return true;
    }

    Item const* Mgr::GetStored(Player* player, ObjectGuid::LowType guid) const
    {
        StoredItem const* stored = FindStored(player, guid);
        return stored ? stored->item : nullptr;
    }

    bool Mgr::Store(Player* player, ObjectGuid::LowType guid, std::string& error)
    {
        if (!Find(player))
            return false;

        Item* item = FindCarried(player, guid);
        if (!item)
        {
            error = FindStored(player, guid) ? "That's already in the wardrobe." : "That item isn't on me.";
            return false;
        }
        return DepositItem(player, item, error);
    }

    void Mgr::SendEquipResult(Player* viewer, Player* target, uint32 setId, std::vector<EquipFailure> const& failures)
    {
        Profile* profile = Find(target);
        if (!profile)
            return;
        auto set = profile->sets.find(setId);
        if (set == profile->sets.end())
            return;

        // "slot,reason" for the addon: "missing", "held,<name>" (shared wardrobe) or an InventoryResult.
        std::vector<std::string> entries;
        for (EquipFailure const& failure : failures)
        {
            if (!failure.missing)
                entries.push_back(Acore::StringFormat("{},{}", failure.slot, failure.result));
            else if (failure.holder.empty())
                entries.push_back(Acore::StringFormat("{},missing", failure.slot));
            else
                entries.push_back(Acore::StringFormat("{},held,{}", failure.slot, failure.holder));
        }

        Comms::SendList(viewer, "EQRES", entries);
        std::string const who = target == viewer ? "" : Acore::StringFormat("{}: ", target->GetName());
        if (failures.empty())
            Comms::SendInfo(viewer, Acore::StringFormat("{}Equipped {}.", who, set->second.name));
        else
            Comms::SendInfo(viewer, Acore::StringFormat("{}Equipped {} with {} slot(s) skipped.", who,
                set->second.name, failures.size()));
    }

    bool Mgr::SamePool(Player* viewer, Subject const& target) const
    {
        if (viewer->GetGUID().GetCounter() == target.guid)
            return true;
        return Cfg().sharedPool && GroupOf(viewer->GetSession()->GetAccountId()) == GroupOf(target.account);
    }

    bool Mgr::ResolveTarget(Player* viewer, ObjectGuid::LowType guid, Subject& out, std::string& error)
    {
        if (!guid || guid == viewer->GetGUID().GetCounter())
        {
            out = Online(viewer);
            return true;
        }

        if (Player* target = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(guid)))
        {
            if (!Find(target))
            {
                error = Acore::StringFormat("{} has no gear wardrobe.", target->GetName());
                return false;
            }
            if (!MayCommand(viewer, target))
            {
                error = Acore::StringFormat("You aren't {}'s master.", target->GetName());
                return false;
            }
            out = Online(target);
            return true;
        }
        return LoadOffline(viewer, guid, out, error);
    }

    std::vector<uint32> Mgr::OfflineAccounts(Player* viewer) const
    {
        uint32 const own = viewer->GetSession()->GetAccountId();
        std::vector<uint32> accounts = Cfg().sharedPool ? GroupAccounts(own) : std::vector<uint32>{ own };
#ifdef GEAR_WARDROBE_PLAYERBOTS
        accounts.erase(std::remove_if(accounts.begin(), accounts.end(), [](uint32 account)
        {
            return sPlayerbotAIConfig.IsInRandomAccountList(account);
        }), accounts.end());
#endif
        return accounts;
    }

    bool Mgr::LoadOffline(Player* viewer, ObjectGuid::LowType guid, Subject& out, std::string& error)
    {
        QueryResult result = CharacterDatabase.Query(
            "SELECT account, name, class, race, level FROM characters WHERE guid = {}", guid);
        if (!result)
        {
            error = "That character no longer exists.";
            return false;
        }

        Field* fields = result->Fetch();
        uint32 const account = fields[0].Get<uint32>();
        std::vector<uint32> const allowed = OfflineAccounts(viewer);
        if (!account || std::find(allowed.begin(), allowed.end(), account) == allowed.end())
        {
            error = "That character doesn't share your wardrobe.";
            return false;
        }

        auto [itr, added] = _offline.try_emplace(guid);
        if (added)
            LoadSets(guid, itr->second);
        itr->second.account = account;

        out = Subject();
        out.guid = guid;
        out.account = account;
        out.profile = &itr->second;
        out.name = fields[1].Get<std::string>();
        out.cls = fields[2].Get<uint8>();
        out.race = fields[3].Get<uint8>();
        out.level = fields[4].Get<uint8>();
        return true;
    }

    std::vector<OfflineItem> Mgr::LoadOfflineInventory(ObjectGuid::LowType owner) const
    {
        std::vector<OfflineItem> items;
        QueryResult result = CharacterDatabase.Query("SELECT ci.bag, ci.slot, ci.item, ii.itemEntry, ii.enchantments, "
            "ii.randomPropertyId FROM character_inventory ci INNER JOIN item_instance ii ON ii.guid = ci.item "
            "WHERE ci.guid = {}", owner);
        if (!result)
            return items;

        // Rows in a bag name the bag's item guid; where that bag sits says bags or bank.
        struct Row
        {
            ObjectGuid::LowType bag;
            uint8 slot;
            ObjectGuid::LowType item;
            ItemTemplate const* proto;
            std::string link;
        };
        std::vector<Row> rows;
        std::unordered_map<ObjectGuid::LowType, uint8> bagSlots;
        do
        {
            Field* fields = result->Fetch();
            Row row{ fields[0].Get<uint32>(), fields[1].Get<uint8>(), fields[2].Get<uint32>(),
                sObjectMgr->GetItemTemplate(fields[3].Get<uint32>()), {} };
            if (!row.bag)
                bagSlots[row.item] = row.slot;
            if (row.proto)
                row.link = LinkOf(row.proto->ItemId, fields[4].Get<std::string_view>(), fields[5].Get<int32>());
            rows.push_back(std::move(row));
        } while (result->NextRow());

        for (Row const& row : rows)
        {
            if (!row.proto)
                continue;

            OfflineItem item{ row.item, 'B', row.slot, row.proto, row.link };
            if (!row.bag)
            {
                if (row.slot < EQUIPMENT_SLOT_END)
                    item.loc = 'E';
                else if (row.slot >= INVENTORY_SLOT_ITEM_START && row.slot < INVENTORY_SLOT_ITEM_END)
                    item.loc = 'B';
                else if (row.slot >= BANK_SLOT_ITEM_START && row.slot < BANK_SLOT_ITEM_END)
                    item.loc = 'K';
                else
                    continue; // equipped bags, bank bags, keyring, buyback
            }
            else
            {
                auto bag = bagSlots.find(row.bag);
                if (bag == bagSlots.end())
                    continue;
                if (bag->second >= INVENTORY_SLOT_BAG_START && bag->second < INVENTORY_SLOT_BAG_END)
                    item.loc = 'B';
                else if (bag->second >= BANK_SLOT_BAG_START && bag->second < BANK_SLOT_BAG_END)
                    item.loc = 'K';
                else
                    continue;
            }
            items.push_back(std::move(item));
        }
        return items;
    }

    bool Mgr::FitsSlot(ItemTemplate const* proto, uint8 slot)
    {
        switch (proto->InventoryType)
        {
            case INVTYPE_HEAD: return slot == EQUIPMENT_SLOT_HEAD;
            case INVTYPE_NECK: return slot == EQUIPMENT_SLOT_NECK;
            case INVTYPE_SHOULDERS: return slot == EQUIPMENT_SLOT_SHOULDERS;
            case INVTYPE_BODY: return slot == EQUIPMENT_SLOT_BODY;
            case INVTYPE_CHEST:
            case INVTYPE_ROBE: return slot == EQUIPMENT_SLOT_CHEST;
            case INVTYPE_WAIST: return slot == EQUIPMENT_SLOT_WAIST;
            case INVTYPE_LEGS: return slot == EQUIPMENT_SLOT_LEGS;
            case INVTYPE_FEET: return slot == EQUIPMENT_SLOT_FEET;
            case INVTYPE_WRISTS: return slot == EQUIPMENT_SLOT_WRISTS;
            case INVTYPE_HANDS: return slot == EQUIPMENT_SLOT_HANDS;
            case INVTYPE_FINGER: return slot == EQUIPMENT_SLOT_FINGER1 || slot == EQUIPMENT_SLOT_FINGER2;
            case INVTYPE_TRINKET: return slot == EQUIPMENT_SLOT_TRINKET1 || slot == EQUIPMENT_SLOT_TRINKET2;
            case INVTYPE_CLOAK: return slot == EQUIPMENT_SLOT_BACK;
            case INVTYPE_WEAPON:
            case INVTYPE_2HWEAPON: return slot == EQUIPMENT_SLOT_MAINHAND || slot == EQUIPMENT_SLOT_OFFHAND;
            case INVTYPE_WEAPONMAINHAND: return slot == EQUIPMENT_SLOT_MAINHAND;
            case INVTYPE_SHIELD:
            case INVTYPE_WEAPONOFFHAND:
            case INVTYPE_HOLDABLE: return slot == EQUIPMENT_SLOT_OFFHAND;
            case INVTYPE_RANGED:
            case INVTYPE_RANGEDRIGHT:
            case INVTYPE_THROWN:
            case INVTYPE_RELIC: return slot == EQUIPMENT_SLOT_RANGED;
            case INVTYPE_TABARD: return slot == EQUIPMENT_SLOT_TABARD;
            default: return false;
        }
    }

    uint32 Mgr::GuessWear(Subject const& subject, ItemTemplate const* proto)
    {
        if (!subject.cls || !subject.race)
            return EQUIP_ERR_OK;

        uint32 const classMask = 1 << (subject.cls - 1);
        uint32 const raceMask = 1 << (subject.race - 1);
        if (!(proto->AllowableClass & classMask) || !(proto->AllowableRace & raceMask))
            return EQUIP_ERR_YOU_CAN_NEVER_USE_THAT_ITEM;
        TeamId const team = Player::TeamIdForRace(subject.race);
        if ((proto->HasFlag2(ITEM_FLAG2_FACTION_HORDE) && team != TEAM_HORDE)
            || (proto->HasFlag2(ITEM_FLAG2_FACTION_ALLIANCE) && team != TEAM_ALLIANCE))
            return EQUIP_ERR_YOU_CAN_NEVER_USE_THAT_ITEM;

        // What each class can learn (WotLK trainers): mail at 40 for hunters and shamans, plate at 40 for warriors
        // and paladins. Weapon skills by subclass bit.
        auto bit = [](uint32 subclass) { return 1u << subclass; };
        uint8 const cls = subject.cls;
        bool const level40 = subject.level >= 40;
        if (proto->Class == ITEM_CLASS_ARMOR)
        {
            switch (proto->SubClass)
            {
                case ITEM_SUBCLASS_ARMOR_LEATHER:
                    if (cls == CLASS_PRIEST || cls == CLASS_MAGE || cls == CLASS_WARLOCK)
                        return EQUIP_ERR_NO_REQUIRED_PROFICIENCY;
                    break;
                case ITEM_SUBCLASS_ARMOR_MAIL:
                    if (!(cls == CLASS_WARRIOR || cls == CLASS_PALADIN || cls == CLASS_DEATH_KNIGHT
                        || ((cls == CLASS_HUNTER || cls == CLASS_SHAMAN) && level40)))
                        return EQUIP_ERR_NO_REQUIRED_PROFICIENCY;
                    break;
                case ITEM_SUBCLASS_ARMOR_PLATE:
                    if (!(cls == CLASS_DEATH_KNIGHT || ((cls == CLASS_WARRIOR || cls == CLASS_PALADIN) && level40)))
                        return EQUIP_ERR_NO_REQUIRED_PROFICIENCY;
                    break;
                case ITEM_SUBCLASS_ARMOR_SHIELD:
                    if (!(cls == CLASS_WARRIOR || cls == CLASS_PALADIN || cls == CLASS_SHAMAN))
                        return EQUIP_ERR_NO_REQUIRED_PROFICIENCY;
                    break;
                case ITEM_SUBCLASS_ARMOR_LIBRAM:
                    if (cls != CLASS_PALADIN)
                        return EQUIP_ERR_YOU_CAN_NEVER_USE_THAT_ITEM;
                    break;
                case ITEM_SUBCLASS_ARMOR_IDOL:
                    if (cls != CLASS_DRUID)
                        return EQUIP_ERR_YOU_CAN_NEVER_USE_THAT_ITEM;
                    break;
                case ITEM_SUBCLASS_ARMOR_TOTEM:
                    if (cls != CLASS_SHAMAN)
                        return EQUIP_ERR_YOU_CAN_NEVER_USE_THAT_ITEM;
                    break;
                case ITEM_SUBCLASS_ARMOR_SIGIL:
                    if (cls != CLASS_DEATH_KNIGHT)
                        return EQUIP_ERR_YOU_CAN_NEVER_USE_THAT_ITEM;
                    break;
                default:
                    break;
            }
        }
        else if (proto->Class == ITEM_CLASS_WEAPON)
        {
            uint32 const meleePlate = bit(ITEM_SUBCLASS_WEAPON_AXE) | bit(ITEM_SUBCLASS_WEAPON_AXE2)
                | bit(ITEM_SUBCLASS_WEAPON_MACE) | bit(ITEM_SUBCLASS_WEAPON_MACE2) | bit(ITEM_SUBCLASS_WEAPON_POLEARM)
                | bit(ITEM_SUBCLASS_WEAPON_SWORD) | bit(ITEM_SUBCLASS_WEAPON_SWORD2);
            uint32 const ranged = bit(ITEM_SUBCLASS_WEAPON_BOW) | bit(ITEM_SUBCLASS_WEAPON_GUN)
                | bit(ITEM_SUBCLASS_WEAPON_CROSSBOW) | bit(ITEM_SUBCLASS_WEAPON_THROWN);
            uint32 const casters = bit(ITEM_SUBCLASS_WEAPON_STAFF) | bit(ITEM_SUBCLASS_WEAPON_DAGGER)
                | bit(ITEM_SUBCLASS_WEAPON_WAND);
            uint32 allowed = 0;
            switch (cls)
            {
                case CLASS_WARRIOR:
                    allowed = meleePlate | ranged | bit(ITEM_SUBCLASS_WEAPON_STAFF) | bit(ITEM_SUBCLASS_WEAPON_FIST)
                        | bit(ITEM_SUBCLASS_WEAPON_DAGGER);
                    break;
                case CLASS_PALADIN:
                case CLASS_DEATH_KNIGHT:
                    allowed = meleePlate;
                    break;
                case CLASS_HUNTER:
                    allowed = bit(ITEM_SUBCLASS_WEAPON_AXE) | bit(ITEM_SUBCLASS_WEAPON_AXE2)
                        | bit(ITEM_SUBCLASS_WEAPON_POLEARM) | bit(ITEM_SUBCLASS_WEAPON_SWORD)
                        | bit(ITEM_SUBCLASS_WEAPON_SWORD2) | bit(ITEM_SUBCLASS_WEAPON_STAFF)
                        | bit(ITEM_SUBCLASS_WEAPON_FIST) | bit(ITEM_SUBCLASS_WEAPON_DAGGER) | ranged;
                    break;
                case CLASS_ROGUE:
                    allowed = bit(ITEM_SUBCLASS_WEAPON_AXE) | bit(ITEM_SUBCLASS_WEAPON_MACE)
                        | bit(ITEM_SUBCLASS_WEAPON_SWORD) | bit(ITEM_SUBCLASS_WEAPON_FIST)
                        | bit(ITEM_SUBCLASS_WEAPON_DAGGER) | ranged;
                    break;
                case CLASS_PRIEST:
                    allowed = casters | bit(ITEM_SUBCLASS_WEAPON_MACE);
                    break;
                case CLASS_SHAMAN:
                    allowed = bit(ITEM_SUBCLASS_WEAPON_AXE) | bit(ITEM_SUBCLASS_WEAPON_AXE2)
                        | bit(ITEM_SUBCLASS_WEAPON_MACE) | bit(ITEM_SUBCLASS_WEAPON_MACE2)
                        | bit(ITEM_SUBCLASS_WEAPON_STAFF) | bit(ITEM_SUBCLASS_WEAPON_FIST)
                        | bit(ITEM_SUBCLASS_WEAPON_DAGGER);
                    break;
                case CLASS_MAGE:
                case CLASS_WARLOCK:
                    allowed = casters | bit(ITEM_SUBCLASS_WEAPON_SWORD);
                    break;
                case CLASS_DRUID:
                    allowed = bit(ITEM_SUBCLASS_WEAPON_MACE) | bit(ITEM_SUBCLASS_WEAPON_MACE2)
                        | bit(ITEM_SUBCLASS_WEAPON_POLEARM) | bit(ITEM_SUBCLASS_WEAPON_STAFF)
                        | bit(ITEM_SUBCLASS_WEAPON_FIST) | bit(ITEM_SUBCLASS_WEAPON_DAGGER);
                    break;
                default:
                    allowed = ~0u;
                    break;
            }
            // Fishing poles, miscellaneous and exotic weapons: anyone.
            allowed |= bit(ITEM_SUBCLASS_WEAPON_MISC) | bit(ITEM_SUBCLASS_WEAPON_FISHING_POLE)
                | bit(ITEM_SUBCLASS_WEAPON_EXOTIC) | bit(ITEM_SUBCLASS_WEAPON_EXOTIC2)
                | bit(ITEM_SUBCLASS_WEAPON_SPEAR);
            if (proto->SubClass < 32 && !(allowed & bit(proto->SubClass)))
                return EQUIP_ERR_NO_REQUIRED_PROFICIENCY;
        }

        if (subject.level < proto->RequiredLevel)
            return EQUIP_ERR_CANT_EQUIP_LEVEL_I;
        return EQUIP_ERR_OK;
    }

    bool Mgr::AssignFromViewer(Player* viewer, Subject const& target, uint32 setId, uint8 slot, int32 bag,
        int32 bagSlot, std::string& error)
    {
        if (!SamePool(viewer, target))
        {
            error = Acore::StringFormat("{} doesn't share your wardrobe.", target.name);
            return false;
        }

        if (!target.profile || !FindSet(*target.profile, setId, error))
            return false;

        uint8 serverBag = 0;
        uint8 serverSlot = 0;
        Item* item = ToServerPos(bag, bagSlot, serverBag, serverSlot)
            ? viewer->GetItemByPos(serverBag, serverSlot) : nullptr;
        if (!item)
        {
            error = "There is no item there.";
            return false;
        }
        bool const fits = slot < EQUIPMENT_SLOT_END && (target.player
            ? target.player->FindEquipSlot(item->GetTemplate(), slot, true) == slot
            : FitsSlot(item->GetTemplate(), slot));
        if (!fits)
        {
            error = "That item doesn't go in that slot.";
            return false;
        }

        // Into the wardrobe first: AssignSlot only takes items the target can reach.
        ObjectGuid::LowType const guid = item->GetGUID().GetCounter();
        if (!DepositItem(viewer, item, error))
            return false;
        return AssignSlot(target, setId, slot, MODE_ITEM, guid, error);
    }

    std::vector<std::string> Mgr::UsedBy(ObjectGuid::LowType self, uint32 account,
        std::set<ObjectGuid::LowType> const& items) const
    {
        std::vector<std::string> out;
        if (items.empty())
            return out;

        uint32 const group = GroupOf(account);
        auto shares = [this, self, group](ObjectGuid::LowType owner, uint32 ownerAccount)
        {
            return Cfg().sharedPool ? ownerAccount && GroupOf(ownerAccount) == group : owner == self;
        };

        // Sets in memory (online characters, and offline ones shown or logged out since startup) are the truth:
        // their rows may still be on the way.
        std::set<ObjectGuid::LowType> inMemory;
        auto add = [&](ObjectGuid::LowType owner, Profile const& profile)
        {
            if (!shares(owner, profile.account))
                return;
            inMemory.insert(owner);

            std::string name;
            if (!sCharacterCache->GetCharacterNameByGuid(ObjectGuid::Create<HighGuid::Player>(owner), name))
                name = "?";
            for (auto const& [id, set] : profile.sets)
                for (auto const& [slot, entry] : set.slots)
                    if (entry.mode == MODE_ITEM && items.count(entry.item))
                        out.push_back(Acore::StringFormat("{},{},{},{}", entry.item, slot, name, set.name));
        };
        for (auto const& [owner, profile] : _profiles)
            add(owner, profile);
        for (auto const& [owner, profile] : _offline)
            add(owner, profile);

        // The others, from the database (without a shared wardrobe only the character's own sets count).
        if (!Cfg().sharedPool)
            return out;

        if (QueryResult result = CharacterDatabase.Query("SELECT s.item_guid, s.slot, c.guid, c.name, ws.name "
            "FROM gear_wardrobe_slot s "
            "INNER JOIN gear_wardrobe_set ws ON ws.owner_guid = s.owner_guid AND ws.set_id = s.set_id "
            "INNER JOIN characters c ON c.guid = s.owner_guid "
            "WHERE s.mode = {} AND c.account IN ({})", uint32(MODE_ITEM), JoinIds(GroupAccounts(account))))
        {
            do
            {
                Field* fields = result->Fetch();
                ObjectGuid::LowType const item = fields[0].Get<uint32>();
                if (!items.count(item) || inMemory.count(fields[2].Get<uint32>()))
                    continue;
                out.push_back(Acore::StringFormat("{},{},{},{}", item, fields[1].Get<uint8>(),
                    fields[3].Get<std::string>(), fields[4].Get<std::string>()));
            } while (result->NextRow());
        }
        return out;
    }

    std::vector<std::string> Mgr::Characters(Player* viewer) const
    {
        auto entry = [](Player* player)
        {
            return Acore::StringFormat("{},{},{},{},0", player->GetGUID().GetCounter(), player->GetName(),
                player->getClass(), player->GetLevel());
        };

        // Only wardrobe users have a profile, so this never walks the random bots.
        std::vector<Player*> bots;
        for (auto const& [owner, profile] : _profiles)
        {
            if (owner == viewer->GetGUID().GetCounter())
                continue;
            Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(owner));
            if (bot && bot->GetSession()->IsHeadless() && MayCommand(viewer, bot))
                bots.push_back(bot);
        }
        std::sort(bots.begin(), bots.end(), [](Player* a, Player* b) { return a->GetName() < b->GetName(); });

        std::vector<std::string> out = { entry(viewer) };
        for (Player* bot : bots)
            out.push_back(entry(bot));

        // Offline characters sharing the wardrobe: set editing only.
        std::vector<uint32> const accounts = OfflineAccounts(viewer);
        if (accounts.empty())
            return out;
        if (QueryResult result = CharacterDatabase.Query("SELECT guid, name, class, level FROM characters "
            "WHERE account IN ({}) AND guid <> {} ORDER BY name", JoinIds(accounts), viewer->GetGUID().GetCounter()))
        {
            do
            {
                Field* fields = result->Fetch();
                ObjectGuid::LowType const guid = fields[0].Get<uint32>();
                if (ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(guid)))
                    continue; // online: listed above if it is an altbot of the viewer's
                out.push_back(Acore::StringFormat("{},{},{},{},1", guid, fields[1].Get<std::string>(),
                    fields[2].Get<uint8>(), fields[3].Get<uint8>()));
            } while (result->NextRow());
        }
        return out;
    }

    bool Mgr::Sell(Player* player, std::vector<ObjectGuid::LowType> const& guids, std::string& error)
    {
        if (!Find(player))
            return false;

        uint64 total = 0;
        uint32 unsellable = 0;
        std::vector<ObjectGuid::LowType> sold;
        for (ObjectGuid::LowType guid : guids)
        {
            StoredItem const* stored = FindStored(player, guid);
            if (!stored || std::find(sold.begin(), sold.end(), guid) != sold.end())
                continue;
            ItemTemplate const* proto = stored->item->GetTemplate();
            if (!proto->SellPrice)
            {
                ++unsellable;
                continue;
            }
            total += uint64(proto->SellPrice) * stored->item->GetCount();
            sold.push_back(guid);
        }

        if (sold.empty())
        {
            error = unsellable ? "Vendors don't buy that." : "That item isn't in the wardrobe.";
            return false;
        }
        if (uint64(player->GetMoney()) + total > MAX_MONEY_AMOUNT)
        {
            error = "You can't carry that much gold.";
            return false;
        }

        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        for (ObjectGuid::LowType guid : sold)
        {
            _stored.at(guid).item->DeleteFromDB(trans);
            trans->Append("DELETE FROM gear_wardrobe_item WHERE item_guid = {}", guid);
            // Sets that used it leave that slot alone from now on (also those of offline characters).
            trans->Append("DELETE FROM gear_wardrobe_slot WHERE item_guid = {} AND mode = {}", guid, uint32(MODE_ITEM));
            DropStored(guid);
        }
        std::set<ObjectGuid::LowType> const gone(sold.begin(), sold.end());
        for (auto* profiles : { &_profiles, &_offline })
            for (auto& [owner, profile] : *profiles)
                for (auto& [id, set] : profile.sets)
                    for (auto itr = set.slots.begin(); itr != set.slots.end();)
                    {
                        if (itr->second.mode == MODE_ITEM && gone.count(itr->second.item))
                            itr = set.slots.erase(itr);
                        else
                            ++itr;
                    }

        player->ModifyMoney(int32(total));
        player->SaveGoldToDB(trans);
        CharacterDatabase.CommitTransaction(trans);

        LOG_INFO("module", "mod-gear-wardrobe: {} sold {} stored items for {} copper.", player->GetName(), sold.size(),
            total);
        uint32 const gold = uint32(total / 10000);
        uint32 const silver = uint32(total % 10000 / 100);
        uint32 const copper = uint32(total % 100);
        std::string money = gold ? Acore::StringFormat("{}g {}s {}c", gold, silver, copper)
            : silver ? Acore::StringFormat("{}s {}c", silver, copper) : Acore::StringFormat("{}c", copper);
        Comms::SendInfo(player, Acore::StringFormat("Sold {} item(s) for {}.{}", sold.size(), money,
            unsellable ? Acore::StringFormat(" {} had no vendor price and stayed.", unsellable) : ""));
        return true;
    }

    void Mgr::SendSync(Player* viewer, Player* target)
    {
        SendSync(viewer, Online(target));
    }

    void Mgr::SendSync(Player* viewer, Subject const& subject)
    {
        Profile* profile = subject.profile;
        if (!profile)
            return;

        Player* const target = subject.player; // null: offline
        bool const self = subject.guid == viewer->GetGUID().GetCounter();

        // Shared wardrobe: taken-out items another character has (checked first, so the slots below agree).
        std::vector<ObjectGuid::LowType> const held = ResolveTaken(subject.guid, subject.account);
        auto isHeld = [&held](ObjectGuid::LowType guid) { return std::binary_search(held.begin(), held.end(), guid); };

        // An offline character's gear, from the database.
        std::vector<OfflineItem> offlineItems;
        std::unordered_map<ObjectGuid::LowType, OfflineItem const*> offlineByGuid;
        if (!target)
        {
            offlineItems = LoadOfflineInventory(subject.guid);
            for (OfflineItem const& item : offlineItems)
                offlineByGuid[item.guid] = &item;
        }

        // "guid,name,self,samePool,offline": whose wardrobe this is. Older addons ignore the payload.
        Comms::Send(viewer, "BEGIN", Acore::StringFormat("{},{},{},{},{}", subject.guid, subject.name, self ? 1 : 0,
            SamePool(viewer, subject) ? 1 : 0, target ? 0 : 1));

        std::vector<std::string> sets;
        std::vector<std::string> slots;
        for (auto& [id, set] : profile->sets)
        {
            sets.push_back(Acore::StringFormat("{},{},{}", id, set.icon, set.name));
            std::set<ObjectGuid::LowType> claimed;
            bool repointed = false;
            for (auto& [slot, entry] : set.slots)
            {
                // Where the item is now: E worn, B bags, K bank, W wardrobe, O another character has it, M missing.
                char loc = 'M';
                std::string link;
                if (entry.mode == MODE_ITEM)
                {
                    Item* item = target ? target->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(entry.item))
                        : nullptr;
                    auto offline = offlineByGuid.find(entry.item);
                    if (item)
                    {
                        loc = item->IsEquipped() ? 'E' : Player::IsBankPos(item->GetPos()) ? 'K' : 'B';
                        link = LinkOf(item);
                    }
                    else if (offline != offlineByGuid.end())
                    {
                        loc = offline->second->loc;
                        link = offline->second->link;
                    }
                    else if (StoredItem const* stored = FindStored(subject.guid, subject.account, entry.item))
                    {
                        loc = 'W';
                        link = stored->link;
                    }
                    else if (ObjectGuid::LowType const spare = target ? FindSpare(target, set, entry.item, claimed) : 0)
                    {
                        // Out of reach, but there is an identical copy: the set uses that one from now on.
                        claimed.insert(spare);
                        entry.item = spare;
                        repointed = true;
                        if (Item* copy = target->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(spare)))
                        {
                            loc = copy->IsEquipped() ? 'E' : 'B';
                            link = LinkOf(copy);
                        }
                        else
                        {
                            loc = 'W';
                            link = _stored.at(spare).link;
                        }
                    }
                    else if (isHeld(entry.item))
                    {
                        loc = 'O';
                        link = _taken.at(entry.item).link;
                    }
                }
                else
                    loc = '-';
                slots.push_back(Acore::StringFormat("{},{},{},{},{},{}", id, slot, entry.mode, entry.item, loc, link));
            }
            if (repointed)
                SaveSet(subject.guid, set);
        }

        // Oldest item guid first, as the addon got them before the wardrobe was kept in memory.
        std::vector<ObjectGuid::LowType> visible;
        for (auto const& [guid, stored] : _stored)
            if (CanSee(subject.guid, subject.account, stored))
                visible.push_back(guid);
        std::sort(visible.begin(), visible.end());

        // "guid,reason": stored items the character can't wear (reason as in CanWear; a guess when offline).
        auto reasonFor = [this, &subject, target](Item* item)
        {
            return target ? CanWear(target, item) : GuessWear(subject, item->GetTemplate());
        };
        std::vector<std::string> items;
        std::vector<std::string> noUse;
        for (ObjectGuid::LowType guid : visible)
        {
            StoredItem const& stored = _stored.at(guid);
            items.push_back(Acore::StringFormat("{},{}", guid, stored.link));
            if (uint32 const reason = reasonFor(stored.item))
                noUse.push_back(Acore::StringFormat("{},{}", guid, reason));
        }

        // "guid,holder,link"; older addons ignore the command.
        std::vector<std::string> out;
        for (ObjectGuid::LowType guid : held)
        {
            TakenItem const& taken = _taken.at(guid);
            std::string name;
            if (!sCharacterCache->GetCharacterNameByGuid(ObjectGuid::Create<HighGuid::Player>(taken.holder), name))
                name = "?";
            out.push_back(Acore::StringFormat("{},{},{}", guid, name, taken.link));
        }

        std::set<ObjectGuid::LowType> listed(visible.begin(), visible.end());
        listed.insert(held.begin(), held.end());
        std::vector<std::string> const usedBy = UsedBy(subject.guid, subject.account, listed);

        // Another character's worn and carried gear, "guid,loc,reason,link": the viewer's client only sees its own.
        std::vector<std::string> carried;
        if (target && !self)
        {
            for (ReachableItem const& reachable : Reachable(target))
            {
                ItemTemplate const* proto = reachable.item->GetTemplate();
                if (reachable.stored || (proto->Class != ITEM_CLASS_ARMOR && proto->Class != ITEM_CLASS_WEAPON))
                    continue;
                bool const worn = reachable.item->IsEquipped();
                carried.push_back(Acore::StringFormat("{},{},{},{}", reachable.item->GetGUID().GetCounter(),
                    worn ? 'E' : 'B', worn ? 0 : CanWear(target, reachable.item), LinkOf(reachable.item)));
            }
        }
        else if (!target)
        {
            for (OfflineItem const& item : offlineItems)
            {
                bool const gear = item.proto->Class == ITEM_CLASS_ARMOR || item.proto->Class == ITEM_CLASS_WEAPON;
                if (item.loc == 'K' || !gear)
                    continue;
                carried.push_back(Acore::StringFormat("{},{},{},{}", item.guid, item.loc,
                    item.loc == 'E' ? 0 : GuessWear(subject, item.proto), item.link));
            }
        }

        Comms::SendList(viewer, "SETS", sets);
        Comms::SendList(viewer, "SLOTS", slots);
        Comms::SendList(viewer, "ITEMS", items);
        Comms::SendList(viewer, "OUT", out);
        Comms::SendList(viewer, "NOUSE", noUse);
        Comms::SendList(viewer, "USEDBY", usedBy);
        Comms::SendList(viewer, "CARRIED", carried);
        Comms::SendList(viewer, "CHARS", Characters(viewer));
        Comms::Send(viewer, "END", Acore::StringFormat("{},{}", Cfg().maxSets, Cfg().maxItems));
    }

    void Mgr::HandleAddonMessage(Player* player, std::string const& body)
    {
        if (!Cfg().enable || !Find(player))
            return;

        std::string_view view(body);

        // "@<guid>|CMD:..." runs CMD for that character (an altbot of the player's, or an offline character sharing
        // its wardrobe); replies come back here.
        Subject target = Online(player);
        if (!view.empty() && view.front() == '@')
        {
            std::size_t const bar = view.find('|');
            if (bar == std::string_view::npos)
                return;
            ObjectGuid::LowType const guid = Acore::StringTo<ObjectGuid::LowType>(view.substr(1, bar - 1)).value_or(0);
            view = view.substr(bar + 1);

            std::string error;
            if (!ResolveTarget(player, guid, target, error))
            {
                Comms::SendError(player, error);
                SendSync(player, player);
                return;
            }
        }
        bool const self = target.player == player;
        // Equipping and saving worn gear need the character in the world.
        auto online = [&target](std::string& error)
        {
            if (target.player)
                return true;
            error = Acore::StringFormat("{} is offline. Log them in to equip or save worn gear.", target.name);
            return false;
        };

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

        // Set commands act on `target`; DEP, WD and SELL always use the player's own bags and gold.
        std::string error;
        bool changed = false;
        if (command == "SYNC")
        {
            SendSync(player, target);
            return;
        }
        else if (command == "NEW")
            changed = CreateSet(target, std::string(payload), error);
        else if (command == "REN")
            changed = RenameSet(target, uint32(num(0)), tail(), error);
        else if (command == "DEL")
            changed = DeleteSet(target, uint32(num(0)), error);
        else if (command == "ICON")
            changed = SetIcon(target, uint32(num(0)), tail(), error);
        else if (command == "SAVE")
            changed = online(error) && SaveWorn(target.player, uint32(num(0)), error);
        else if (command == "SLOT")
            changed = AssignSlot(target, uint32(num(0)), uint8(num(1)), uint8(num(2)), guidArg(3), error);
        else if (command == "SLOTPOS")
        {
            // The position is in the player's own bags; for another character the item is stored and assigned in one
            // go.
            if (self)
                changed = AssignSlotFromPos(player, uint32(num(0)), uint8(num(1)), num(2), num(3), error);
            else
                changed = AssignFromViewer(player, target, uint32(num(0)), uint8(num(1)), num(2), num(3), error);
        }
        else if (command == "DEP")
            changed = Deposit(player, num(0), num(1), error);
        else if (command == "WD")
            changed = Withdraw(player, guidArg(0), error);
        else if (command == "SELL")
        {
            std::vector<ObjectGuid::LowType> guids;
            for (std::size_t i = 0; i < args.size(); ++i)
                if (ObjectGuid::LowType const guid = guidArg(i))
                    guids.push_back(guid);
            changed = Sell(player, guids, error);
        }
        else if (command == "EQUIP")
        {
            std::vector<EquipFailure> failures;
            if ((changed = online(error) && EquipSet(target.player, uint32(num(0)), failures, error)))
                SendEquipResult(player, target.player, uint32(num(0)), failures);
        }
        else
            return;

        if (!error.empty())
            Comms::SendError(player, error);
        if (changed)
            SendSync(player, target);
    }
}
