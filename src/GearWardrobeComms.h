#ifndef MOD_GEAR_WARDROBE_COMMS_H
#define MOD_GEAR_WARDROBE_COMMS_H

#include "Define.h"
#include <string>
#include <string_view>
#include <vector>

class Player;

// Addon protocol. The addon whispers itself with SendAddonMessage("GWRD", "CMD:payload", "WHISPER", me); the server
// answers in kind. Payload fields are ',' separated and list entries ';' separated.
namespace GearWardrobe::Comms
{
    // True when `msg` is an addon whisper for this module; `body` receives the text after the prefix.
    bool Extract(uint32 language, std::string const& msg, std::string& body);

    void Send(Player* player, std::string_view command, std::string_view payload = {});

    // Sends `entries` as one or more `command:` messages, splitting between entries to stay under the wire limit.
    void SendList(Player* player, std::string_view command, std::vector<std::string> const& entries);

    void SendError(Player* player, std::string_view text);
    void SendInfo(Player* player, std::string_view text);
}

#endif
