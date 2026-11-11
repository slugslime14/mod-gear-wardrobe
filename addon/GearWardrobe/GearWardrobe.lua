-- GearWardrobe: talks to the mod-gear-wardrobe server module over addon whispers to yourself.
-- Server -> client messages are "CMD:payload"; fields are ',' separated and list entries ';' separated.
-- Sets and the wardrobe live on the server, per character; GearWardrobeDB only holds UI preferences.
-- The window shows one character at a time: you, or an online altbot you are master of (S.target). Requests about
-- sets go to that character ("@guid|CMD:payload"); storing, taking out and selling always use your own bags and gold.

GearWardrobe = {
    PREFIX = "GWRD",
    sets = {},              -- ordered by id: { id, icon, name, slots = { [invSlot] = { mode, guid, loc, item } } }
    setById = {},
    items = {},             -- wardrobe contents: { guid, item }  (item = "item:..." string)
    itemByGuid = {},
    out = {},               -- shared wardrobe: items another character took out: { guid, item, holder, out = true }
    outByGuid = {},
    setsByGuid = {},        -- item guid -> { { set, invSlot }, ... } for every set slot that names that item
    noUse = {},             -- stored item guid -> why the shown character can't wear it (server reason code)
    usedBy = {},            -- item guid -> { { invSlot, character, set }, ... }, every character sharing the wardrobe
    carried = {},           -- another character's gear: { guid, loc = "E"|"B", reason, item }
    chars = {},             -- the characters you can show: { guid, name, class, level, self, offline }
    target = nil,           -- guid of the character shown, nil for yourself
    viewing = { self = true, samePool = true },  -- whose data the last sync held: { guid, name, self, samePool, offline }
    mySets = {},            -- your own sets (the quick-swap bar and /gw equip), whoever is shown
    maxSets = 0,
    maxItems = 0,
    listeners = {},
}

local S = GearWardrobe

S.MODE_ITEM   = 0
S.MODE_EMPTY  = 1
S.MODE_IGNORE = 2

-- Inventory slot ids (1-19) the server's equipment slots 0-18 map to, with their character-sheet names.
S.SLOTS = {
    { id = 1,  name = "HeadSlot",          label = "Head" },
    { id = 2,  name = "NeckSlot",          label = "Neck" },
    { id = 3,  name = "ShoulderSlot",      label = "Shoulder" },
    { id = 4,  name = "ShirtSlot",         label = "Shirt" },
    { id = 5,  name = "ChestSlot",         label = "Chest" },
    { id = 6,  name = "WaistSlot",         label = "Waist" },
    { id = 7,  name = "LegsSlot",          label = "Legs" },
    { id = 8,  name = "FeetSlot",          label = "Feet" },
    { id = 9,  name = "WristSlot",         label = "Wrist" },
    { id = 10, name = "HandsSlot",         label = "Hands" },
    { id = 11, name = "Finger0Slot",       label = "Finger" },
    { id = 12, name = "Finger1Slot",       label = "Finger" },
    { id = 13, name = "Trinket0Slot",      label = "Trinket" },
    { id = 14, name = "Trinket1Slot",      label = "Trinket" },
    { id = 15, name = "BackSlot",          label = "Back" },
    { id = 16, name = "MainHandSlot",      label = "Main Hand" },
    { id = 17, name = "SecondaryHandSlot", label = "Off Hand" },
    { id = 18, name = "RangedSlot",        label = "Ranged" },
    { id = 19, name = "TabardSlot",        label = "Tabard" },
}

