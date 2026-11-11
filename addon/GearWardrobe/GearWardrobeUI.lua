-- GearWardrobe window: set list, slot editor, wardrobe grid, character-sheet button, quick-swap bar.

local S = GearWardrobe

local LIST_ROWS = 12
local ROW_HEIGHT = 22
local GRID_COLS = 9
local GRID_ROWS = 7
local SLOT_SIZE = 37
local SLOT_ROW_HEIGHT = 18
local SET_MARK = "Interface\\Buttons\\UI-CheckBox-Check"
local ALL_GEAR_ICON = "Interface\\Icons\\INV_Misc_Bag_08"

-- The wardrobe's slot list, top to bottom; 0 is "All gear". Rings and trinkets share one row each.
local WARDROBE_SLOTS = { 0, 1, 2, 3, 15, 5, 4, 19, 9, 10, 6, 7, 8, 11, 13, 16, 17, 18 }
local QUESTION_MARK = "Interface\\Icons\\INV_Misc_QuestionMark"
-- Shared wardrobe: drawn over an item another character has.
local UNAVAILABLE_MARK = "Interface\\RaidFrame\\ReadyCheck-NotReady"

local frame, setsPanel, wardrobePanel, setsTab, wardrobeTab
local listScroll, listRows, editorTitle, iconButton, slotButtons, equipButton, saveButton, renameButton, deleteButton
local gridButtons, gridPage, pageText, countText, searchBox, slotRows, gridTitle, gridCopies
local characterDrop, usableCheck, unusedCheck, sellShownButton, offlineNote
local menuFrame, bar
local selectedId
local filterSlot = 0
local gridList = {}

-- ---------------------------------------------------------------------------------------------------------------
-- Small builders

local function Size(region, width, height)
    region:SetWidth(width)
    region:SetHeight(height)
end

local function CreateButton(parent, label, width)
    local button = CreateFrame("Button", nil, parent, "UIPanelButtonTemplate")
    Size(button, width, 22)
    button:SetText(label)
    return button
end

-- A square item button with an icon, a quality border and a small caption.
local function CreateItemButton(parent, size)
    local button = CreateFrame("Button", nil, parent)
    Size(button, size, size)
    button.icon = button:CreateTexture(nil, "ARTWORK")
    button.icon:SetAllPoints()
    button.border = button:CreateTexture(nil, "OVERLAY")
    button.border:SetTexture("Interface\\Buttons\\UI-ActionButton-Border")
    button.border:SetBlendMode("ADD")
    button.border:SetPoint("CENTER")
    Size(button.border, size * 1.8, size * 1.8)
    button.border:Hide()
    button.caption = button:CreateFontString(nil, "OVERLAY", "NumberFontNormalSmall")
    button.caption:SetPoint("BOTTOMRIGHT", -2, 2)
    button.unavailable = button:CreateTexture(nil, "OVERLAY")
    Size(button.unavailable, size * 0.6, size * 0.6)
    button.unavailable:SetPoint("CENTER")
    button.unavailable:SetTexture(UNAVAILABLE_MARK)
    button.unavailable:Hide()
    button:SetHighlightTexture("Interface\\Buttons\\ButtonHilight-Square", "ADD")
    button:RegisterForClicks("LeftButtonUp", "RightButtonUp")
    return button
end

local function SetQualityBorder(button, quality)
    if quality and quality >= 2 then
        local r, g, b = GetItemQualityColor(quality)
        button.border:SetVertexColor(r, g, b, 0.8)
        button.border:Show()
    else
        button.border:Hide()
    end
end

-- Greys an item out and marks it with a red X: another character has it (shared wardrobe).
local function SetUnavailable(button, unavailable)
    button.icon:SetDesaturated(unavailable)
    if unavailable then
        button.icon:SetVertexColor(0.6, 0.6, 0.6)
        button.unavailable:Show()
    else
        button.unavailable:Hide()
    end
end

-- Greys an item out without the red X: the shown character can't wear it.
local function SetUnusable(button)
    button.icon:SetDesaturated(true)
    button.icon:SetVertexColor(0.5, 0.5, 0.5)
end

-- The class color of a CHARS entry's class id.
local CLASS_FILES = { "WARRIOR", "PALADIN", "HUNTER", "ROGUE", "PRIEST", "DEATHKNIGHT", "SHAMAN", "MAGE", "WARLOCK",
    nil, "DRUID" }

local function CharacterLabel(char)
    local file = CLASS_FILES[char.class]
    local color = file and RAID_CLASS_COLORS[file]
    local name = color and string.format("|cff%02x%02x%02x%s|r", color.r * 255, color.g * 255, color.b * 255, char.name)
        or char.name
    local class = file and LOCALIZED_CLASS_NAMES_MALE and LOCALIZED_CLASS_NAMES_MALE[file] or ""
    return string.format("%s  |cff888888%d %s%s|r", name, char.level, class,
        char.self and ", you" or (char.offline and ", offline" or ""))
end

