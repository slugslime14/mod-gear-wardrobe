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

local frame, setsPanel, wardrobePanel, setsTab, wardrobeTab
local listScroll, listRows, editorTitle, iconButton, slotButtons, equipButton, saveButton, renameButton, deleteButton
local gridButtons, gridPage, pageText, countText, searchBox, slotRows, gridTitle
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
    if entry and entry.mode == S.MODE_ITEM and entry.item then
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
    local count = 0
    for _, candidate in ipairs(S:Candidates(invSlot)) do
        local name, _, quality, texture = S:ItemInfo(candidate.item)
        if name and count < 25 then
            count = count + 1
            local color = ITEM_QUALITY_COLORS[quality] or ITEM_QUALITY_COLORS[1]
            table.insert(entries, {
                text = color.hex .. name .. "|r |cff888888(" .. where[candidate.source] .. ")|r",
                icon = texture,
                notCheckable = true,
                func = function() S:AssignSlot(set.id, invSlot, candidate) end,
            })
        end
    end
    if count == 0 then
        table.insert(entries, { text = "No matching gear in your bags or wardrobe", disabled = true, notCheckable = true })
    end
    ShowMenu(entries)
end

local function SlotReceiveDrag(button)
    local set = SelectedSet()
    local bag, slot = S:CursorPosition()
    if not set or not bag then return false end
    ClearCursor()
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
        if entry and entry.mode == S.MODE_ITEM then
            local _, _, quality, texture = S:ItemInfo(entry.item)
            button.icon:SetTexture(texture or QUESTION_MARK)
            SetQualityBorder(button, quality)
            if entry.loc == "M" or entry.loc == "K" then
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
        equipButton:Enable()
        saveButton:Enable()
        renameButton:Enable()
        deleteButton:Enable()
    else
        editorTitle:SetText(#S.sets == 0 and "Create a gear set to get started" or "Select a gear set")
        iconButton:Hide()
        equipButton:Disable()
        saveButton:Disable()
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

local function GridFilter()
    gridList = {}
    local search = searchBox and searchBox:GetText():lower() or ""
    for _, stored in ipairs(S.items) do
        local name = S:ItemInfo(stored.item)
        local matches = search == "" or (name and name:lower():find(search, 1, true))
        if matches and (filterSlot == 0 or S:Fits(filterSlot, stored.item)) then
            table.insert(gridList, stored)
        end
    end
end

-- The slot list on the left: how many stored items fit each slot, and which slot the grid shows.
local function UpdateSlotList()
    local counts = { [0] = #S.items }
    for _, stored in ipairs(S.items) do
        for _, row in ipairs(slotRows) do
            if row.invSlot ~= 0 and S:Fits(row.invSlot, stored.item) then
                counts[row.invSlot] = (counts[row.invSlot] or 0) + 1
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
        local stored = gridList[(gridPage - 1) * perPage + i]
        button.stored = stored
        if stored then
            local _, _, quality, texture = S:ItemInfo(stored.item)
            button.icon:SetTexture(texture or QUESTION_MARK)
            SetQualityBorder(button, quality)
            local uses = S.setsByGuid[stored.guid]
            if uses then button.inSet:Show() else button.inSet:Hide() end
            button.caption:SetText(uses and #uses > 1 and #uses or "")
            button:Show()
        else
            button:Hide()
        end
    end
    pageText:SetText(string.format("Page %d / %d", gridPage, pages))
    countText:SetText(string.format("%d / %d items stored", #S.items, S.maxItems))
    gridTitle:SetText(string.format("%s  |cff888888(%d)|r", filterSlot == 0 and "All gear" or S.SLOTS[filterSlot].label,
        #gridList))
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
    S:Deposit(bag, slot)
end

local function GridItemTooltip(button)
    if not button.stored then return end
    GameTooltip:SetOwner(button, "ANCHOR_RIGHT")
    GameTooltip:SetHyperlink(button.stored.item)
    GameTooltip:AddLine(" ")
    local uses = S.setsByGuid[button.stored.guid]
    if uses then
        GameTooltip:AddLine("Used by gear sets:", 1, 0.82, 0)
        for _, use in ipairs(uses) do
            GameTooltip:AddLine("  " .. use.set.name .. " |cff888888(" .. S.SLOTS[use.invSlot].label .. ")|r", 1, 1, 1)
        end
    else
        GameTooltip:AddLine("Not in any gear set", 0.6, 0.6, 0.6)
    end
    GameTooltip:AddLine("Right-click to take it out. Shift-click to link.", 0.5, 0.5, 0.5, true)
    GameTooltip:Show()
end

-- ---------------------------------------------------------------------------------------------------------------
-- Quick-swap bar

local barButtons = {}

local function UpdateBar()
    if not bar then return end
    if not GearWardrobeDB.showBar or #S.sets == 0 then
        bar:Hide()
        return
    end
    for i, set in ipairs(S.sets) do
        local button = barButtons[i]
        if not button then
            button = CreateItemButton(bar, 30)
            button:SetPoint("LEFT", 8 + (i - 1) * 33, 0)
            button:SetScript("OnClick", function(self) S:Equip(self.set.id) end)
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
    for i = #S.sets + 1, #barButtons do
        barButtons[i]:Hide()
    end
    Size(bar, 16 + #S.sets * 33, 46)
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
            if self.set then S:Equip(self.set.id) end
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
        GameTooltip:AddLine("Every slot the set doesn't ignore takes what you are wearing now.", 0.6, 0.6, 0.6, true)
        GameTooltip:Show()
    end)
    saveButton:SetScript("OnLeave", function() GameTooltip:Hide() end)
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
        button:SetScript("OnEnter", GridItemTooltip)
        button:SetScript("OnLeave", function() GameTooltip:Hide() end)
        button:SetScript("OnReceiveDrag", GridReceiveDrag)
        button:SetScript("OnClick", function(self, mouse)
            if GetCursorInfo() then
                GridReceiveDrag()
            elseif not self.stored then
                return
            elseif IsShiftKeyDown() then
                local _, link = S:ItemInfo(self.stored.item)
                if link then ChatEdit_InsertLink(link) end
            elseif mouse == "RightButton" then
                S:Withdraw(self.stored.guid)
            end
        end)
        gridButtons[i] = button
    end

    local hint = wardrobePanel:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
    hint:SetPoint("TOPLEFT", gridBg, "BOTTOMLEFT", 6, -8)
    hint:SetWidth(230)
    hint:SetJustifyH("LEFT")
    hint:SetText("Drag gear here to store it.\n|T" .. SET_MARK .. ":14|t marks items a gear set uses.")

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
    Size(frame, 620, 470)
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

    setsTab = CreateButton(frame, "Gear Sets", 100)
    setsTab:SetPoint("TOPLEFT", 20, -40)
    setsTab:SetScript("OnClick", function() ShowTab(false) end)
    wardrobeTab = CreateButton(frame, "Wardrobe", 100)
    wardrobeTab:SetPoint("LEFT", setsTab, "RIGHT", 4, 0)
    wardrobeTab:SetScript("OnClick", function() ShowTab(true) end)

    CreateSetsPanel()
    CreateWardrobePanel()
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
        if set then S:Equip(set.id) else S:Print("No gear set named '" .. rest .. "'.") end
    elseif command == "bar" then
        GearWardrobeDB.showBar = not GearWardrobeDB.showBar
        UpdateBar()
    elseif command == "sync" then
        S:Sync()
    else
        S:Toggle()
    end
end