-- Which item equip locations fit each inventory slot (the server has the final say).
local FITS = {
    [1] = { INVTYPE_HEAD = true },
    [2] = { INVTYPE_NECK = true },
    [3] = { INVTYPE_SHOULDER = true },
    [4] = { INVTYPE_BODY = true },
    [5] = { INVTYPE_CHEST = true, INVTYPE_ROBE = true },
    [6] = { INVTYPE_WAIST = true },
    [7] = { INVTYPE_LEGS = true },
    [8] = { INVTYPE_FEET = true },
    [9] = { INVTYPE_WRIST = true },
    [10] = { INVTYPE_HAND = true },
    [11] = { INVTYPE_FINGER = true },
    [12] = { INVTYPE_FINGER = true },
    [13] = { INVTYPE_TRINKET = true },
    [14] = { INVTYPE_TRINKET = true },
    [15] = { INVTYPE_CLOAK = true },
    [16] = { INVTYPE_WEAPON = true, INVTYPE_2HWEAPON = true, INVTYPE_WEAPONMAINHAND = true },
    [17] = { INVTYPE_WEAPON = true, INVTYPE_2HWEAPON = true, INVTYPE_WEAPONOFFHAND = true, INVTYPE_SHIELD = true, INVTYPE_HOLDABLE = true },
    [18] = { INVTYPE_RANGED = true, INVTYPE_RANGEDRIGHT = true, INVTYPE_THROWN = true, INVTYPE_RELIC = true },
    [19] = { INVTYPE_TABARD = true },
}

S.LOCATIONS = { E = "Worn", B = "In your bags", K = "In your bank", W = "In the wardrobe", O = "Another character has it",
    M = "Missing" }

-- Who has a taken-out item (shared wardrobe), or nil.
function S:HolderOf(guid)
    local out = guid and self.outByGuid[guid]
    return out and out.holder
end

-- Why the shown character can't wear an item (codes from the server's CanWear).
local REASONS = {
    [2] = "Skill too low",
    [8] = "Can't use this armor or weapon type",
    [10] = "Wrong class or race",
    [11] = "Wrong class or race",
    [14] = "Can't dual wield",
    [17] = "Unique, and already carried",
    [20] = "Can't wear this",
    [39] = "Not allowed right now",
    [64] = "Reputation too low",
    [1000] = "From an expansion not reached yet",
}

-- `reason` is a code from NOUSE / CARRIED; nil when the item can be worn.
function S:WhyNot(reason, item)
    if not reason then
        return nil
    end
    if reason == 1 then
        local _, _, _, _, minLevel = GetItemInfo(item or "")
        return minLevel and ("Requires level " .. minLevel) or "Level too low"
    end
    return REASONS[reason] or ("Can't wear this (error " .. reason .. ")")
end

-- The name of the character shown.
function S:ViewingName()
    return self.viewing.name or UnitName("player")
end

local DEFAULTS = {
    showBar = false,
    barPoint = nil,
    usableOnly = true,   -- hide gear the shown character can't wear
    unusedOnly = false,  -- show only gear no gear set uses
}

-- ---------------------------------------------------------------------------------------------------------------
-- Events between the protocol and the UI

function S:On(event, callback)
    self.listeners[event] = self.listeners[event] or {}
    table.insert(self.listeners[event], callback)
end

function S:Fire(event, ...)
    for _, callback in ipairs(self.listeners[event] or {}) do
        callback(...)
    end
end

function S:Print(text)
    DEFAULT_CHAT_FRAME:AddMessage("|cffc8a2ff[Wardrobe]|r " .. text)
end

-- About yourself. The server answers with your own sync, so the window then shows you.
function S:Send(command, payload)
    SendAddonMessage(self.PREFIX, command .. ":" .. (payload or ""), "WHISPER", UnitName("player"))
end

-- About the character shown. Storing, taking out and selling go through here too: the server still uses your bags
-- and gold for them, and answers with the shown character's sync.
function S:SendFor(command, payload)
    local prefix = self.target and ("@" .. self.target .. "|") or ""
    SendAddonMessage(self.PREFIX, prefix .. command .. ":" .. (payload or ""), "WHISPER", UnitName("player"))
end

function S:Sync()
    self:SendFor("SYNC")
end

-- Shows another character (a guid from S.chars), or yourself with nil.
function S:SelectCharacter(guid)
    local me = self.chars[1]
    if me and guid == me.guid then
        guid = nil
    end
    self.target = guid
    self:Sync()