-- Vendor price of an "item:..." string (GetItemInfo's 11th value), or nil while the client doesn't know it.
local function SellPrice(item)
    return (select(11, GetItemInfo(item or "")))
end

local function Money(copper)
    return copper and GetCoinTextureString(copper) or "its vendor price"
end

local function SelectedSet()
    return selectedId and S.setById[selectedId]
end

local function ShowMenu(entries)
    EasyMenu(entries, menuFrame, "cursor", 0, 0, "MENU")
end

-- ---------------------------------------------------------------------------------------------------------------
-- Popups

local function PopupEditBox(dialog)
    return _G[dialog:GetName() .. "EditBox"]
end

StaticPopupDialogs["GEARWARDROBE_NEW"] = {
    text = "Name the new gear set. It starts as the gear you are wearing.",
    button1 = ACCEPT,
    button2 = CANCEL,
    hasEditBox = 1,
    maxLetters = 24,
    OnShow = function(self)
        PopupEditBox(self):SetText("")
        PopupEditBox(self):SetFocus()
    end,
    OnAccept = function(self)
        S:CreateSet(PopupEditBox(self):GetText())
    end,
    EditBoxOnEnterPressed = function(self)
        S:CreateSet(self:GetText())
        self:GetParent():Hide()
    end,
    EditBoxOnEscapePressed = function(self)
        self:GetParent():Hide()
    end,
    timeout = 0,
    whileDead = 1,
    hideOnEscape = 1,
}

StaticPopupDialogs["GEARWARDROBE_RENAME"] = {
    text = "Rename %s to:",
    button1 = ACCEPT,
    button2 = CANCEL,
    hasEditBox = 1,
    maxLetters = 24,
    OnShow = function(self)
        local set = S.setById[self.data]
        PopupEditBox(self):SetText(set and set.name or "")
        PopupEditBox(self):HighlightText()
        PopupEditBox(self):SetFocus()
    end,
    OnAccept = function(self)
        S:RenameSet(self.data, PopupEditBox(self):GetText())
    end,
    EditBoxOnEnterPressed = function(self)
        local dialog = self:GetParent()
        S:RenameSet(dialog.data, self:GetText())
        dialog:Hide()
    end,
    EditBoxOnEscapePressed = function(self)
        self:GetParent():Hide()
    end,
    timeout = 0,
    whileDead = 1,
    hideOnEscape = 1,
}

StaticPopupDialogs["GEARWARDROBE_DELETE"] = {
    text = "Delete the gear set %s? Its items stay where they are.",
    button1 = YES,
    button2 = NO,
    OnAccept = function(self)
        S:DeleteSet(self.data)
    end,
    timeout = 0,
    whileDead = 1,
    hideOnEscape = 1,
}

-- data = { guids }; the text is built by the caller.
StaticPopupDialogs["GEARWARDROBE_SELL"] = {
    text = "%s",
    button1 = YES,
    button2 = NO,
    OnAccept = function(self)
        S:Sell(self.data)
    end,
    timeout = 0,
    whileDead = 1,
    hideOnEscape = 1,
    showAlert = 1,
}

-- ---------------------------------------------------------------------------------------------------------------
-- Set list

local function UpdateList()
    if not listScroll then return end
    FauxScrollFrame_Update(listScroll, #S.sets, LIST_ROWS, ROW_HEIGHT)
    local offset = FauxScrollFrame_GetOffset(listScroll)
    for i, row in ipairs(listRows) do
        local set = S.sets[offset + i]
        if set then
            row.set = set
            row.icon:SetTexture(S:SetIcon(set))
            row.text:SetText(set.name)
            if set.id == selectedId then row.selected:Show() else row.selected:Hide() end
            row:Show()
        else
            row.set = nil
            row:Hide()
        end
    end
end

-- ---------------------------------------------------------------------------------------------------------------
-- Set editor

local function SlotTooltip(button)
    local set = SelectedSet()
    local entry = set and set.slots[button.slot.id]
    GameTooltip:SetOwner(button, "ANCHOR_RIGHT")
    if entry and entry.mode == S.MODE_ITEM and entry.item and entry.loc == "O" then
        GameTooltip:SetHyperlink(entry.item)
        GameTooltip:AddLine(" ")
        GameTooltip:AddLine((S:HolderOf(entry.guid) or "Another character") .. " has this item", 1, 0.2, 0.2)
        GameTooltip:AddLine("Equipping the set skips this slot until it is back in the wardrobe.", 1, 1, 1, true)
    elseif entry and entry.mode == S.MODE_ITEM and entry.item then
        GameTooltip:SetHyperlink(entry.item)
        GameTooltip:AddLine(" ")
        GameTooltip:AddLine(S.LOCATIONS[entry.loc] or "", entry.loc == "M" and 1 or 0.6, entry.loc == "M" and 0.2 or 0.8, entry.loc == "M" and 0.2 or 1)
    elseif entry and entry.mode == S.MODE_ITEM then
        GameTooltip:AddLine(button.slot.label)
        GameTooltip:AddLine("Item missing (sold, banked or destroyed)", 1, 0.2, 0.2)
    elseif entry and entry.mode == S.MODE_EMPTY then
        GameTooltip:AddLine(button.slot.label)
        GameTooltip:AddLine("Left empty: whatever is worn here is taken off.", 1, 1, 1, true)
    else
        GameTooltip:AddLine(button.slot.label)
        GameTooltip:AddLine("Ignored: this set doesn't change the slot.", 1, 1, 1, true)
    end
    GameTooltip:AddLine("Click to choose an item. Right-click to ignore or empty. Drop gear here to store and assign it.", 0.5, 0.5, 0.5, true)
    GameTooltip:Show()
end

local function SlotMenu(button)
    local set = SelectedSet()
    if not set then return end
    local invSlot = button.slot.id
    local entries = {
        { text = button.slot.label, isTitle = true, notCheckable = true },
        { text = "Ignore this slot", notCheckable = true, func = function() S:SetSlotMode(set.id, invSlot, S.MODE_IGNORE) end },
        { text = "Leave this slot empty", notCheckable = true, func = function() S:SetSlotMode(set.id, invSlot, S.MODE_EMPTY) end },
    }
    local where = { W = "wardrobe", B = "bags", E = "worn" }
    local count, hidden = 0, 0
    for _, candidate in ipairs(S:Candidates(invSlot)) do
        local name, _, quality, texture = S:ItemInfo(candidate.item)
        local why = S:WhyNot(candidate.reason, candidate.item)
        if why and GearWardrobeDB.usableOnly then
            hidden = hidden + 1
        elseif name and count < 25 then
            count = count + 1
            local color = ITEM_QUALITY_COLORS[quality] or ITEM_QUALITY_COLORS[1]
            -- Still assignable when greyed: a set can be planned for a level not reached yet.
            table.insert(entries, {
                text = (why and "|cff808080" or color.hex) .. name .. "|r |cff888888(" .. where[candidate.source]
                    .. ((candidate.count or 1) > 1 and ", " .. candidate.count or "")
                    .. (why and ", " .. why:lower() or "") .. ")|r",
                icon = texture,
                notCheckable = true,
                func = function() S:AssignSlot(set.id, invSlot, candidate) end,
            })
        end
    end
    if count == 0 then
        table.insert(entries, { text = "No matching gear " .. (S.viewing.self and "in your bags or wardrobe"
            or "on " .. S:ViewingName() .. " or in the wardrobe"), disabled = true, notCheckable = true })
    end
    if hidden > 0 then
        table.insert(entries, { text = string.format("%d more %s can't wear (Usable only is on)", hidden,
            S:ViewingName()), disabled = true, notCheckable = true })
    end
    ShowMenu(entries)
end

local function SlotReceiveDrag(button)
    local set = SelectedSet()
    local bag, slot = S:CursorPosition()
    if not set or not bag then return false end
    ClearCursor()
    if not S.viewing.self then
        -- Your item goes into the wardrobe and into their set in one request.
        if S.viewing.samePool then
            S:AssignFromPos(set.id, button.slot.id, bag, slot)
        else
            S:Print(S:ViewingName() .. " doesn't share your wardrobe.")
        end
        return true
    end
    -- Assign first: the server resolves the item by position, then the deposit moves it into the wardrobe.
    S:AssignFromPos(set.id, button.slot.id, bag, slot)
    if bag ~= -1 then
        S:Deposit(bag, slot)
    end
    return true
end

local function UpdateEditor()
    if not slotButtons then return end
    local set = SelectedSet()
    for _, button in ipairs(slotButtons) do
        local entry = set and set.slots[button.slot.id]
        local _, emptyTexture = GetInventorySlotInfo(button.slot.name)
        button.icon:SetVertexColor(1, 1, 1)
        button.icon:SetDesaturated(false)
        button.caption:SetText("")
        SetQualityBorder(button, nil)
        SetUnavailable(button, false)
        if entry and entry.mode == S.MODE_ITEM then
            local _, _, quality, texture = S:ItemInfo(entry.item)
            button.icon:SetTexture(texture or QUESTION_MARK)
            SetQualityBorder(button, quality)
            if entry.loc == "O" then
                SetUnavailable(button, true)
            elseif entry.loc == "M" or entry.loc == "K" then
                button.icon:SetVertexColor(1, 0.2, 0.2)
            end
            button.caption:SetText(entry.loc == "W" and "|cffc8a2ffW|r" or "")
        elseif entry and entry.mode == S.MODE_EMPTY then
            button.icon:SetTexture(emptyTexture)
            button.caption:SetText("|cffff8080X|r")
        else
            button.icon:SetTexture(emptyTexture)
            button.icon:SetDesaturated(true)
            button.icon:SetVertexColor(0.4, 0.4, 0.4)
        end
        if set then button:Enable() else button:Disable() end
    end

    if set then
        editorTitle:SetText(set.name)
        iconButton.icon:SetTexture(S:SetIcon(set))
        iconButton:Show()
        -- Offline characters: their sets can be edited, but nothing can be put on or read from what they wear.
        if S.viewing.offline then
            equipButton:Disable()
            saveButton:Disable()
            offlineNote:SetText(S:ViewingName() .. " is offline: log them in to equip or save worn gear.")
            offlineNote:Show()
        else
            equipButton:Enable()
            saveButton:Enable()
            offlineNote:Hide()
        end
        renameButton:Enable()
        deleteButton:Enable()
    else
        editorTitle:SetText(#S.sets == 0 and "Create a gear set to get started" or "Select a gear set")
        iconButton:Hide()
        equipButton:Disable()
        saveButton:Disable()
        offlineNote:Hide()
        renameButton:Disable()
        deleteButton:Disable()
    end
end

local function IconMenu()
    local set = SelectedSet()
    if not set then return end
    local entries = {
        { text = "Set icon", isTitle = true, notCheckable = true },
        { text = "Automatic", notCheckable = true, func = function() S:SetSetIcon(set.id, "") end },
    }
    local seen = {}
    for _, slot in ipairs(S.SLOTS) do
        local entry = set.slots[slot.id]
        local name, _, _, texture = S:ItemInfo(entry and entry.item)
        if texture and not seen[texture] then
            seen[texture] = true
            table.insert(entries, { text = name, icon = texture, notCheckable = true, func = function() S:SetSetIcon(set.id, texture) end })
        end
    end
    ShowMenu(entries)
end

-- ---------------------------------------------------------------------------------------------------------------
-- Wardrobe grid

-- The grid shows each kind of item once: identical copies (same item, enchant, gems and suffix) share a button,
-- { item, copies = { stored... }, available = <copies in the wardrobe> }. Groups with a stored copy come first.
local function GridFilter()
    gridList = {}
    gridCopies = 0
    local byItem = {}
    local search = searchBox and searchBox:GetText():lower() or ""
    -- Stored items first, then the ones another character took out (shared wardrobe).
    for _, list in ipairs({ S.items, S.out }) do
        for _, stored in ipairs(list) do
            local name = S:ItemInfo(stored.item)
            local matches = search == "" or (name and name:lower():find(search, 1, true))
            -- Copies another character has carry no usable flag; they stay in view.
            local usable = not GearWardrobeDB.usableOnly or not S.noUse[stored.guid]
            local unused = not GearWardrobeDB.unusedOnly or not S.usedBy[stored.guid]
            if matches and usable and unused and (filterSlot == 0 or S:Fits(filterSlot, stored.item)) then
                local group = byItem[stored.item]
                if not group then
                    group = { item = stored.item, copies = {}, available = 0 }
                    byItem[stored.item] = group
                    table.insert(gridList, group)
                end
                table.insert(group.copies, stored)
                if not stored.out then
                    group.available = group.available + 1
                end
                gridCopies = gridCopies + 1
            end
        end
    end
end

-- Every gear set slot, of any character sharing the wardrobe, that uses a copy in `group`.
local function GroupUsedBy(group)
    local uses = {}
    for _, copy in ipairs(group.copies) do
        for _, use in ipairs(S.usedBy[copy.guid] or {}) do
            table.insert(uses, use)
        end
    end
    return uses
end

-- Why the shown character can't wear `group`'s item, or nil (identical copies agree).
local function GroupWhyNot(group)
    for _, copy in ipairs(group.copies) do
        if not copy.out then
            return S:WhyNot(S.noUse[copy.guid], copy.item)
        end
    end
end

-- Stored copies in `group`, the unused ones first.
local function StoredCopies(group)
    local copies = {}
    for _, copy in ipairs(group.copies) do
        if not copy.out and not S.usedBy[copy.guid] then table.insert(copies, copy) end
    end
    for _, copy in ipairs(group.copies) do
        if not copy.out and S.usedBy[copy.guid] then table.insert(copies, copy) end
    end
    return copies
end

-- Every gear set slot of the shown character that uses any copy in `group`.
local function GroupUses(group)
    local uses = {}
    for _, copy in ipairs(group.copies) do
        for _, use in ipairs(S.setsByGuid[copy.guid] or {}) do
            table.insert(uses, use)
        end
    end
    return uses
end

-- The copy a right-click takes out: a stored one no gear set uses, else any stored one.
local function CopyToTake(group)
    local fallback
    for _, copy in ipairs(group.copies) do
        if not copy.out then
            if not S.setsByGuid[copy.guid] then
                return copy
            end
            fallback = fallback or copy
        end
    end
    return fallback
end

-- The slot list on the left: how many stored items fit each slot, and which slot the grid shows.
local function UpdateSlotList()
    local counts = { [0] = #S.items + #S.out }
    for _, list in ipairs({ S.items, S.out }) do
        for _, stored in ipairs(list) do
            for _, row in ipairs(slotRows) do
                if row.invSlot ~= 0 and S:Fits(row.invSlot, stored.item) then
                    counts[row.invSlot] = (counts[row.invSlot] or 0) + 1
                end
            end
        end
    end
    for _, row in ipairs(slotRows) do
        local count = counts[row.invSlot] or 0
        local shade = count == 0 and 0.5 or 1
        row.count:SetText(count)
        row.text:SetTextColor(shade, shade, shade)
        row.icon:SetDesaturated(count == 0)
        if row.invSlot == filterSlot then row.selected:Show() else row.selected:Hide() end
    end
end

local function UpdateGrid()
    if not gridButtons then return end
    GridFilter()
    UpdateSlotList()
    local perPage = GRID_COLS * GRID_ROWS
    local pages = math.max(1, math.ceil(#gridList / perPage))
    gridPage = math.min(gridPage or 1, pages)
    for i, button in ipairs(gridButtons) do
        local group = gridList[(gridPage - 1) * perPage + i]
        button.group = group
        if group then
            local _, _, quality, texture = S:ItemInfo(group.item)
            button.icon:SetTexture(texture or QUESTION_MARK)
            button.icon:SetVertexColor(1, 1, 1)
            SetQualityBorder(button, quality)
            -- The red X only when no copy is in the wardrobe.
            SetUnavailable(button, group.available == 0)
            if group.available > 0 and GroupWhyNot(group) then
                SetUnusable(button)
            end
            local uses = GroupUses(group)
            if #uses > 0 then button.inSet:Show() else button.inSet:Hide() end
            button.caption:SetText(#uses > 1 and #uses or "")
            button.count:SetText(#group.copies > 1 and #group.copies or "")
            button:Show()
        else
            button:Hide()
        end
    end
    pageText:SetText(string.format("Page %d / %d", gridPage, pages))
    if GearWardrobeDB.unusedOnly and S.viewing.samePool then
        local copies = 0
        for _, group in ipairs(gridList) do
            copies = copies + #StoredCopies(group)
        end
        sellShownButton:SetText(string.format("Sell shown (%d)", copies))
        sellShownButton:Show()
        if copies > 0 then sellShownButton:Enable() else sellShownButton:Disable() end
    else
        sellShownButton:Hide()
    end
    countText:SetText(string.format("%d / %d items stored", #S.items, S.maxItems)
        .. (#S.out > 0 and string.format("\n%d taken out by other characters", #S.out) or ""))
    gridTitle:SetText(string.format("%s  |cff888888(%d)|r", filterSlot == 0 and "All gear" or S.SLOTS[filterSlot].label,
        gridCopies))
end

local function SetFilterSlot(invSlot)
    filterSlot = invSlot
    gridPage = 1
    UpdateGrid()
end

local function GridReceiveDrag()
    local bag, slot = S:CursorPosition()
    if not bag then return end
    ClearCursor()
    if S.viewing.samePool then
        S:Deposit(bag, slot)
    else
        S:Print(S:ViewingName() .. " doesn't share your wardrobe; show yourself to store items.")
    end
end

local function SellOne(copy)
    local name = S:ItemInfo(copy.item) or "this item"
    local text = string.format("Sell %s for %s?\nIt leaves the wardrobe for good.", name, Money(SellPrice(copy.item)))
    local uses = S.usedBy[copy.guid]
    if uses then
        local names = {}
        for _, use in ipairs(uses) do
            table.insert(names, use.character .. ": " .. use.set)
        end
        text = text .. "\n\n|cffff8080Used by " .. table.concat(names, ", ") .. ".|r"
    end
    local dialog = StaticPopup_Show("GEARWARDROBE_SELL", text)
    if dialog then dialog.data = { copy.guid } end
end

-- Right-click: take a copy out (to your bags) or sell it. Only for a wardrobe you share.
local function GridItemMenu(group)
    local name, _, quality = S:ItemInfo(group.item)
    local color = ITEM_QUALITY_COLORS[quality] or ITEM_QUALITY_COLORS[1]
    local entries = { { text = color.hex .. (name or "Item") .. "|r", isTitle = true, notCheckable = true } }
    local take = CopyToTake(group)
    if not S.viewing.samePool then
        table.insert(entries, { text = S:ViewingName() .. " doesn't share your wardrobe", disabled = true,
            notCheckable = true })
    elseif not take then
        table.insert(entries, { text = "No copy is in the wardrobe right now", disabled = true, notCheckable = true })
    else
        table.insert(entries, { text = "Take out (to your bags)", notCheckable = true,
            func = function() S:Withdraw(take.guid) end })
        local copy = StoredCopies(group)[1]
        table.insert(entries, { text = "Sell" .. (#group.copies > 1 and " one" or "") .. " for "
            .. Money(SellPrice(group.item)), notCheckable = true, func = function() SellOne(copy) end })
    end
    table.insert(entries, { text = CANCEL, notCheckable = true })
    ShowMenu(entries)
end

local function SellShown()
    local guids, total, unknown = {}, 0, false
    for _, group in ipairs(gridList) do
        for _, copy in ipairs(StoredCopies(group)) do
            table.insert(guids, copy.guid)
            local price = SellPrice(copy.item)
            if price then total = total + price else unknown = true end
        end
    end
    if #guids == 0 then return end
    local dialog = StaticPopup_Show("GEARWARDROBE_SELL", string.format(
        "Sell the %d items shown for %s%s?\nNo gear set uses them. They leave the wardrobe for good.",
        #guids, unknown and "about " or "", GetCoinTextureString(total)))
    if dialog then dialog.data = guids end
end

local function GridItemTooltip(button)
    local group = button.group
    if not group then return end
    GameTooltip:SetOwner(button, "ANCHOR_RIGHT")
    GameTooltip:SetHyperlink(group.item)
    GameTooltip:AddLine(" ")
    if #group.copies > 1 then
        GameTooltip:AddLine(string.format("%d copies, %d in the wardrobe", #group.copies, group.available), 1, 0.82, 0)
    end
    for _, copy in ipairs(group.copies) do
        if copy.out then
            GameTooltip:AddLine((#group.copies > 1 and "  One with " or "") .. copy.holder
                .. (#group.copies > 1 and "" or " has this item"), 1, 0.2, 0.2)
        end
    end
    if group.available == 0 then
        GameTooltip:AddLine("Not available until it is back in the wardrobe.", 1, 1, 1, true)
    end
    local why = GroupWhyNot(group)
    if why then
        GameTooltip:AddLine(S:ViewingName() .. " can't wear this: " .. why, 1, 0.2, 0.2, true)
    end
    local uses = GroupUsedBy(group)
    if #uses > 0 then
        GameTooltip:AddLine("Used by gear sets:", 1, 0.82, 0)
        for _, use in ipairs(uses) do
            GameTooltip:AddLine("  " .. use.character .. ": " .. use.set .. " |cff888888(" .. S.SLOTS[use.invSlot].label
                .. ")|r", 1, 1, 1)
        end
    else
        GameTooltip:AddLine("Not in any gear set", 0.6, 0.6, 0.6)
    end
    GameTooltip:AddLine(group.available == 0 and "Shift-click to link."
        or "Right-click to take it out or sell it. Shift-click to link.", 0.5, 0.5, 0.5, true)
    GameTooltip:Show()
end

-- ---------------------------------------------------------------------------------------------------------------
-- Quick-swap bar

local barButtons = {}

local function UpdateBar()
    if not bar then return end
    if not GearWardrobeDB.showBar or #S.mySets == 0 then
        bar:Hide()
        return
    end
    for i, set in ipairs(S.mySets) do
        local button = barButtons[i]
        if not button then
            button = CreateItemButton(bar, 30)
            button:SetPoint("LEFT", 8 + (i - 1) * 33, 0)
            button:SetScript("OnClick", function(self) S:EquipOwn(self.set.id) end)
            button:SetScript("OnEnter", function(self)
                GameTooltip:SetOwner(self, "ANCHOR_TOP")
                GameTooltip:AddLine(self.set.name)
                GameTooltip:AddLine("Click to equip. Shift-drag the bar to move it.", 0.6, 0.6, 0.6)
                GameTooltip:Show()
            end)
            button:SetScript("OnLeave", function() GameTooltip:Hide() end)
            barButtons[i] = button
        end
        button.set = set
        button.icon:SetTexture(S:SetIcon(set))
        button:Show()
    end
    for i = #S.mySets + 1, #barButtons do
        barButtons[i]:Hide()
    end
    Size(bar, 16 + #S.mySets * 33, 46)
    bar:Show()
end

local function CreateBar()
    bar = CreateFrame("Frame", "GearWardrobeBar", UIParent)
    bar:SetBackdrop({
        bgFile = "Interface\\Tooltips\\UI-Tooltip-Background",
        edgeFile = "Interface\\Tooltips\\UI-Tooltip-Border",
        tile = true, tileSize = 16, edgeSize = 16,
        insets = { left = 4, right = 4, top = 4, bottom = 4 },
    })
    bar:SetBackdropColor(0, 0, 0, 0.6)
    local point = GearWardrobeDB.barPoint
    if point then
        bar:SetPoint(point[1], UIParent, point[1], point[2], point[3])
    else
        bar:SetPoint("BOTTOM", 0, 160)
    end
    bar:EnableMouse(true)
    bar:SetMovable(true)
    bar:SetClampedToScreen(true)
    bar:RegisterForDrag("LeftButton")
    bar:SetScript("OnDragStart", function(self)
        if IsShiftKeyDown() then self:StartMoving() end
    end)
    bar:SetScript("OnDragStop", function(self)
        self:StopMovingOrSizing()
        local anchor, _, _, x, y = self:GetPoint()
        GearWardrobeDB.barPoint = { anchor, x, y }
    end)
    bar:Hide()
end

-- ---------------------------------------------------------------------------------------------------------------
-- Main frame

local function ShowTab(wardrobe)
    if wardrobe then
        setsPanel:Hide()
        wardrobePanel:Show()
        setsTab:UnlockHighlight()
        wardrobeTab:LockHighlight()
    else
        wardrobePanel:Hide()
        setsPanel:Show()
        wardrobeTab:UnlockHighlight()
        setsTab:LockHighlight()
    end
end

local function RefreshAll()
    if selectedId and not S.setById[selectedId] then
        selectedId = nil
    end
    if not selectedId and S.sets[1] then
        selectedId = S.sets[1].id
    end
    if characterDrop then
        local shown
        for _, char in ipairs(S.chars) do
            if (S.target == nil and char.self) or char.guid == S.target then shown = char end
        end
        UIDropDownMenu_SetText(characterDrop, shown and CharacterLabel(shown) or UnitName("player"))
    end
    UpdateList()
    UpdateEditor()
    UpdateGrid()
    UpdateBar()
end

local function CreateSetsPanel()
    setsPanel = CreateFrame("Frame", nil, frame)
    setsPanel:SetAllPoints()

    local listBg = CreateFrame("Frame", nil, setsPanel)
    listBg:SetPoint("TOPLEFT", 20, -72)
    Size(listBg, 190, LIST_ROWS * ROW_HEIGHT + 8)
    listBg:SetBackdrop({
        bgFile = "Interface\\Tooltips\\UI-Tooltip-Background",
        edgeFile = "Interface\\Tooltips\\UI-Tooltip-Border",
        tile = true, tileSize = 16, edgeSize = 16,
        insets = { left = 4, right = 4, top = 4, bottom = 4 },
    })
    listBg:SetBackdropColor(0, 0, 0, 0.6)

    listScroll = CreateFrame("ScrollFrame", "GearWardrobeListScroll", listBg, "FauxScrollFrameTemplate")
    listScroll:SetPoint("TOPLEFT", 4, -4)
    listScroll:SetPoint("BOTTOMRIGHT", -26, 4)
    listScroll:SetScript("OnVerticalScroll", function(self, offset)
        FauxScrollFrame_OnVerticalScroll(self, offset, ROW_HEIGHT, UpdateList)
    end)

    listRows = {}
    for i = 1, LIST_ROWS do
        local row = CreateFrame("Button", nil, listBg)
        Size(row, 160, ROW_HEIGHT)
        row:SetPoint("TOPLEFT", 6, -4 - (i - 1) * ROW_HEIGHT)
        row.icon = row:CreateTexture(nil, "ARTWORK")
        Size(row.icon, 18, 18)
        row.icon:SetPoint("LEFT", 2, 0)
        row.text = row:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
        row.text:SetPoint("LEFT", row.icon, "RIGHT", 6, 0)
        row.text:SetPoint("RIGHT", -2, 0)
        row.text:SetJustifyH("LEFT")
        row.selected = row:CreateTexture(nil, "BACKGROUND")
        row.selected:SetTexture("Interface\\QuestFrame\\UI-QuestTitleHighlight")
        row.selected:SetBlendMode("ADD")
        row.selected:SetAllPoints()
        row:SetHighlightTexture("Interface\\QuestFrame\\UI-QuestTitleHighlight", "ADD")
        row:SetScript("OnClick", function(self)
            selectedId = self.set and self.set.id
            UpdateList()
            UpdateEditor()
        end)
        row:SetScript("OnDoubleClick", function(self)
            if self.set and not S.viewing.offline then S:Equip(self.set.id) end
        end)
        listRows[i] = row
    end

    local newButton = CreateButton(setsPanel, "New", 60)
    newButton:SetPoint("TOPLEFT", listBg, "BOTTOMLEFT", 0, -4)
    newButton:SetScript("OnClick", function() StaticPopup_Show("GEARWARDROBE_NEW") end)
    renameButton = CreateButton(setsPanel, "Rename", 66)
    renameButton:SetPoint("LEFT", newButton, "RIGHT", 2, 0)
    renameButton:SetScript("OnClick", function()
        local set = SelectedSet()
        if set then StaticPopup_Show("GEARWARDROBE_RENAME", set.name, nil, set.id) end
    end)
    deleteButton = CreateButton(setsPanel, "Delete", 60)
    deleteButton:SetPoint("LEFT", renameButton, "RIGHT", 2, 0)
    deleteButton:SetScript("OnClick", function()
        local set = SelectedSet()
        if set then StaticPopup_Show("GEARWARDROBE_DELETE", set.name, nil, set.id) end
    end)

    -- Paper-doll layout: left column, right column, weapons along the bottom.
    local left = { 1, 2, 3, 15, 5, 4, 19, 9 }
    local right = { 10, 6, 7, 8, 11, 12, 13, 14 }
    local bottom = { 16, 17, 18 }
    slotButtons = {}
    local function AddSlot(invSlot, x, y)
        local button = CreateItemButton(setsPanel, SLOT_SIZE)
        button.slot = S.SLOTS[invSlot]
        button:SetPoint("TOPLEFT", x, y)
        button:SetScript("OnEnter", SlotTooltip)
        button:SetScript("OnLeave", function() GameTooltip:Hide() end)
        button:SetScript("OnReceiveDrag", SlotReceiveDrag)
        button:SetScript("OnClick", function(self, mouse)
            if GetCursorInfo() then
                SlotReceiveDrag(self)
                return
            end
            local set = SelectedSet()
            if not set then return end
            if mouse == "RightButton" then
                -- An ignored slot has no entry: right-click empties it; anything else goes back to ignored.
                S:SetSlotMode(set.id, self.slot.id, set.slots[self.slot.id] and S.MODE_IGNORE or S.MODE_EMPTY)
            elseif IsShiftKeyDown() then
                local entry = set.slots[self.slot.id]
                local _, link = S:ItemInfo(entry and entry.item)
                if link then ChatEdit_InsertLink(link) end
            else
                SlotMenu(self)
            end
        end)
        table.insert(slotButtons, button)
    end
    for i, invSlot in ipairs(left) do
        AddSlot(invSlot, 232, -72 - (i - 1) * 41)
    end
    for i, invSlot in ipairs(right) do
        AddSlot(invSlot, 538, -72 - (i - 1) * 41)
    end
    for i, invSlot in ipairs(bottom) do
        AddSlot(invSlot, 342 + (i - 1) * 45, -400)
    end

    editorTitle = setsPanel:CreateFontString(nil, "OVERLAY", "GameFontNormalLarge")
    editorTitle:SetPoint("TOP", setsPanel, "TOPLEFT", 405, -80)
    editorTitle:SetWidth(280)

    iconButton = CreateItemButton(setsPanel, 40)
    iconButton:SetPoint("TOP", setsPanel, "TOPLEFT", 405, -110)
    iconButton:SetScript("OnClick", IconMenu)
    iconButton:SetScript("OnEnter", function(self)
        GameTooltip:SetOwner(self, "ANCHOR_RIGHT")
        GameTooltip:AddLine("Set icon")
        GameTooltip:AddLine("Click to pick one of the set's items.", 0.6, 0.6, 0.6)
        GameTooltip:Show()
    end)
    iconButton:SetScript("OnLeave", function() GameTooltip:Hide() end)

    local legend = setsPanel:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
    legend:SetPoint("TOP", setsPanel, "TOPLEFT", 405, -170)
    legend:SetWidth(250)
    legend:SetJustifyH("CENTER")
    legend:SetText("|cffc8a2ffW|r  stored in the wardrobe\n|cffff8080X|r  slot left empty\n|cff888888Grey|r  slot ignored\n|cffff3333Red|r  item missing or banked\n\n"
        .. "Equipping puts the gear you take off into the wardrobe.")

    equipButton = CreateButton(setsPanel, "Equip", 100)
    equipButton:SetPoint("TOP", setsPanel, "TOPLEFT", 350, -320)
    equipButton:SetScript("OnClick", function()
        local set = SelectedSet()
        if set then S:Equip(set.id) end
    end)
    saveButton = CreateButton(setsPanel, "Save worn gear", 110)
    saveButton:SetPoint("LEFT", equipButton, "RIGHT", 6, 0)
    saveButton:SetScript("OnClick", function()
        local set = SelectedSet()
        if set then S:SaveWorn(set.id) end
    end)
    saveButton:SetScript("OnEnter", function(self)
        GameTooltip:SetOwner(self, "ANCHOR_RIGHT")
        GameTooltip:AddLine("Save worn gear")
        GameTooltip:AddLine("Every slot the set doesn't ignore takes what " .. (S.viewing.self and "you are"
            or S:ViewingName() .. " is") .. " wearing now.", 0.6, 0.6, 0.6, true)
        GameTooltip:Show()
    end)
    saveButton:SetScript("OnLeave", function() GameTooltip:Hide() end)

    offlineNote = setsPanel:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
    offlineNote:SetPoint("TOP", equipButton, "BOTTOM", 58, -6)
    offlineNote:SetWidth(240)
    offlineNote:Hide()
end

local function CreateWardrobePanel()
    wardrobePanel = CreateFrame("Frame", nil, frame)
    wardrobePanel:SetAllPoints()
    wardrobePanel:Hide()

    local backdrop = {
        bgFile = "Interface\\Tooltips\\UI-Tooltip-Background",
        edgeFile = "Interface\\Tooltips\\UI-Tooltip-Border",
        tile = true, tileSize = 16, edgeSize = 16,
        insets = { left = 4, right = 4, top = 4, bottom = 4 },
    }

    -- Left: the slot list. Picking a slot shows its stored gear in the box on the right.
    local slotBg = CreateFrame("Frame", nil, wardrobePanel)
    slotBg:SetPoint("TOPLEFT", 20, -72)
    Size(slotBg, 160, #WARDROBE_SLOTS * SLOT_ROW_HEIGHT + 12)
    slotBg:SetBackdrop(backdrop)
    slotBg:SetBackdropColor(0, 0, 0, 0.6)

    slotRows = {}
    for i, invSlot in ipairs(WARDROBE_SLOTS) do
        local row = CreateFrame("Button", nil, slotBg)
        Size(row, 148, SLOT_ROW_HEIGHT)
        row:SetPoint("TOPLEFT", 6, -6 - (i - 1) * SLOT_ROW_HEIGHT)
        row.invSlot = invSlot
        row.icon = row:CreateTexture(nil, "ARTWORK")
        Size(row.icon, 16, 16)
        row.icon:SetPoint("LEFT", 2, 0)
        row.text = row:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
        row.text:SetPoint("LEFT", row.icon, "RIGHT", 6, 0)
        row.text:SetJustifyH("LEFT")
        row.count = row:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
        row.count:SetPoint("RIGHT", -4, 0)
        row.count:SetTextColor(0.6, 0.6, 0.6)
        if invSlot == 0 then
            row.icon:SetTexture(ALL_GEAR_ICON)
            row.text:SetText("All gear")
        else
            local _, emptyTexture = GetInventorySlotInfo(S.SLOTS[invSlot].name)
            row.icon:SetTexture(emptyTexture)
            row.text:SetText(S.SLOTS[invSlot].label)
        end
        row.selected = row:CreateTexture(nil, "BACKGROUND")
        row.selected:SetTexture("Interface\\QuestFrame\\UI-QuestTitleHighlight")
        row.selected:SetBlendMode("ADD")
        row.selected:SetAllPoints()
        row:SetHighlightTexture("Interface\\QuestFrame\\UI-QuestTitleHighlight", "ADD")
        row:SetScript("OnReceiveDrag", GridReceiveDrag)
        row:SetScript("OnClick", function(self)
            if GetCursorInfo() then
                GridReceiveDrag()
            else
                SetFilterSlot(self.invSlot)
            end
        end)
        slotRows[i] = row
    end

    countText = wardrobePanel:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
    countText:SetPoint("TOPLEFT", slotBg, "BOTTOMLEFT", 6, -8)
    countText:SetJustifyH("LEFT")

    -- With Unused only on: sells every stored copy the grid shows (after a confirmation).
    sellShownButton = CreateButton(wardrobePanel, "Sell shown", 150)
    sellShownButton:SetPoint("TOPLEFT", slotBg, "BOTTOMLEFT", 2, -40)
    sellShownButton:SetScript("OnClick", SellShown)
    sellShownButton:SetScript("OnEnter", function(self)
        GameTooltip:SetOwner(self, "ANCHOR_RIGHT")
        GameTooltip:AddLine("Sell shown")
        GameTooltip:AddLine("Sells every stored item the grid shows for its vendor price; the gold goes to you. "
            .. "Narrow it down with the slot list and the search box first.", 0.6, 0.6, 0.6, true)
        GameTooltip:Show()
    end)
    sellShownButton:SetScript("OnLeave", function() GameTooltip:Hide() end)
    sellShownButton:Hide()

    unusedCheck = CreateFrame("CheckButton", "GearWardrobeUnusedCheck", wardrobePanel, "UICheckButtonTemplate")
    Size(unusedCheck, 24, 24)
    unusedCheck:SetPoint("TOPLEFT", 360, -39)
    _G["GearWardrobeUnusedCheckText"]:SetText("Unused only")
    unusedCheck:SetScript("OnClick", function(self)
        GearWardrobeDB.unusedOnly = self:GetChecked() and true or false
        gridPage = 1
        UpdateGrid()
    end)
    unusedCheck:SetScript("OnEnter", function(self)
        GameTooltip:SetOwner(self, "ANCHOR_RIGHT")
        GameTooltip:AddLine("Unused only")
        GameTooltip:AddLine("Only gear no gear set uses, of any character sharing the wardrobe (offline ones too).",
            0.6, 0.6, 0.6, true)
        GameTooltip:Show()
    end)
    unusedCheck:SetScript("OnLeave", function() GameTooltip:Hide() end)

    -- Right: the chosen slot's gear.
    local gridBg = CreateFrame("Button", nil, wardrobePanel)
    gridBg:SetPoint("TOPLEFT", 188, -104)
    Size(gridBg, 410, slotBg:GetHeight() - 32)
    gridBg:SetBackdrop(backdrop)
    gridBg:SetBackdropColor(0, 0, 0, 0.6)
    gridBg:SetScript("OnReceiveDrag", GridReceiveDrag)
    gridBg:SetScript("OnClick", GridReceiveDrag)

    gridTitle = wardrobePanel:CreateFontString(nil, "OVERLAY", "GameFontNormalLarge")
    gridTitle:SetPoint("BOTTOMLEFT", gridBg, "TOPLEFT", 6, 8)

    searchBox = CreateFrame("EditBox", "GearWardrobeSearch", wardrobePanel, "InputBoxTemplate")
    Size(searchBox, 130, 20)
    searchBox:SetPoint("BOTTOMRIGHT", gridBg, "TOPRIGHT", -4, 6)
    searchBox:SetAutoFocus(false)
    searchBox:SetScript("OnTextChanged", function()
        gridPage = 1
        UpdateGrid()
    end)
    searchBox:SetScript("OnEscapePressed", searchBox.ClearFocus)
    local searchLabel = wardrobePanel:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
    searchLabel:SetPoint("RIGHT", searchBox, "LEFT", -10, 0)
    searchLabel:SetText("Search")

    gridButtons = {}
    for i = 1, GRID_COLS * GRID_ROWS do
        local button = CreateItemButton(gridBg, SLOT_SIZE)
        local col = (i - 1) % GRID_COLS
        local row = math.floor((i - 1) / GRID_COLS)
        button:SetPoint("TOPLEFT", 23 + col * 41, -11 - row * 41)
        -- Marks an item some gear set uses; the caption counts the sets when there's more than one.
        button.inSet = button:CreateTexture(nil, "OVERLAY")
        Size(button.inSet, 18, 18)
        button.inSet:SetPoint("TOPLEFT", -4, 4)
        button.inSet:SetTexture(SET_MARK)
        button.inSet:Hide()
        -- How many copies the button stands for, when more than one.
        button.count = button:CreateFontString(nil, "OVERLAY", "NumberFontNormal")
        button.count:SetPoint("TOPRIGHT", -2, -2)
        button:SetScript("OnEnter", GridItemTooltip)
        button:SetScript("OnLeave", function() GameTooltip:Hide() end)
        button:SetScript("OnReceiveDrag", GridReceiveDrag)
        button:SetScript("OnClick", function(self, mouse)
            if GetCursorInfo() then
                GridReceiveDrag()
            elseif not self.group then
                return
            elseif IsShiftKeyDown() then
                local _, link = S:ItemInfo(self.group.item)
                if link then ChatEdit_InsertLink(link) end
            elseif mouse == "RightButton" then
                GridItemMenu(self.group)
            end
        end)
        gridButtons[i] = button
    end

    local hint = wardrobePanel:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
    hint:SetPoint("TOPLEFT", gridBg, "BOTTOMLEFT", 6, -8)
    hint:SetWidth(230)
    hint:SetJustifyH("LEFT")
    hint:SetText("Drag gear here to store it.\n|T" .. SET_MARK .. ":14|t marks items a gear set uses.\n|T"
        .. UNAVAILABLE_MARK .. ":14|t another character has it.")

    local prev = CreateButton(wardrobePanel, "<", 28)
    prev:SetPoint("TOPRIGHT", gridBg, "BOTTOMRIGHT", -110, -4)
    prev:SetScript("OnClick", function()
        gridPage = math.max(1, (gridPage or 1) - 1)
        UpdateGrid()
    end)
    pageText = wardrobePanel:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
    pageText:SetPoint("LEFT", prev, "RIGHT", 6, 0)
    local nextButton = CreateButton(wardrobePanel, ">", 28)
    nextButton:SetPoint("TOPRIGHT", gridBg, "BOTTOMRIGHT", 0, -4)
    nextButton:SetScript("OnClick", function()
        gridPage = (gridPage or 1) + 1
        UpdateGrid()
    end)
end

local function CreateMainFrame()
    frame = CreateFrame("Frame", "GearWardrobeFrame", UIParent)
    Size(frame, 620, 492)
    frame:SetPoint("CENTER")
    frame:SetFrameStrata("DIALOG")
    frame:SetBackdrop({
        bgFile = "Interface\\DialogFrame\\UI-DialogBox-Background",
        edgeFile = "Interface\\DialogFrame\\UI-DialogBox-Border",
        tile = true, tileSize = 32, edgeSize = 32,
        insets = { left = 11, right = 12, top = 12, bottom = 11 },
    })
    frame:EnableMouse(true)
    frame:SetMovable(true)
    frame:SetClampedToScreen(true)
    frame:RegisterForDrag("LeftButton")
    frame:SetScript("OnDragStart", frame.StartMoving)
    frame:SetScript("OnDragStop", frame.StopMovingOrSizing)
    frame:SetScript("OnShow", function()
        S:Sync()
        RefreshAll()
    end)
    frame:Hide()
    tinsert(UISpecialFrames, "GearWardrobeFrame")

    local title = frame:CreateFontString(nil, "OVERLAY", "GameFontNormalLarge")
    title:SetPoint("TOP", 0, -18)
    title:SetText("Gear Wardrobe")

    local close = CreateFrame("Button", nil, frame, "UIPanelCloseButton")
    close:SetPoint("TOPRIGHT", -6, -6)

    -- Whose sets and wardrobe the window shows: you, or an online altbot you are master of.
    characterDrop = CreateFrame("Frame", "GearWardrobeCharacterDrop", frame, "UIDropDownMenuTemplate")
    characterDrop:SetPoint("TOPLEFT", 0, -10)
    UIDropDownMenu_SetWidth(characterDrop, 170)
    UIDropDownMenu_Initialize(characterDrop, function()
        local heading = false
        for _, char in ipairs(S.chars) do
            if char.offline and not heading then
                heading = true
                local title = UIDropDownMenu_CreateInfo()
                title.text = "Offline (edit sets only)"
                title.isTitle = true
                title.notCheckable = true
                UIDropDownMenu_AddButton(title)
            end
            local info = UIDropDownMenu_CreateInfo()
            info.text = CharacterLabel(char)
            info.checked = (S.target == nil and char.self) or char.guid == S.target
            info.func = function() S:SelectCharacter(char.guid) end
            UIDropDownMenu_AddButton(info)
        end
        if #S.chars <= 1 then
            local info = UIDropDownMenu_CreateInfo()
            info.text = "|cff888888No other characters share your wardrobe|r"
            info.notCheckable = true
            info.disabled = true
            UIDropDownMenu_AddButton(info)
        end
    end)

    usableCheck = CreateFrame("CheckButton", "GearWardrobeUsableCheck", frame, "UICheckButtonTemplate")
    Size(usableCheck, 24, 24)
    usableCheck:SetPoint("TOPLEFT", 240, -39)
    _G["GearWardrobeUsableCheckText"]:SetText("Usable only")
    usableCheck:SetScript("OnClick", function(self)
        GearWardrobeDB.usableOnly = self:GetChecked() and true or false
        gridPage = 1
        UpdateGrid()
    end)
    usableCheck:SetScript("OnEnter", function(self)
        GameTooltip:SetOwner(self, "ANCHOR_RIGHT")
        GameTooltip:AddLine("Usable only")
        GameTooltip:AddLine("Hide gear the shown character can't wear (class, armor type, level, an expansion not "
            .. "reached yet). When off, that gear is greyed out.", 0.6, 0.6, 0.6, true)
        GameTooltip:Show()
    end)
    usableCheck:SetScript("OnLeave", function() GameTooltip:Hide() end)

    setsTab = CreateButton(frame, "Gear Sets", 100)
    setsTab:SetPoint("TOPLEFT", 20, -40)
    setsTab:SetScript("OnClick", function() ShowTab(false) end)
    wardrobeTab = CreateButton(frame, "Wardrobe", 100)
    wardrobeTab:SetPoint("LEFT", setsTab, "RIGHT", 4, 0)
    wardrobeTab:SetScript("OnClick", function() ShowTab(true) end)

    CreateSetsPanel()
    CreateWardrobePanel()
    usableCheck:SetChecked(GearWardrobeDB.usableOnly)
    unusedCheck:SetChecked(GearWardrobeDB.unusedOnly)
    ShowTab(false)
end

-- A button on the character sheet beside the Equipment Manager toggle.
local function CreateCharacterButton()
    local button = CreateButton(PaperDollFrame, "Wardrobe", 74)
    Size(button, 74, 20)
    if GearManagerToggleButton then
        button:SetPoint("RIGHT", GearManagerToggleButton, "LEFT", -4, 0)
    else
        button:SetPoint("TOPRIGHT", PaperDollFrame, "TOPRIGHT", -44, -42)
    end
    button:SetScript("OnClick", function() S:Toggle() end)
end

function S:Toggle()
    if frame:IsShown() then frame:Hide() else frame:Show() end
end

function S:IsShown()
    return frame and frame:IsShown()
end

S:On("LOADED", function()
    menuFrame = CreateFrame("Frame", "GearWardrobeMenu", UIParent, "UIDropDownMenuTemplate")
    CreateMainFrame()
    CreateBar()
    CreateCharacterButton()
    RefreshAll()
end)

S:On("UPDATE", function()
    if frame then RefreshAll() end
end)

SLASH_GEARWARDROBE1 = "/gw"
SLASH_GEARWARDROBE2 = "/wardrobe"
SlashCmdList["GEARWARDROBE"] = function(message)
    message = message or ""
    local command, rest = message:match("^(%S*)%s*(.-)$")
    command = (command or ""):lower()
    if command == "equip" then
        local set = S:FindSet(rest)
        if set then S:EquipOwn(set.id) else S:Print("No gear set named '" .. rest .. "'.") end
    elseif command == "bar" then
        GearWardrobeDB.showBar = not GearWardrobeDB.showBar
        UpdateBar()
    elseif command == "sync" then
        S:Sync()
    else
        S:Toggle()
    end
end
