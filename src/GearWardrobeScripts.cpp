#include "Chat.h"
#include "ChatCommand.h"
#include "GearWardrobeBotChat.h"
#include "GearWardrobeComms.h"
#include "GearWardrobeConfig.h"
#include "GearWardrobeMgr.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "WorldSession.h"

using namespace GearWardrobe;

class GearWardrobeWorldScript : public WorldScript
{
public:
    GearWardrobeWorldScript() : WorldScript("GearWardrobeWorldScript", {
        WORLDHOOK_ON_AFTER_CONFIG_LOAD,
        WORLDHOOK_ON_STARTUP
    }) { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        Cfg().Load();
    }

    void OnStartup() override
    {
        Mgr::Instance().OnStartup();
    }
};

class GearWardrobePlayerScript : public PlayerScript
{
public:
    GearWardrobePlayerScript() : PlayerScript("GearWardrobePlayerScript", {
        PLAYERHOOK_ON_LOGIN,
        PLAYERHOOK_ON_LOGOUT,
        PLAYERHOOK_ON_DELETE,
        PLAYERHOOK_ON_DELETE_FROM_DB,
        PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT
    }) { }

    void OnPlayerLogin(Player* player) override
    {
        if (Mgr::IsWardrobeUser(player))
            Mgr::Instance().OnLogin(player);
    }

    void OnPlayerLogout(Player* player) override
    {
        Mgr::Instance().OnLogout(player);
    }

    void OnPlayerDelete(ObjectGuid guid, uint32 /*accountId*/) override
    {
        Mgr::Instance().OnDelete(guid.GetCounter());
    }

    void OnPlayerDeleteFromDB(CharacterDatabaseTransaction trans, uint32 guid) override
    {
        Mgr::Instance().OnDeleteFromDB(trans, guid);
    }

    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 language, std::string& msg,
        Player* receiver) override
    {
        // A player whispering a bot. This module registers before playerbots, so a wardrobe command swallowed
        // here never reaches the bot's command parser.
        if (Cfg().enable && type == CHAT_MSG_WHISPER && language != LANG_ADDON && receiver && receiver != player
            && receiver->GetSession()->IsHeadless() && !player->GetSession()->IsHeadless())
            return !BotChat::HandleWhisper(player, receiver, msg);

        // The addon whispers itself; swallow those messages so they are never delivered.
        std::string body;
        if (receiver != player || !Comms::Extract(language, msg, body))
            return true;

        Mgr::Instance().HandleAddonMessage(player, body);
        return false;
    }
};

using namespace Acore::ChatCommands;

class GearWardrobeCommandScript : public CommandScript
{
public:
    GearWardrobeCommandScript() : CommandScript("GearWardrobeCommandScript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable wardrobeCommandTable =
        {
            { "list",  HandleList,  SEC_PLAYER, Console::No },
            { "equip", HandleEquip, SEC_PLAYER, Console::No },
        };
        static ChatCommandTable commandTable =
        {
            { "wardrobe", wardrobeCommandTable },
        };
        return commandTable;
    }

    static Player* GetPlayer(ChatHandler* handler)
    {
        if (!Cfg().enable)
        {
            handler->SendSysMessage("The gear wardrobe is disabled.");
            return nullptr;
        }
        return handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
    }

    static bool HandleList(ChatHandler* handler)
    {
        Player* player = GetPlayer(handler);
        Profile const* profile = player ? Mgr::Instance().GetProfile(player) : nullptr;
        if (!profile)
            return false;

        for (auto const& [id, set] : profile->sets)
            handler->PSendSysMessage("  [{}] {} ({} slots)", id, set.name, set.slots.size());
        handler->PSendSysMessage("{} gear sets, {} items in the wardrobe.", profile->sets.size(),
            Mgr::Instance().StoredCount(player));
        return true;
    }

    static bool HandleEquip(ChatHandler* handler, Tail name)
    {
        Player* player = GetPlayer(handler);
        if (!player)
            return false;

        GearSet const* set = Mgr::Instance().FindSet(player, std::string(name));
        if (!set)
        {
            handler->SendSysMessage("No gear set with that name - see .wardrobe list.");
            return false;
        }

        std::string error;
        std::vector<EquipFailure> failures;
        if (!Mgr::Instance().EquipSet(player, set->id, failures, error))
        {
            handler->SendSysMessage(error);
            return false;
        }
        Mgr::Instance().SendEquipResult(player, player, set->id, failures);
        // The addon may be showing an altbot; it syncs again whatever it shows.
        Comms::Send(player, "CHANGED");
        return true;
    }
};

void AddSC_gear_wardrobe_scripts()
{
    new GearWardrobeWorldScript();
    new GearWardrobePlayerScript();
    new GearWardrobeCommandScript();
}
