#include "GearWardrobeBotChat.h"
#include "Chat.h"
#include "GearWardrobeMgr.h"
#include "Item.h"
#include "Player.h"
#include "StringConvert.h"
#include "StringFormat.h"
#include "Util.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include <array>
#include <vector>

#if __has_include("Playerbots.h")
#include "PlayerbotAI.h"
#include "Playerbots.h"
#define GEAR_WARDROBE_PLAYERBOTS
#endif

namespace GearWardrobe::BotChat
{
    namespace
    {
        constexpr std::string_view USAGE = "Wardrobe: gw list, gw equip <set name or number>, gw save <set name>.";

        std::array<char const*, EQUIPMENT_SLOT_END> const SLOT_NAMES =
        {
            "Head", "Neck", "Shoulder", "Shirt", "Chest", "Waist", "Legs", "Feet", "Wrist", "Hands",
            "Finger 1", "Finger 2", "Trinket 1", "Trinket 2", "Back", "Main Hand", "Off Hand", "Ranged", "Tabard"
        };

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

        // Altbots can be on the player's account, a linked account or a guildmate's, so ask playerbots who is in
        // charge of the bot.
        bool MayCommand(Player* sender, Player* bot)
        {
#ifdef GEAR_WARDROBE_PLAYERBOTS
            if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
                return botAI->GetMaster() == sender;
#endif
            return sender->GetSession()->GetAccountId() == bot->GetSession()->GetAccountId();
        }

        std::string Reason(EquipFailure const& failure)
        {
            if (failure.missing)
                return "item is gone";

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
                default:
                    return Acore::StringFormat("can't equip it (error {})", failure.result);
            }
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

            if (failures.empty())
            {
                Tell(bot, sender, Acore::StringFormat("Equipped {}.", name));
                return;
            }

            Tell(bot, sender, Acore::StringFormat("Equipped {}, but left {} slot(s) as they were:", name,
                failures.size()));
            for (EquipFailure const& failure : failures)
                Tell(bot, sender, Acore::StringFormat("{}: {}", SLOT_NAMES[failure.slot], Reason(failure)));
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

        if (!MayCommand(sender, bot))
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
        else
            Tell(bot, sender, USAGE);
        return true;
    }
}