end

-- ---------------------------------------------------------------------------------------------------------------
-- Item helpers

local scanner = CreateFrame("GameTooltip", "GearWardrobeScanTooltip", UIParent, "GameTooltipTemplate")
scanner:SetOwner(UIParent, "ANCHOR_NONE")
local waitingForItems = false

-- name, link, quality, texture, equipLoc for an "item:..." string; asks the server for unknown items.
function S:ItemInfo(item)
    if not item or item == "" then
        return nil
    end
    local name, link, quality, _, _, _, _, _, equipLoc, texture = GetItemInfo(item)
    if not name then
        scanner:SetHyperlink(item)
        waitingForItems = true
        return nil
    end
    return name, link, quality, texture, equipLoc
end

function S:Fits(invSlot, item)
    local _, _, _, _, equipLoc = self:ItemInfo(item)
    return equipLoc and FITS[invSlot][equipLoc] or false
end

-- "item:..." strings carry no level field past the unique id, which GetItemInfo and tooltips accept.
local function ItemString(link)
    return link and link:match("(item:[%-%d:]+)")
end
S.ItemString = ItemString

function S:SetIcon(set)
    if set.icon and set.icon ~= "" then
        return "Interface\\Icons\\" .. set.icon
    end
    for _, slot in ipairs({ 5, 1, 16, 3 }) do
        local entry = set.slots[slot]
        if entry and entry.item then
            local _, _, _, texture = self:ItemInfo(entry.item)
            if texture then
                return texture
            end
        end
    end
    return "Interface\\Icons\\INV_Misc_QuestionMark"
end

-- One of your own sets, by name.
function S:FindSet(name)
    name = (name or ""):lower()
    for _, set in ipairs(self.mySets) do
        if set.name:lower() == name then
            return set
        end
    end
end

-- Every item that could fill `invSlot`: { item, source = "W"|"B"|"E", guid | bag+slot | invSlot }. Identical stored
-- copies are one candidate (`count` copies), naming a copy no gear set uses if there is one.
function S:Candidates(invSlot)
    local list = {}
    local byItem = {}
    for _, stored in ipairs(self.items) do
        if self:Fits(invSlot, stored.item) then
            local candidate = byItem[stored.item]
            if not candidate then
                candidate = { item = stored.item, source = "W", guid = stored.guid, count = 0,
                    reason = self.noUse[stored.guid] }
                byItem[stored.item] = candidate
                table.insert(list, candidate)
            elseif self.setsByGuid[candidate.guid] and not self.setsByGuid[stored.guid] then
                candidate.guid = stored.guid
            end
            candidate.count = candidate.count + 1
        end
    end
    -- Another character: its own gear comes from the server, since the client only sees yours.
    if not self.viewing.self then
        for _, carried in ipairs(self.carried) do
            if self:Fits(invSlot, carried.item) then
                table.insert(list, { item = carried.item, source = carried.loc, guid = carried.guid,
                    reason = carried.reason, theirs = true })
            end
        end
        return list
    end
    for _, slot in ipairs(self.SLOTS) do
        local item = ItemString(GetInventoryItemLink("player", slot.id))
        if item and self:Fits(invSlot, item) then
            table.insert(list, { item = item, source = "E", invSlot = slot.id })
        end
    end
    for bag = 0, NUM_BAG_SLOTS do
        for bagSlot = 1, GetContainerNumSlots(bag) do
            local item = ItemString(GetContainerItemLink(bag, bagSlot))
            if item and self:Fits(invSlot, item) then
                table.insert(list, { item = item, source = "B", bag = bag, bagSlot = bagSlot })
            end
        end
    end
    return list
end

-- ---------------------------------------------------------------------------------------------------------------
-- Requests

function S:CreateSet(name)
    self:SendFor("NEW", name)
end

function S:RenameSet(id, name)
    self:SendFor("REN", id .. "," .. name)
end

function S:DeleteSet(id)
    self:SendFor("DEL", id)
end

