#include "GearWardrobeComms.h"
#include "Chat.h"
#include "Player.h"
#include "SharedDefines.h"
#include "WorldPacket.h"
#include "WorldSession.h"

namespace GearWardrobe::Comms
{
    namespace
    {
        constexpr std::string_view ENVELOPE = "GWRD\t";
        // The client drops chat packets whose text exceeds 255 bytes; leave headroom for the envelope.
        constexpr std::size_t MAX_WIRE = 250;

        void SendWire(Player* player, std::string const& wire)
        {
            if (!player || !player->GetSession())
                return;

            WorldPacket data;
            ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, player, player, wire);
            player->SendDirectMessage(&data);
        }

        // Free-text fields share the separators of the protocol.
        std::string Clean(std::string_view text)
        {
            std::string out(text);
            for (char& c : out)
                if (c == '\t' || c == '\n' || c == '\r')
                    c = ' ';
            return out;
        }
    }

    bool Extract(uint32 language, std::string const& msg, std::string& body)
    {
        if (language != LANG_ADDON || msg.size() <= ENVELOPE.size() || msg.compare(0, ENVELOPE.size(), ENVELOPE) != 0)
            return false;

        body = msg.substr(ENVELOPE.size());
        return true;
    }

    void Send(Player* player, std::string_view command, std::string_view payload)
    {
        std::string wire(ENVELOPE);
        wire.append(command);
        wire.push_back(':');
        wire.append(payload);
        if (wire.size() > MAX_WIRE)
            wire.resize(MAX_WIRE);
        SendWire(player, wire);
    }

    void SendList(Player* player, std::string_view command, std::vector<std::string> const& entries)
    {
        std::string header(ENVELOPE);
        header.append(command);
        header.push_back(':');

        std::string wire = header;
        bool empty = true;
        for (std::string const& entry : entries)
        {
            std::size_t const extra = entry.size() + (empty ? 0 : 1);
            if (!empty && wire.size() + extra > MAX_WIRE)
            {
                SendWire(player, wire);
                wire = header;
                empty = true;
            }

            if (!empty)
                wire.push_back(';');
            wire.append(entry);
            empty = false;
        }

        if (!empty)
            SendWire(player, wire);
    }

    void SendError(Player* player, std::string_view text)
    {
        Send(player, "ERR", Clean(text));
    }

    void SendInfo(Player* player, std::string_view text)
    {
        Send(player, "MSG", Clean(text));
    }
}
