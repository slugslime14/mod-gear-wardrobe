-- GearWardrobe: talks to the mod-gear-wardrobe server module over addon whispers to yourself.
-- Server -> client messages are "CMD:payload"; fields are ',' separated and list entries ';' separated.
-- Sets and the wardrobe live on the server, per character; GearWardrobeDB only holds UI preferences.

GearWardrobe = {
    PREFIX = "GWRD",
    sets = {},              -- ordered by id: { id, icon, name, slots = { [invSlot] = { mode, guid, loc, item } } }
    setById = {},
    items = {},             -- wardrobe contents: { guid, item }  (item = "item:..." string)
    itemByGuid = {},
    setsByGuid = {},        -- item guid -> { { set, invSlot }, ... } for every set slot that names that item
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

S.LOCATIONS = { E = "Worn", B = "In your bags", K = "In your bank", W = "In the wardrobe", M = "Missing" }

local DEFAULTS = {
    showBar = false,
    barPoint = nil,
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

function S:Send(command, payload)
    SendAddonMessage(self.PREFIX, command .. ":" .. (payload or ""), "WHISPER", UnitName("player"))
end

function S:Sync()
    self:Send("SYNC")
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

function S:FindSet(name)
    name = (name or ""):lower()
    for _, set in ipairs(self.sets) do
        if set.name:lower() == name then
            return set
        end
    end
end

-- Every item that could fill `invSlot`: { item, source = "W"|"B"|"E", guid | bag+slot | invSlot }.
function S:Candidates(invSlot)
    local list = {}
    for _, stored in ipairs(self.items) do
        if self:Fits(invSlot, stored.item) then
            table.insert(list, { item = stored.item, source = "W", guid = stored.guid })
        end
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
    self:Send("NEW", name)
end

function S:RenameSet(id, name)
    self:Send("REN", id .. "," .. name)
end

function S:DeleteSet(id)
    self:Send("DEL", id)
end

function S:SetSetIcon(id, texture)
    self:Send("ICON", id .. "," .. ((texture or ""):match("([^\\]+)$") or ""))
end

function S:SaveWorn(id)
    self:Send("SAVE", id)
end

function S:Equip(id)
    self:Send("EQUIP", id)
end

function S:SetSlotMode(id, invSlot, mode)
    self:Send("SLOT", string.format("%d,%d,%d,0", id, invSlot - 1, mode))
end

-- `candidate` comes from S:Candidates.
function S:AssignSlot(id, invSlot, candidate)
    if candidate.source == "W" then
        self:Send("SLOT", string.format("%d,%d,%d,%s", id, invSlot - 1, self.MODE_ITEM, candidate.guid))
    elseif candidate.source == "E" then
        self:Send("SLOTPOS", string.format("%d,%d,-1,%d", id, invSlot - 1, candidate.invSlot))
    else
        self:Send("SLOTPOS", string.format("%d,%d,%d,%d", id, invSlot - 1, candidate.bag, candidate.bagSlot))
    end
end

-- bag -1 means the character sheet, with `slot` an inventory slot id.
function S:Deposit(bag, slot)
    self:Send("DEP", bag .. "," .. slot)
end

function S:AssignFromPos(id, invSlot, bag, slot)
    self:Send("SLOTPOS", string.format("%d,%d,%d,%d", id, invSlot - 1, bag, slot))
end

function S:Withdraw(guid)
    self:Send("WD", guid)
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

handlers.BEGIN = function()
    pending = { sets = {}, setById = {}, items = {}, itemByGuid = {} }
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

handlers.END = function(payload)
    if not pending then return end
    local maxSets, maxItems = payload:match("^(%d+),(%d+)$")
    S.sets, S.setById, S.items, S.itemByGuid = pending.sets, pending.setById, pending.items, pending.itemByGuid
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
        if info then
            S:Print(string.format("%s: %s", info.label, reason == "missing" and "item not found (sold, banked or destroyed)"
                or "couldn't be equipped (error " .. reason .. "); it is in the wardrobe"))
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