function S:SetSetIcon(id, texture)
    self:SendFor("ICON", id .. "," .. ((texture or ""):match("([^\\]+)$") or ""))
end

function S:SaveWorn(id)
    self:SendFor("SAVE", id)
end

function S:Equip(id)
    self:SendFor("EQUIP", id)
end

function S:SetSlotMode(id, invSlot, mode)
    self:SendFor("SLOT", string.format("%d,%d,%d,0", id, invSlot - 1, mode))
end

-- Your own set, from the quick-swap bar or /gw equip, whoever the window shows.
function S:EquipOwn(id)
    self:Send("EQUIP", id)
end

-- `candidate` comes from S:Candidates.
function S:AssignSlot(id, invSlot, candidate)
    if candidate.source == "W" or candidate.theirs then
        self:SendFor("SLOT", string.format("%d,%d,%d,%s", id, invSlot - 1, self.MODE_ITEM, candidate.guid))
    elseif candidate.source == "E" then
        self:Send("SLOTPOS", string.format("%d,%d,-1,%d", id, invSlot - 1, candidate.invSlot))
    else
        self:Send("SLOTPOS", string.format("%d,%d,%d,%d", id, invSlot - 1, candidate.bag, candidate.bagSlot))
    end
end

-- bag -1 means the character sheet, with `slot` an inventory slot id.
function S:Deposit(bag, slot)
    self:SendFor("DEP", bag .. "," .. slot)
end

-- For another character the server also stores the item (from your bags) before assigning it.
function S:AssignFromPos(id, invSlot, bag, slot)
    self:SendFor("SLOTPOS", string.format("%d,%d,%d,%d", id, invSlot - 1, bag, slot))
end

function S:Withdraw(guid)
    self:SendFor("WD", guid)
end

-- Removes stored items for good; you get their vendor price. Sent in batches that fit one message.
function S:Sell(guids)
    local batch = ""
    for _, guid in ipairs(guids) do
        if #batch + #guid + 1 > 230 then
            self:SendFor("SELL", batch)
            batch = ""
        end
        batch = batch == "" and guid or (batch .. "," .. guid)
    end
    if batch ~= "" then
        self:SendFor("SELL", batch)
    end
end

-- ---------------------------------------------------------------------------------------------------------------
-- Remember where the item on the cursor came from: the cursor itself only knows the item, not its bag slot.

S.cursorSource = nil

hooksecurefunc("PickupContainerItem", function(bag, slot)
    S.cursorSource = { bag = bag, slot = slot, item = ItemString(GetContainerItemLink(bag, slot)) }
end)

hooksecurefunc("PickupInventoryItem", function(invSlot)
    S.cursorSource = { bag = -1, slot = invSlot, item = ItemString(GetInventoryItemLink("player", invSlot)) }
end)

-- The bag position of the item on the cursor, if it is still the one that was picked up.
function S:CursorPosition()
    local kind, _, link = GetCursorInfo()
    local source = self.cursorSource
    if kind ~= "item" or not source or source.item ~= ItemString(link) then
        return nil
    end
    return source.bag, source.slot
end

-- ---------------------------------------------------------------------------------------------------------------
-- Incoming messages

local pending
local handlers = {}

local function Entries(payload)
    return payload:gmatch("[^;]+")
end

-- "guid,name,self,samePool,offline" (older servers send less, or nothing: then it is always you, online).
handlers.BEGIN = function(payload)
    local guid, name, isSelf, samePool, offline = payload:match("^(%d+),([^,]*),(%d),(%d),?(%d?)$")
    pending = { sets = {}, setById = {}, items = {}, itemByGuid = {}, out = {}, outByGuid = {}, noUse = {},
        usedBy = {}, carried = {}, chars = {},
        viewing = { guid = guid, name = name, self = isSelf ~= "0", samePool = samePool ~= "0",
            offline = offline == "1" } }
end

handlers.NOUSE = function(payload)
    if not pending then return end
    for entry in Entries(payload) do
        local guid, reason = entry:match("^(%d+),(%d+)$")
        if guid then
            pending.noUse[guid] = tonumber(reason)
        end
    end
