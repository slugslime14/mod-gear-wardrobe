# mod-gear-wardrobe

Per-character gear sets for AzerothCore (WoW 3.3.5) backed by a server-side **wardrobe**. Gear in the wardrobe
doesn't take bag or bank space, and one click equips a whole set straight from it.

Items stay real: enchants, gems, durability and binding are kept. While stored, an item keeps its `item_instance`
row and loses only its `character_inventory` row, the same way mail holds items.

## How it works

- **Sets** list one item per slot. A slot can also be marked *empty* (take off whatever is worn there) or
  *ignored* (leave it alone). An item can belong to several sets.
- **Equipping** a set pulls each piece from the wardrobe, your bags, or another worn slot. The gear you take off
  goes into the wardrobe. Items that are missing (sold, destroyed, or in the bank) are skipped. A piece that can't be
  equipped (level, class, progression…) ends up in the wardrobe, and the chat window tells you which slot it was.
- **Gear bank**: any armor or weapon can be stored, even if it isn't in a set. Bags, timed items and items in a
  trade can't be stored.
- You can switch only while alive, out of combat, not casting, not trading, and not in an arena match.

## Addon

Copy `addon/GearWardrobe` into `Interface/AddOns`. Open it with `/gw`, `/wardrobe`, or the **Wardrobe** button on the
character sheet.

- **Gear Sets** tab
  - **New** creates a set from what you are wearing.
  - **Rename** and **Delete** work on the selected set.
  - Double-click a set (or press **Equip**) to switch to it.
  - **Save worn gear** updates every non-ignored slot to what you are wearing now.
  - Click a slot to choose an item from the wardrobe, your bags or worn gear.
  - Right-click a slot to toggle ignore/empty.
  - Drop a bag item on a slot to assign it and store it in one step.
  - Click the icon to choose the set's icon.
- **Wardrobe** tab: the list on the left has a row per slot (Head, Neck, Shoulder…) with how many stored items
  fit it; pick one and the box on the right shows that slot's gear (**All gear** shows everything). The search
  box narrows it by name. Items a gear set uses carry a check mark (with a number when several sets use them),
  and their tooltip names the sets. Drag gear here from your bags or the character sheet to store it;
  right-click an item to take it out.
- `/gw equip <name>` for macros, and `/gw bar` toggles a quick-swap bar (shift-drag to move it).

Server commands: `.wardrobe list`, `.wardrobe equip <name>`.

## Playerbots

Random bots (the rndbot and addclass accounts) don't get a wardrobe. Altbots do: each keeps the sets saved on that
character when you played it yourself. Bots have no addon, so you drive an altbot's wardrobe by whispering it:

| Whisper | What the bot does |
|---|---|
| `gw` or `gw list` | Lists its sets by number, marks the one it is wearing, and says how many items are stored |
| `gw equip <name or number>` | Equips the set and tells you which slots it had to skip, and why |
| `gw save <name>` | Saves what it is wearing: updates the set with that name, or creates a new one |

Only the bot's master can use these. Spec is separate: whisper playerbots' own `talents switch 1` or
`talents switch 2`. The usual rules apply, so a bot in combat won't change gear.

## Installation

1. The module lives in `modules/mod-gear-wardrobe`. Re-run CMake and build.
2. Copy `conf/mod_gear_wardrobe.conf.dist` to your server's config directory as `mod_gear_wardrobe.conf`.
3. Run `data/sql/characters/gear_wardrobe.sql` against the characters database.
4. Install the addon.

## Configuration

| Option | Default | |
|---|---|---|
| `GearWardrobe.Enable` | 1 | Turn the module on or off |
| `GearWardrobe.MaxSets` | 50 | Gear sets per character |
| `GearWardrobe.MaxItems` | 500 | Items you can deposit per character. Gear taken off while switching sets always fits |

## Data

- `gear_wardrobe_item`: stored item GUIDs. A wardrobe row is cleared if its item also shows up in
  `character_inventory` (checked at login) or no longer exists (checked at startup).
- `gear_wardrobe_set` and `gear_wardrobe_slot`: the sets.
- Deleting a character removes its rows. Its stored items go with the rest of `item_instance`.

## License

AGPL-3.0. See [LICENSE](LICENSE).
