# mod-gear-wardrobe

Per-character gear sets for AzerothCore (WoW 3.3.5) backed by a server-side **wardrobe**. Gear in the wardrobe
doesn't take bag or bank space, and one click equips a whole set straight from it.

Items stay real: enchants, gems, durability and binding are kept. While stored, an item keeps its `item_instance`
row and loses only its `character_inventory` row, the same way mail holds items.

Optionally the characters of an account share one wardrobe (see [Shared wardrobe](#shared-wardrobe)), and
playerbots altbots can be managed from the addon or by whisper (see [Playerbots](#playerbots)).

## How it works

- **Sets** list one item per slot. A slot can also be marked *empty* (take off whatever is worn there) or
  *ignored* (leave it alone). An item can belong to several sets.
- **Equipping** a set pulls each piece from the wardrobe, your bags, or another worn slot. The gear you take off
  goes into the wardrobe. Items that are missing (sold, destroyed, or in the bank) are skipped. A piece that can't be
  equipped (level, class, progression…) is skipped too: one from your bags stays there, and the chat window tells
  you which slot it was.
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
  - Click a slot to choose an item from the wardrobe, your bags or worn gear. With **Usable only** on, items the
    character can't wear are left out; with it off they are greyed, and can still be picked (to plan a set for a
    level not reached yet).
  - Right-click a slot to toggle ignore/empty.
  - Drop a bag item on a slot to assign it and store it in one step.
  - Click the icon to choose the set's icon.
- **Wardrobe** tab: the list on the left has a row per slot (Head, Neck, Shoulder…) with how many stored items
  fit it; pick one and the box on the right shows that slot's gear (**All gear** shows everything). The search
  box narrows it by name. Items a gear set of the shown character uses carry a check mark (with a number when
  several sets use them); the tooltip lists every gear set that uses the item, of any character sharing the
  wardrobe. Identical copies (same item, enchant, gems and suffix) share one button, with
  the number of copies in its top-right corner. Drag gear here from your bags or the character sheet to store it;
  right-click an item for a menu: **Take out** (to your bags; of several copies, one no gear set uses, if there is
  one) or **Sell**.
  - **Usable only** (on by default) hides gear the shown character can't wear: class, armor or weapon type, level,
    reputation, or, with mod-individual-progression, an expansion it hasn't reached. Off, that gear is greyed and
    the tooltip says why.
  - **Unused only** shows gear no gear set uses, of any character that shares the wardrobe, offline ones included.
    Every tooltip lists who uses an item ("Elwyn: heal (Head)").
  - **Selling**: right-click an item, **Sell**. With Unused only on, **Sell shown** sells every stored copy the grid
    shows (narrow it with the slot list and search first). A confirmation comes first, and it can't be undone. You
    get the vendor price, and sets that used a sold item leave that slot alone from then on. Items without a vendor
    price stay.
- **Character picker** (top left) chooses whose gear sets the window shows:
  - you;
  - any online altbot you are its playerbots master of, in your party or not;
  - offline characters on your account (and, with the shared wardrobe, on linked accounts).

  For the character shown, New, Rename, Delete, the icon and slot editing act on its sets, and the slot menu also
  offers its own worn and carried gear. Usable only and the greyed-out items follow that character. Storing, taking
  out and selling always use your own bags and gold. Dropping an item from your bags on another character's set slot
  stores it and assigns it in one step (shared wardrobe only). Without a shared wardrobe, another character's
  wardrobe is its own: you can look at it but not store or take items there.

  Offline characters can only have their sets edited. New starts from what they wore when they logged out. Equip and
  Save worn gear are greyed out until they are logged in. Whether they can wear an item is a best guess from class,
  race, faction, the armor and weapon types their class learns, and level; reputation, profession and
  mod-individual-progression's expansion lock aren't checked for them.
- `/gw equip <name>` for macros, and `/gw bar` toggles a quick-swap bar (shift-drag to move it). Both always use your
  own sets.

Server commands: `.wardrobe list`, `.wardrobe equip <name>`.

## Playerbots

Random bots (the rndbot and addclass accounts) don't get a wardrobe. Altbots do: each keeps the sets saved on that
character when you played it yourself. Bots have no addon, so you drive an altbot's wardrobe by whispering it:

| Whisper | What the bot does |
|---|---|
| `gw` or `gw list` | Lists its sets by number, marks the one it is wearing, and says how many items are stored |
| `gw equip <name or number>` | Equips the set and tells you which slots it had to skip, and why |
| `gw save <name>` | Saves what it is wearing: updates the set with that name, or creates a new one |
| `gw items [slot]` | Lists the stored items it could wear (class, armor type, level, reputation and other modules' rules such as mod-individual-progression's expansion lock), as item links grouped by slot, best item level first |
| `gw wear <item> [slot]` | Puts the item on, from its bags or the wardrobe. What it replaces goes to the wardrobe. If the server refuses the item, nothing moves and the bot says why |
| `gw store <item or slot>` | Puts an item from its bags or worn gear into the wardrobe; `gw store head` stores what it wears there |

`<item>` is one or more item links (shift-click them from the Wardrobe tab, from `gw items`, or from anywhere else),
or an item name: `gw wear lionheart helm`. A link picks the exact copy (enchant, gems, suffix) when the bot can reach
it, else any copy of that item. A name that fits several different items gets a "Which one?" reply with their links.
`[slot]` is optional: `head`, `neck`, `shoulder`, `back`, `chest`, `shirt`, `tabard`, `wrist`, `hands`, `waist`,
`legs`, `feet`, `ring` / `ring1` / `ring2` (or `finger`...), `trinket` / `trinket1` / `trinket2`, `mainhand` (`mh`),
`offhand` (`oh`, `shield`), `weapon`, `ranged`. Without one, the item goes in a free slot if it fits one; two rings in
one whisper go on both fingers. A two-hander moves the off-hand item to the wardrobe; an off-hand item next to a
two-hander is refused (wear a one-hander first, in the same whisper is fine).

Then `gw save <name>` keeps the result as a set. Example: BRD drops a priest item while the priest isn't with you.
Store it in the wardrobe, whisper the priest altbot `gw items head` (or shift-click the item), `gw wear <link>`, then
`gw save heal`.

Only the bot's master can use these. Spec is separate: whisper playerbots' own `talents switch 1` or
`talents switch 2`. The usual rules apply, so a bot in combat won't change gear. When a bot changes its gear or the
wardrobe, your open addon window refreshes whoever it is showing.

## Shared wardrobe

With `GearWardrobe.SharedPool = 1` the characters of an account share one wardrobe, and so do accounts linked with
mod-playerbots' `.playerbots account link` (unless `GearWardrobe.SharedPool.LinkedAccounts = 0`). Each character
still has its own gear sets; a set can use any stored item.

- An item taken out of the wardrobe (withdrawn or equipped) becomes that character's, soulbound items included.
  Item bindings themselves are not changed.
- The wardrobe never takes an item from another character. If a set uses an item someone else has taken, that slot
  is skipped and the server says who has it.
- Copies of an item are separate items: two characters with the same staff each use their own. If a set's own copy
  is out of reach (another character has it, it is in the bank, or it is gone) and an identical copy is available
  (same item, enchant, gems and random suffix; one you carry first, else one in the wardrobe), the set uses that copy
  and keeps it. This works without the shared wardrobe too, for your own copies.
- Items another character took out stay visible to the others, as unavailable: greyed out with a red X in the
  Wardrobe tab and in the gear sets that use them, and the tooltip names who has the item. They count as available
  again once that character stores them (`gear_wardrobe_out` keeps track, also across restarts).
- `GearWardrobe.MaxItems` is then the limit for the whole shared wardrobe.
- Deleting a character gives its stored items to another character of the group (one on the same account first), so
  they stay in the wardrobe. If there is no one left, they are deleted with the character.
- Links are re-read whenever a character logs in.
- Turning the option off again is safe: each character then sees only the items it stored itself.

## Installation

1. The module lives in `modules/mod-gear-wardrobe`. Re-run CMake and build.
2. Copy `conf/mod_gear_wardrobe.conf.dist` to your server's config directory as `mod_gear_wardrobe.conf`.
3. The characters-database SQL in `data/sql/characters/` is applied by the server's database updater at startup
   (`Updates.EnableDatabases` including the characters database). Without the updater, run
   `data/sql/characters/gear_wardrobe.sql` against the characters database yourself.
4. Install the addon. Keep the addon and the server module from the same version: the addon protocol changes
   between versions.

### Optional modules

Both are detected at build time; without them the module works on its own.

- **mod-playerbots**: random bots get no wardrobe; altbots can be managed by whisper or from the addon's character
  picker (only by their master); `.playerbots account link` links accounts for the shared wardrobe.
- **mod-individual-progression**: its expansion lock counts as "can't wear" in the Usable only filter and in
  `gw items`, and is enforced when equipping, as for any other gear.

## Configuration

| Option | Default | |
|---|---|---|
| `GearWardrobe.Enable` | 1 | Turn the module on or off |
| `GearWardrobe.MaxSets` | 50 | Gear sets per character |
| `GearWardrobe.MaxItems` | 500 | Items you can deposit per character (per shared wardrobe with `SharedPool`). Gear taken off while switching sets always fits |
| `GearWardrobe.SharedPool` | 0 | Characters of an account share one wardrobe (see above) |
| `GearWardrobe.SharedPool.LinkedAccounts` | 1 | With `SharedPool`, also share with playerbots-linked accounts |

## Data

- `gear_wardrobe_item`: stored item GUIDs. The server loads every stored item once at startup and keeps it in memory
  from then on; the database is only written to. A wardrobe row is cleared if its item also shows up in
  `character_inventory` (checked at startup and login; the inventory copy wins) or no longer exists (checked at
  startup).
- `gear_wardrobe_set` and `gear_wardrobe_slot`: the sets.
- `gear_wardrobe_out` (shared wardrobe only): items taken out of the wardrobe and who has them. A row goes when the
  item is stored again or is no longer found on that character.
- Deleting a character removes its rows. Its stored items go with the rest of `item_instance`, unless the wardrobe
  is shared (see above).
- Selling a stored item deletes its `item_instance` row and its wardrobe rows, and removes it from every gear set
  that used it.
- An offline character's sets are kept in memory once shown or after it logs out, so an edit can't be lost to a
  save that hasn't reached the database yet when the character logs in.

## License

AGPL-3.0. See [LICENSE](LICENSE).