end

-- "guid,slot,character,set"
handlers.USEDBY = function(payload)
    if not pending then return end
    for entry in Entries(payload) do
        local guid, slot, character, set = entry:match("^(%d+),(%d+),([^,]*),(.*)$")
        if guid then
            pending.usedBy[guid] = pending.usedBy[guid] or {}
            table.insert(pending.usedBy[guid], { invSlot = tonumber(slot) + 1, character = character, set = set })
        end
    end
end

-- "guid,loc,reason,item" for another character's worn (E) and carried (B) gear.
handlers.CARRIED = function(payload)
    if not pending then return end
    for entry in Entries(payload) do
        local guid, loc, reason, item = entry:match("^(%d+),(%a),(%d+),(.+)$")
        if guid then
            table.insert(pending.carried, { guid = guid, loc = loc, reason = reason ~= "0" and tonumber(reason) or nil,
                item = "item:" .. item })
        end
    end
end

-- "guid,name,class,level,offline": you first, then your online altbots, then offline characters sharing the
-- wardrobe (their sets can be edited, not equipped).
handlers.CHARS = function(payload)
    if not pending then return end
    for entry in Entries(payload) do
        local guid, name, class, level, offline = entry:match("^(%d+),([^,]*),(%d+),(%d+),?(%d?)$")
        if guid then
            table.insert(pending.chars, { guid = guid, name = name, class = tonumber(class), level = tonumber(level),
                self = #pending.chars == 0, offline = offline == "1" })
        end
    end
end

-- A bot whisper or a command changed what the window may show: ask again for whoever it shows.
handlers.CHANGED = function()
    if S.IsShown and S:IsShown() then
        S:QueueSync()
    end
end

handlers.SETS = function(payload)
    if not pending then return end
    for entry in Entries(payload) do
        local id, icon, name = entry:match("^(%d+),([^,]*),(.*)$")
        if id then
            local set = { id = tonumber(id), icon = icon, name = name, slots = {} }
            table.insert(pending.sets, set)
            pending.setById[set.id] = set
        end
    end
end

handlers.SLOTS = function(payload)
    if not pending then return end
    for entry in Entries(payload) do
        local id, slot, mode, guid, loc, item = entry:match("^(%d+),(%d+),(%d+),(%d+),([^,]),(.*)$")
        local set = id and pending.setById[tonumber(id)]
        if set then
            set.slots[tonumber(slot) + 1] = {
                mode = tonumber(mode),
                guid = guid,
                loc = loc,
                item = item ~= "" and ("item:" .. item) or nil,
            }
        end
    end
end

handlers.ITEMS = function(payload)
    if not pending then return end
    for entry in Entries(payload) do
        local guid, item = entry:match("^(%d+),(.+)$")
        if guid then
            local stored = { guid = guid, item = "item:" .. item }
            table.insert(pending.items, stored)
            pending.itemByGuid[guid] = stored
        end
    end
end

-- Shared wardrobe: "guid,holder,item" for items another character took out.
handlers.OUT = function(payload)
    if not pending then return end
    for entry in Entries(payload) do
        local guid, holder, item = entry:match("^(%d+),([^,]*),(.+)$")
        if guid then
            local out = { guid = guid, holder = holder, item = "item:" .. item, out = true }
            table.insert(pending.out, out)
            pending.outByGuid[guid] = out
        end
    end
end

handlers.END = function(payload)
    if not pending then return end
    local maxSets, maxItems = payload:match("^(%d+),(%d+)$")
    S.sets, S.setById, S.items, S.itemByGuid = pending.sets, pending.setById, pending.items, pending.itemByGuid
    S.out, S.outByGuid = pending.out, pending.outByGuid
    S.noUse, S.usedBy, S.carried = pending.noUse, pending.usedBy, pending.carried
    S.viewing = pending.viewing
    if #pending.chars > 0 then
        S.chars = pending.chars
    end
    -- The server decides who is shown: a bot that went offline falls back to you.
    S.target = (not S.viewing.self) and S.viewing.guid or nil
    if S.viewing.self then
        S.mySets = S.sets
    end
    S.maxSets, S.maxItems = tonumber(maxSets) or 0, tonumber(maxItems) or 0
    pending = nil
    S.setsByGuid = {}
    for _, set in ipairs(S.sets) do
        for _, slot in ipairs(S.SLOTS) do
            local entry = set.slots[slot.id]
            if entry and entry.mode == S.MODE_ITEM and entry.guid ~= "0" then
                S.setsByGuid[entry.guid] = S.setsByGuid[entry.guid] or {}
                table.insert(S.setsByGuid[entry.guid], { set = set, invSlot = slot.id })
            end
        end
    end
    S:Fire("UPDATE")
end

handlers.EQRES = function(payload)
    for entry in Entries(payload) do
        local slot, reason = entry:match("^(%d+),(.+)$")
        local info = slot and S.SLOTS[tonumber(slot) + 1]
        local holder = reason and reason:match("^held,(.*)$")
        if info and holder then
            S:Print(string.format("%s: %s has that item", info.label, holder))
        elseif info then
            S:Print(string.format("%s: %s", info.label, reason == "missing" and "item not found (sold, banked or destroyed)"
                or (S:WhyNot(tonumber(reason)) or "couldn't be equipped") .. ", skipped"))
        end
    end
end

handlers.ERR = function(payload)
    UIErrorsFrame:AddMessage(payload, 1.0, 0.1, 0.1, 1.0)
    S:Print("|cffff6060" .. payload .. "|r")
end

handlers.MSG = function(payload)
    S:Print(payload)
end

local function OnAddonMessage(prefix, message, channel, sender)
    if prefix ~= S.PREFIX or channel ~= "WHISPER" or sender ~= UnitName("player") then
        return
    end
    local colon = message:find(":", 1, true)
    local command = colon and message:sub(1, colon - 1) or message
    local payload = colon and message:sub(colon + 1) or ""
    local handler = handlers[command]
    if handler then
        handler(payload)
    end
end

-- ---------------------------------------------------------------------------------------------------------------
-- Events

local events = CreateFrame("Frame")
events:RegisterEvent("ADDON_LOADED")
events:RegisterEvent("PLAYER_ENTERING_WORLD")
events:RegisterEvent("CHAT_MSG_ADDON")
events:RegisterEvent("BAG_UPDATE")
events:RegisterEvent("UNIT_INVENTORY_CHANGED")

-- Item locations shift as bags change; resync once a burst of changes settles. Items the client had to fetch
-- get a redraw once they arrive.
local syncAt, redrawAt
events:SetScript("OnUpdate", function()
    local now = GetTime()
    if syncAt and now >= syncAt then
        syncAt = nil
        S:Sync()
    end
    if waitingForItems and not redrawAt then
        redrawAt = now + 1
    elseif redrawAt and now >= redrawAt then
        redrawAt = nil
        waitingForItems = false
        S:Fire("UPDATE")
    end
end)

function S:QueueSync()
    syncAt = GetTime() + 0.5
end

events:SetScript("OnEvent", function(_, event, arg1, arg2, arg3, arg4)
    if event == "ADDON_LOADED" and arg1 == "GearWardrobe" then
        GearWardrobeDB = GearWardrobeDB or {}
        for key, value in pairs(DEFAULTS) do
            if GearWardrobeDB[key] == nil then
                GearWardrobeDB[key] = value
            end
        end
        S:Fire("LOADED")
    elseif event == "PLAYER_ENTERING_WORLD" then
        S:Sync()
    elseif event == "CHAT_MSG_ADDON" then
        OnAddonMessage(arg1, arg2, arg3, arg4)
    elseif (event == "BAG_UPDATE" or (event == "UNIT_INVENTORY_CHANGED" and arg1 == "player")) and S.IsShown and S:IsShown() then
        S:QueueSync()
    end
end)
