#include "GearWardrobeBotChat.h"
#include "Chat.h"
#include "GearWardrobeComms.h"
#include "GearWardrobeMgr.h"
#include "Item.h"
#include "Player.h"
#include "StringConvert.h"
#include "StringFormat.h"
#include "Util.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <unordered_map>
#include <vector>

namespace GearWardrobe::BotChat
{
    namespace
    {
        constexpr std::string_view USAGE = "Wardrobe: gw list, gw equip <set>, gw save <set>, gw items [slot], "
            "gw wear <item> [slot], gw store <item or slot>. <item> is an item link or name.";

        // Whispers longer than this are cut off by the client.
        constexpr std::size_t MAX_LINE = 250;
        // gw items stops after this many lines.
        constexpr std::size_t MAX_ITEM_LINES = 12;

        // Built the way playerbots' TellMaster does it, so the reply skips the chat hooks.
        void Tell(Player* bot, Player* to, std::string_view text)
        {
            WorldPacket data;
            ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_UNIVERSAL, bot, nullptr, text);
            to->SendDirectMessage(&data);
        }

        // A swallowed whisper never gets the "To <bot>:" echo that Player::Whisper sends; send it here.
        void Echo(Player* sender, Player* bot, std::string_view text)
        {
            WorldPacket data;
            ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER_INFORM, LANG_UNIVERSAL, bot, bot, text);
            sender->SendDirectMessage(&data);
        }

        std::string Reason(EquipFailure const& failure)
        {
            if (failure.missing)
                return failure.holder.empty() ? "item is gone" : failure.holder + " has it";

            switch (failure.result)
            {
                case EQUIP_ERR_CANT_EQUIP_LEVEL_I:
                    return "level too low";
                case EQUIP_ERR_CANT_EQUIP_SKILL:
                case EQUIP_ERR_NO_REQUIRED_PROFICIENCY:
                    return "can't use that item type";
                case EQUIP_ERR_ITEM_UNIQUE_EQUIPABLE:
                case EQUIP_ERR_ITEM_UNIQUE_EQUIPPABLE_SOCKETED:
                    return "unique-equipped";
                case EQUIP_ERR_YOU_CAN_NEVER_USE_THAT_ITEM:
                case EQUIP_ERR_YOU_CAN_NEVER_USE_THAT_ITEM2:
                    return "my class or race can't use it";
                case EQUIP_ERR_CANT_EQUIP_REPUTATION:
                    return "my reputation is too low";
                case EQUIP_ERR_CANT_DUAL_WIELD:
                    return "I can't dual wield";
                case EQUIP_ERR_CANT_EQUIP_WITH_TWOHANDED:
                    return "my two-hander is in the way";
                case EQUIP_ERR_CANT_CARRY_MORE_OF_THIS:
                    return "I already have as many of it as I can";
                case EQUIP_ERR_DONT_OWN_THAT_ITEM:
                    return "it's soulbound to someone else";
                case EQUIP_ERR_NOT_WHILE_DISARMED:
                    return "I'm disarmed";
                case EQUIP_ERR_YOU_ARE_STUNNED:
                    return "I'm stunned";
                case EQUIP_ERR_NOT_IN_COMBAT:
                    return "I'm in combat";
                case EQUIP_ERR_ITEM_CANT_BE_EQUIPPED:
                case EQUIP_ERR_ITEM_DOESNT_GO_TO_SLOT:
                    return "it doesn't fit there";
                case EQUIP_ERR_CANT_DO_RIGHT_NOW:
                    // What another module's check (mod-individual-progression's expansion lock) answers.
                    return "not allowed (an expansion I haven't reached?)";
                default:
                    return Acore::StringFormat("can't equip it (error {})", failure.result);
            }
        }

        // The master's addon may be showing this bot or the shared wardrobe: it syncs again whatever it shows.
        void Refresh(Player* sender)
        {
            if (!sender->GetSession()->IsHeadless())
                Comms::Send(sender, "CHANGED");
        }

        std::string Lower(std::string_view text)
        {
            std::string out(text);
            std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return std::tolower(c); });
            return out;
        }

        // "finger 2", "Main-Hand", "ring" -> equipment slots; empty for an unknown name.
        std::vector<uint8> ParseSlots(std::string_view text)
        {
            static std::unordered_map<std::string, std::vector<uint8>> const names =
            {
                { "head", { EQUIPMENT_SLOT_HEAD } }, { "helm", { EQUIPMENT_SLOT_HEAD } },
                { "neck", { EQUIPMENT_SLOT_NECK } }, { "amulet", { EQUIPMENT_SLOT_NECK } },
                { "shoulder", { EQUIPMENT_SLOT_SHOULDERS } }, { "shoulders", { EQUIPMENT_SLOT_SHOULDERS } },
                { "shirt", { EQUIPMENT_SLOT_BODY } },
                { "chest", { EQUIPMENT_SLOT_CHEST } }, { "robe", { EQUIPMENT_SLOT_CHEST } },
                { "waist", { EQUIPMENT_SLOT_WAIST } }, { "belt", { EQUIPMENT_SLOT_WAIST } },
                { "legs", { EQUIPMENT_SLOT_LEGS } }, { "pants", { EQUIPMENT_SLOT_LEGS } },
                { "feet", { EQUIPMENT_SLOT_FEET } }, { "boots", { EQUIPMENT_SLOT_FEET } },
                { "wrist", { EQUIPMENT_SLOT_WRISTS } }, { "wrists", { EQUIPMENT_SLOT_WRISTS } },
                { "bracers", { EQUIPMENT_SLOT_WRISTS } },
                { "hands", { EQUIPMENT_SLOT_HANDS } }, { "gloves", { EQUIPMENT_SLOT_HANDS } },
                { "finger", { EQUIPMENT_SLOT_FINGER1, EQUIPMENT_SLOT_FINGER2 } },
                { "ring", { EQUIPMENT_SLOT_FINGER1, EQUIPMENT_SLOT_FINGER2 } },
                { "rings", { EQUIPMENT_SLOT_FINGER1, EQUIPMENT_SLOT_FINGER2 } },
                { "finger1", { EQUIPMENT_SLOT_FINGER1 } }, { "ring1", { EQUIPMENT_SLOT_FINGER1 } },
                { "finger2", { EQUIPMENT_SLOT_FINGER2 } }, { "ring2", { EQUIPMENT_SLOT_FINGER2 } },
                { "trinket", { EQUIPMENT_SLOT_TRINKET1, EQUIPMENT_SLOT_TRINKET2 } },
                { "trinkets", { EQUIPMENT_SLOT_TRINKET1, EQUIPMENT_SLOT_TRINKET2 } },
                { "trinket1", { EQUIPMENT_SLOT_TRINKET1 } }, { "trinket2", { EQUIPMENT_SLOT_TRINKET2 } },
                { "back", { EQUIPMENT_SLOT_BACK } }, { "cloak", { EQUIPMENT_SLOT_BACK } },
                { "cape", { EQUIPMENT_SLOT_BACK } },
                { "mainhand", { EQUIPMENT_SLOT_MAINHAND } }, { "mh", { EQUIPMENT_SLOT_MAINHAND } },
                { "offhand", { EQUIPMENT_SLOT_OFFHAND } }, { "oh", { EQUIPMENT_SLOT_OFFHAND } },
                { "shield", { EQUIPMENT_SLOT_OFFHAND } },
                { "weapon", { EQUIPMENT_SLOT_MAINHAND, EQUIPMENT_SLOT_OFFHAND } },
                { "weapons", { EQUIPMENT_SLOT_MAINHAND, EQUIPMENT_SLOT_OFFHAND } },
                { "ranged", { EQUIPMENT_SLOT_RANGED } }, { "relic", { EQUIPMENT_SLOT_RANGED } },
                { "wand", { EQUIPMENT_SLOT_RANGED } },
                { "tabard", { EQUIPMENT_SLOT_TABARD } },
            };

            std::string key;
            for (char c : text)
                if (std::isalnum(static_cast<unsigned char>(c)))
                    key.push_back(char(std::tolower(static_cast<unsigned char>(c))));

            auto itr = names.find(key);
            return itr != names.end() ? itr->second : std::vector<uint8>();
        }

        // "Finger" for both rings, "Trinket" for both trinkets: how gw items groups its list.
        std::string GroupName(uint8 slot)
        {
            switch (slot)
            {
                case EQUIPMENT_SLOT_FINGER1:
                case EQUIPMENT_SLOT_FINGER2:
                    return "Finger";
                case EQUIPMENT_SLOT_TRINKET1:
                case EQUIPMENT_SLOT_TRINKET2:
                    return "Trinket";
                default:
                    return SlotName(slot);
            }
        }

        // Both rings sort as the first finger, both trinkets as the first trinket.
        uint8 GroupKey(uint8 slot)
        {
            if (slot == EQUIPMENT_SLOT_FINGER2)
                return EQUIPMENT_SLOT_FINGER1;
            if (slot == EQUIPMENT_SLOT_TRINKET2)
                return EQUIPMENT_SLOT_TRINKET1;
            return slot;
        }

        // Every slot `item` can go in for `bot`.
        std::vector<uint8> SlotsOf(Player* bot, Item const* item)
        {
            std::vector<uint8> slots;
            for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
                if (bot->FindEquipSlot(item->GetTemplate(), slot, true) == slot)
                    slots.push_back(slot);
            return slots;
        }

        // Entry, enchant, gems and random property: what makes two copies of an item the same. The link's unique
        // field is left out, since clients don't agree on it.
        std::string MatchKey(std::string_view itemString)
        {
            std::size_t from = 0;
            for (int field = 0; field < 7; ++field)
            {
                std::size_t const colon = itemString.find(':', from);
                if (colon == std::string_view::npos)
                    return std::string(itemString);
                if (field == 6)
                    return std::string(itemString.substr(0, colon));
                from = colon + 1;
            }
            return std::string(itemString);
        }

        struct LinkRef
        {
            uint32 entry = 0;
            std::string key;  // MatchKey
            std::string text; // the whole link, to quote it back
        };

        // Item links in `text` ("|cff...|Hitem:...|h[Name]|h|r"); `rest` gets the text around them.
        std::vector<LinkRef> TakeLinks(std::string const& text, std::string& rest)
        {
            std::vector<LinkRef> links;
            rest.clear();
            std::size_t from = 0;
            while (true)
            {
                std::size_t const start = text.find("|Hitem:", from);
                if (start == std::string::npos)
                    break;

                std::size_t const fields = start + 7;
                std::size_t const nameStart = text.find("|h", fields);
                if (nameStart == std::string::npos)
                    break;
                std::size_t const nameEnd = text.find("|h", nameStart + 2);
                std::size_t end = nameEnd == std::string::npos ? nameStart + 2 : nameEnd + 2;
                if (text.compare(end, 2, "|r") == 0)
                    end += 2;

                // The color code before it belongs to the link too.
                std::size_t begin = start;
                if (begin >= 10 && text.compare(begin - 10, 2, "|c") == 0)
                    begin -= 10;

                std::string_view const itemString(text.data() + fields, nameStart - fields);
                LinkRef link;
                link.entry = Acore::StringTo<uint32>(itemString.substr(0, itemString.find(':'))).value_or(0);
                link.key = MatchKey(itemString);
                link.text = text.substr(begin, end - begin);
                if (link.entry)
                    links.push_back(std::move(link));

                rest += text.substr(from, begin - from) + " ";
                from = end;
            }
            rest += text.substr(from);
            rest = Acore::String::Trim(rest);
            return links;
        }

        // What a whisper can name: for wear, the bags first, then the wardrobe, then worn gear; for store, the bags,
        // then worn gear. Armor and weapons only.
        std::vector<Item*> Candidates(Player* bot, bool forWear)
        {
            std::vector<Item*> bags;
            std::vector<Item*> stored;
            std::vector<Item*> worn;
            for (ReachableItem const& reachable : Mgr::Instance().Reachable(bot))
            {
                ItemTemplate const* proto = reachable.item->GetTemplate();
                if (proto->Class != ITEM_CLASS_ARMOR && proto->Class != ITEM_CLASS_WEAPON)
                    continue;
                if (reachable.stored)
                    stored.push_back(reachable.item);
                else if (reachable.item->IsEquipped())
                    worn.push_back(reachable.item);
                else
                    bags.push_back(reachable.item);
            }

            std::vector<Item*> out = std::move(bags);
            if (forWear)
                out.insert(out.end(), stored.begin(), stored.end());
            out.insert(out.end(), worn.begin(), worn.end());
            return out;
        }

        // The first candidate matching `link` that isn't in `used`: the exact copy if there is one, else any item of
        // that entry (a link from a loot message or a quest reward has no enchants).
        Item* FindLinked(std::vector<Item*> const& candidates, LinkRef const& link,
            std::set<ObjectGuid::LowType> const& used)
        {
            Item* sameEntry = nullptr;
            for (Item* item : candidates)
            {
                if (item->GetEntry() != link.entry || used.count(item->GetGUID().GetCounter()))
                    continue;
                if (MatchKey(LinkOf(item)) == link.key)
                    return item;
                if (!sameEntry)
                    sameEntry = item;
            }
            return sameEntry;
        }

        // Candidates named `name` (case-insensitive), with or without their random suffix; if none is, those whose
        // name contains it.
        std::vector<Item*> FindNamed(std::vector<Item*> const& candidates, Player* reader, std::string const& name)
        {
            std::string const wanted = Lower(name);
            std::vector<Item*> exact;
            std::vector<Item*> partial;
            for (Item* item : candidates)
            {
                std::string const full = Lower(ItemName(item, reader));
                std::string const base = Lower(item->GetTemplate()->Name1);
                if (full == wanted || base == wanted)
                    exact.push_back(item);
                else if (full.find(wanted) != std::string::npos)
                    partial.push_back(item);
            }
            return exact.empty() ? partial : exact;
        }

        // The items a whisper names: its links, or else one item by name. A trailing slot name ("ring2") goes to
        // `slots` when `withSlot`. False after telling the master what was wrong.
        bool ResolveItems(Player* sender, Player* bot, std::string const& arg, bool forWear, bool withSlot,
            std::vector<Item*>& items, std::vector<uint8>& slots)
        {
            std::vector<Item*> const candidates = Candidates(bot, forWear);
            std::string const where = forWear ? "on me or in the wardrobe" : "on me";

            std::string rest;
            std::vector<LinkRef> const links = TakeLinks(arg, rest);
            if (!links.empty())
            {
                if (!rest.empty())
                {
                    slots = withSlot ? ParseSlots(rest) : std::vector<uint8>();
                    if (slots.empty())
                    {
                        Tell(bot, sender, withSlot ? Acore::StringFormat("I don't know a slot called {}.", rest)
                            : std::string(USAGE));
                        return false;
                    }
                }

                std::set<ObjectGuid::LowType> used;
                for (LinkRef const& link : links)
                {
                    if (Item* item = FindLinked(candidates, link, used))
                    {
                        used.insert(item->GetGUID().GetCounter());
                        items.push_back(item);
                    }
                    else
                        Tell(bot, sender, Acore::StringFormat("I have no {} {}.", link.text, where));
                }
                return !items.empty();
            }

            // By name: the whole text, else the text minus a trailing slot name of one or two words.
            std::vector<Item*> found = FindNamed(candidates, sender, rest);
            if (found.empty() && withSlot)
            {
                for (std::size_t words = 2; words >= 1 && found.empty(); --words)
                {
                    std::size_t cut = rest.size();
                    for (std::size_t i = 0; i < words && cut != std::string::npos; ++i)
                        cut = cut ? rest.rfind(' ', cut - 1) : std::string::npos;
                    if (cut == std::string::npos || cut == 0)
                        continue;
                    std::vector<uint8> trailing = ParseSlots(std::string_view(rest).substr(cut + 1));
                    if (trailing.empty())
                        continue;
                    found = FindNamed(candidates, sender, Acore::String::Trim(rest.substr(0, cut)));
                    if (!found.empty())
                        slots = std::move(trailing);
                }
            }

            if (found.empty())
            {
                Tell(bot, sender, Acore::StringFormat("I have no item called {} {}.", rest, where));
                return false;
            }

            // Several different items match: list them (one link each) and let the master pick.
            std::vector<Item*> distinct;
            for (Item* item : found)
                if (std::none_of(distinct.begin(), distinct.end(), [item](Item* other)
                    { return MatchKey(LinkOf(other)) == MatchKey(LinkOf(item)); }))
                    distinct.push_back(item);
            if (distinct.size() > 1)
            {
                std::string line = "Which one?";
                for (std::size_t i = 0; i < distinct.size() && i < 5; ++i)
                {
                    std::string const link = ChatLink(distinct[i], sender);
                    if (line.size() + link.size() + 1 > MAX_LINE)
                        break;
                    line += " " + link;
                }
                Tell(bot, sender, line);
                return false;
            }

            items.push_back(found.front());
            return true;
        }

        void Items(Player* sender, Player* bot, std::string const& arg)
        {
            std::vector<uint8> filter;
            if (!arg.empty())
            {
                filter = ParseSlots(arg);
                if (filter.empty())
                {
                    Tell(bot, sender, Acore::StringFormat("I don't know a slot called {}.", arg));
                    return;
                }
            }

            struct Usable
            {
                uint8 slot = 0;
                uint32 level = 0;
                std::string link;
                uint32 count = 0;
            };
            std::vector<Usable> usable;
            std::size_t unusable = 0;
            for (ReachableItem const& reachable : Mgr::Instance().Reachable(bot))
            {
                if (!reachable.stored)
                    continue;

                // No slot at all: another class's relic, say.
                std::vector<uint8> slots = SlotsOf(bot, reachable.item);
                if (slots.empty())
                {
                    unusable += filter.empty() ? 1 : 0;
                    continue;
                }
                if (!filter.empty())
                {
                    std::vector<uint8> both;
                    for (uint8 slot : slots)
                        if (std::find(filter.begin(), filter.end(), slot) != filter.end())
                            both.push_back(slot);
                    if (both.empty())
                        continue;
                    slots = std::move(both);
                }
                if (Mgr::Instance().CanWear(bot, reachable.item) != 0)
                {
                    ++unusable;
                    continue;
                }

                std::string link = ChatLink(reachable.item, sender);
                auto same = std::find_if(usable.begin(), usable.end(),
                    [&link](Usable const& other) { return other.link == link; });
                if (same != usable.end())
                    ++same->count;
                else
                    usable.push_back({ slots.front(), reachable.item->GetTemplate()->ItemLevel, std::move(link), 1 });
            }

            std::string const scope = filter.empty() ? "" : Acore::StringFormat(" for {}", arg);
            std::string const others = unusable ? Acore::StringFormat(" ({} more I can't use.)", unusable) : "";
            if (usable.empty())
            {
                Tell(bot, sender, Acore::StringFormat("Nothing in the wardrobe I can use{}.{}", scope, others));
                return;
            }

            // By slot, best item level first.
            std::stable_sort(usable.begin(), usable.end(), [](Usable const& a, Usable const& b)
            {
                if (GroupKey(a.slot) != GroupKey(b.slot))
                    return GroupKey(a.slot) < GroupKey(b.slot);
                return a.level > b.level;
            });

            Tell(bot, sender, Acore::StringFormat("Wardrobe items I can use{}:{}", scope, others));
            std::vector<std::string> lines;
            std::string line;
            std::string group;
            std::size_t shown = 0;
            for (Usable const& item : usable)
            {
                std::string const name = GroupName(item.slot);
                std::string const entry = item.count > 1 ? Acore::StringFormat("{} x{}", item.link, item.count)
                    : item.link;
                if (name != group || line.size() + entry.size() + 1 > MAX_LINE)
                {
                    if (!line.empty())
                        lines.push_back(line);
                    if (lines.size() >= MAX_ITEM_LINES)
                    {
                        line.clear();
                        break;
                    }
                    line = name + ":";
                    group = name;
                }
                line += " " + entry;
                ++shown;
            }
            if (!line.empty())
                lines.push_back(line);
            for (std::string const& text : lines)
                Tell(bot, sender, text);
            if (shown < usable.size())
                Tell(bot, sender, Acore::StringFormat("...and {} more. Narrow it down with gw items <slot>.",
                    usable.size() - shown));
        }

        void Wear(Player* sender, Player* bot, std::string const& arg)
        {
            std::vector<Item*> items;
            std::vector<uint8> slots;
            if (!ResolveItems(sender, bot, arg, true, true, items, slots))
                return;

            bool changed = false;
            std::set<uint8> filled; // a second ring in the same whisper goes to the other finger
            for (Item* item : items)
            {
                // Links first: once worn or refused the pointer may be the wardrobe's again.
                std::string const link = ChatLink(item, sender);
                ObjectGuid::LowType const guid = item->GetGUID().GetCounter();

                WearResult result;
                std::string error;
                if (!Mgr::Instance().Wear(bot, guid, slots, filled, result, error))
                {
                    if (result.failure)
                        Tell(bot, sender, Acore::StringFormat("I can't wear {}: {}.", link, Reason(*result.failure)));
                    else
                        Tell(bot, sender, Acore::StringFormat("{}: {}", link, error.empty()
                            ? "I couldn't wear that." : error));
                    // The old item may already be off (rare): say where it went.
                    changed |= !result.removed.empty();
                    continue;
                }

                changed = true;
                filled.insert(result.slot);
                std::string reply = Acore::StringFormat("Wearing {} ({}).", link, SlotName(result.slot));
                std::string removed;
                for (ObjectGuid::LowType off : result.removed)
                    if (Item const* stored = Mgr::Instance().GetStored(bot, off))
                        removed += (removed.empty() ? "" : " ") + ChatLink(stored, sender);
                if (!removed.empty())
                    reply += Acore::StringFormat(" Into the wardrobe: {}", removed);
                Tell(bot, sender, reply);
            }

            if (changed)
                Refresh(sender);
        }

        void Store(Player* sender, Player* bot, std::string const& arg)
        {
            std::vector<Item*> items;
            std::vector<uint8> slots;
            if (std::vector<uint8> const worn = ParseSlots(arg); !worn.empty())
            {
                // "gw store head": what is worn there.
                for (uint8 slot : worn)
                    if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                        items.push_back(item);
                if (items.empty())
                {
                    Tell(bot, sender, Acore::StringFormat("I'm wearing nothing there ({}).", arg));
                    return;
                }
            }
            else if (!ResolveItems(sender, bot, arg, false, false, items, slots))
                return;

            bool changed = false;
            for (Item* item : items)
            {
                std::string const link = ChatLink(item, sender);
                std::string error;
                if (Mgr::Instance().Store(bot, item->GetGUID().GetCounter(), error))
                {
                    changed = true;
                    Tell(bot, sender, Acore::StringFormat("Put {} in the wardrobe.", link));
                }
                else
                    Tell(bot, sender, Acore::StringFormat("{}: {}", link, error.empty()
                        ? "I couldn't store that." : error));
            }

            if (changed)
                Refresh(sender);
        }

        bool IsWorn(Player* bot, GearSet const& set)
        {
            for (auto const& [slot, entry] : set.slots)
            {
                Item* worn = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
                if (entry.mode == MODE_EMPTY ? worn != nullptr : !worn || worn->GetGUID().GetCounter() != entry.item)
                    return false;
            }
            return !set.slots.empty();
        }

        // By name first, so a set called "2" still works; then by the number `gw list` shows.
        GearSet const* Resolve(Player* bot, Profile const& profile, std::string const& arg)
        {
            if (GearSet const* set = Mgr::Instance().FindSet(bot, arg))
                return set;
            if (Optional<uint32> id = Acore::StringTo<uint32>(arg))
                if (auto itr = profile.sets.find(*id); itr != profile.sets.end())
                    return &itr->second;
            return nullptr;
        }

        void List(Player* sender, Player* bot, Profile const& profile)
        {
            std::size_t const stored = Mgr::Instance().StoredCount(bot);
            if (profile.sets.empty())
            {
                Tell(bot, sender, Acore::StringFormat("I have no gear sets ({} items in my wardrobe). "
                    "Save what I'm wearing with gw save <name>.", stored));
                return;
            }

            Tell(bot, sender, Acore::StringFormat("My gear sets ({} items in my wardrobe):", stored));
            for (auto const& [id, set] : profile.sets)
                Tell(bot, sender, Acore::StringFormat("{}. {} ({} slots){}", id, set.name, set.slots.size(),
                    IsWorn(bot, set) ? " - wearing it" : ""));
        }

        void Equip(Player* sender, Player* bot, Profile const& profile, std::string const& arg)
        {
            GearSet const* set = Resolve(bot, profile, arg);
            if (!set)
            {
                Tell(bot, sender, Acore::StringFormat("I have no gear set called {}. See gw list.", arg));
                return;
            }

            std::string const name = set->name;
            std::string error;
            std::vector<EquipFailure> failures;
            if (!Mgr::Instance().EquipSet(bot, set->id, failures, error))
            {
                Tell(bot, sender, error.empty() ? "I couldn't change my gear." : error);
                return;
            }
            Refresh(sender);

            if (failures.empty())
            {
                Tell(bot, sender, Acore::StringFormat("Equipped {}.", name));
                return;
            }

            Tell(bot, sender, Acore::StringFormat("Equipped {}, but left {} slot(s) as they were:", name,
                failures.size()));
            for (EquipFailure const& failure : failures)
                Tell(bot, sender, Acore::StringFormat("{}: {}", SlotName(failure.slot), Reason(failure)));
        }

        void Save(Player* sender, Player* bot, Profile const& profile, std::string const& arg)
        {
            std::string error;
            if (GearSet const* set = Mgr::Instance().FindSet(bot, arg))
            {
                std::string const name = set->name;
                if (Mgr::Instance().SaveWorn(bot, set->id, error))
                    Tell(bot, sender, Acore::StringFormat("Updated {} with what I'm wearing.", name));
                else
                    Tell(bot, sender, error);
                return;
            }

            // A new set gets the next id and starts as what is worn.
            if (!Mgr::Instance().CreateSet(bot, arg, error))
            {
                Tell(bot, sender, error.empty() ? "I couldn't save that set." : error);
                return;
            }
            GearSet const& set = profile.sets.rbegin()->second;
            Tell(bot, sender, Acore::StringFormat("Saved what I'm wearing as {} ({} slots).", set.name,
                set.slots.size()));
        }
    }

    bool HandleWhisper(Player* sender, Player* bot, std::string const& msg)
    {
        // "gw" alone or followed by a space; everything else is left to playerbots.
        std::string const text = Acore::String::Trim(msg);
        if (!StringStartsWithI(text, "gw") || (text.size() > 2 && text[2] != ' '))
            return false;

        Echo(sender, bot, msg);

        if (!Mgr::MayCommand(sender, bot))
        {
            Tell(bot, sender, "Only my master can change my gear.");
            return true;
        }

        Profile const* profile = Mgr::Instance().GetProfile(bot);
        if (!profile)
        {
            Tell(bot, sender, "I don't have a gear wardrobe.");
            return true;
        }

        std::string const rest = Acore::String::Trim(text.substr(2));
        std::size_t const space = rest.find(' ');
        std::string const verb = rest.substr(0, space);
        std::string const arg = space == std::string::npos ? std::string()
            : Acore::String::Trim(rest.substr(space + 1));

        if (verb.empty() || StringEqualI(verb, "list"))
            List(sender, bot, *profile);
        else if (StringEqualI(verb, "equip") && !arg.empty())
            Equip(sender, bot, *profile, arg);
        else if (StringEqualI(verb, "save") && !arg.empty())
            Save(sender, bot, *profile, arg);
        else if (StringEqualI(verb, "items"))
            Items(sender, bot, arg);
        else if (StringEqualI(verb, "wear") && !arg.empty())
            Wear(sender, bot, arg);
        else if (StringEqualI(verb, "store") && !arg.empty())
            Store(sender, bot, arg);
        else
            Tell(bot, sender, USAGE);
        return true;
    }
}
