-- Embedded in applepie_manager.dll; runs in the game's existing Lua VM.
-- Only Manager UI is implemented here. Plugins still use the C SDK.
-- Return a status even before the native command bridge is available.
for _, name in ipairs({"HL", "UIUtils", "LuaManagerInst", "UIManager", "PanelId", "GEnums", "require_ex", "Register", "MessageConst"}) do
    if not _G[name] then return "waiting:" .. name end
end
if not PanelId.GameSetting then return "waiting:PanelId.GameSetting" end

local prefix = "@ApplepieMenu/v1/"
local disposed = false
local function hex(s)
    return (tostring(s):gsub(".", function(c) return string.format("%02x", string.byte(c)) end))
end
local function send(command)
    if disposed then return end
    -- This reserved path is a command, not a game resource. Its return is unused.
    LuaManagerInst:LoadLua(prefix .. command)
end
if _G.ApplepieNativeSettings then
    if _G.ApplepieNativeSettings.IsDisposed() then return "waiting:LuaEnvRecreation" end
    _G.ApplepieNativeSettings.Ensure()
    send("ready")
    return "ready"
end
local panelModule = require_ex("UI/Panels/GameSetting/GameSettingCtrl")
local baseModule = require_ex("UI/Panels/Base/UICtrl")
local popup = require_ex("UI/Panels/CommonPopUp/CommonPopUpCtrl").CommonPopUpCtrl
local itemWidget = require_ex("UI/Widgets/GameSettingItemCell")
assert(panelModule and panelModule.GameSettingCtrl, "GameSettingCtrl unavailable")
local ctrl = panelModule.GameSettingCtrl
assert(ctrl._BuildSettingDataList and ctrl._RefreshSettingTab and ctrl._RefreshSettingTabCell and itemWidget.InitGameSettingItemCell and ctrl._RefreshSettingItemCell
    and ctrl._DropdownAdjustExpandDirection and ctrl._SetSettingItemControllerNaviTarget, "Game settings contract changed")
assert(popup._ShowPopUp and popup.OnHide and popup.OnClose and popup.AutoOpen, "Popup contract changed")
local tabId = "applepie_manager"
local active, data, page = nil, nil, -1
local chinese = true
local rows = {}
local capturePopup, restoreCaptureUI
local function text(zh, en) return chinese and zh or en end

-- Decode once on the game thread. Both native icon layers retain their own tint.
local iconTexture, iconSprite, iconAttempted
local function tabSprite()
    if iconAttempted then return iconSprite end
    iconAttempted = true
    local ok, iconError = pcall(function()
        local unity = CS.UnityEngine
        local bytes = CS.System.Convert.FromBase64String(applepieTabIconBase64)
        iconTexture = unity.Texture2D(2, 2, unity.TextureFormat.RGBA32, false)
        assert(unity.ImageConversion.LoadImage(iconTexture, bytes, true), "PNG decoding failed")
        iconTexture.name = "ApplepieManagerTabIcon"
        iconTexture.filterMode = unity.FilterMode.Bilinear
        iconTexture.wrapMode = unity.TextureWrapMode.Clamp
        iconSprite = unity.Sprite.Create(iconTexture, unity.Rect(0, 0, iconTexture.width, iconTexture.height),
            Vector2(0.5, 0.5), 100, 0, unity.SpriteMeshType.FullRect)
        assert(iconSprite, "Sprite creation failed")
        iconSprite.name = "ApplepieManagerTabIcon"
        -- Keep this one shared texture/sprite across menu closure and scene changes.
        iconTexture.hideFlags = unity.HideFlags.HideAndDontSave
        iconSprite.hideFlags = unity.HideFlags.HideAndDontSave
    end)
    if not ok then
        if iconSprite then CS.UnityEngine.Object.Destroy(iconSprite) end
        if iconTexture then CS.UnityEngine.Object.Destroy(iconTexture) end
        iconSprite, iconTexture = nil, nil
        send("icon-error/" .. hex(tostring(iconError):sub(1, 768)))
    end
    return iconSprite
end
local function pluginLabel(plugin)
    local color = plugin.state == "Active" and "6FCF97" or "EB5757"
    return plugin.name .. " · <color=#" .. color .. ">" .. plugin.state .. "</color>"
end
local function notice(content)
    popup.ShowPopUp({content = tostring(content), hideCancel = true})
end
local function stopCapture()
    send("cancel-key")
    if capturePopup then capturePopup:Hide(); capturePopup = nil end
end
local function showHeaderVersion(self, show)
    local version = self.view.applepieVersionText
    if show and not version then
        local title = self.view.tabTitleTxt
        local object = CSUtils.CreateObject(title.gameObject, title.rectTransform.parent)
        object.name = "ApplepieManagerVersion"
        version = object:GetComponent(title:GetType())
        -- Keep the game's title font/style, but let the label follow the header's
        -- right edge independently of its title text and the parent's layout.
        local layoutType = typeof(CS.UnityEngine.UI.LayoutElement)
        local layout = object:GetComponent(layoutType) or object:AddComponent(layoutType)
        layout.ignoreLayout = true
        local fitter = object:GetComponent(typeof(CS.UnityEngine.UI.ContentSizeFitter))
        if fitter then fitter.enabled = false end
        local rect = version.rectTransform
        rect.anchorMin = Vector2(0, rect.anchorMin.y)
        rect.anchorMax = Vector2(1, rect.anchorMax.y)
        rect.offsetMin = Vector2(0, rect.offsetMin.y)
        rect.offsetMax = Vector2(-24, rect.offsetMax.y)
        version.enableAutoSizing = false
        version.enableWordWrapping = false
        version.fontSize = title.fontSize * 0.6
        version.horizontalAlignment = CS.TMPro.HorizontalAlignmentOptions.Right
        version.raycastTarget = false
        version.text = applepieVersion
        self.view.applepieVersionText = version
    end
    if version then version.gameObject:SetActive(show) end
end
local function close(self)
    if active == self then
        stopCapture()
        showHeaderVersion(self, false)
        active, data = nil, nil
        send("close")
    end
end
local function button(label, value, click, enabled)
    rows[#rows + 1] = {kind = "button", label = label, value = value, click = click, enabled = enabled ~= false}
end
local function toggle(label, value, change, enabled)
    rows[#rows + 1] = {kind = "toggle", label = label, value = value, change = change, enabled = enabled ~= false}
end
local function dropdown(label, options, selected, change, enabled)
    rows[#rows + 1] = {kind = "dropdown", label = label, options = options, selected = selected, change = change, enabled = enabled ~= false}
end
local render
local function selectPage(index)
    stopCapture()
    page, data = index, nil
    send("open/" .. index)
    render(true)
end
local function editValue(plugin, field, revision)
    -- The shipped popup supports per-open characterLimit. Its next _ShowPopUp
    -- restores the game's default; no shared input delegate needs replacing.
    local instance = popup.AutoOpen(PanelId.CommonPopUp, nil, false)
    instance:_ShowPopUp({content = field.label, subContent = field.description,
        input = true, inputName = tostring(field.value), characterLimit = 255,
        onConfirm = function(value)
            if active and page == plugin then
                send("set/" .. plugin .. "/" .. revision .. "/" .. field.index .. "/" .. hex(value))
            end
        end})
end
local function buildRows()
    rows = {}
    if not data then
        button("Applepie Manager", text("正在读取…", "Loading…"), function() send("refresh") end)
        return
    end
    chinese = data.chinese
    if page == -1 then
        button(text("管理器设置", "Manager settings"), text("配置", "Configure"), function() selectPage(-2) end)
        for _, p in ipairs(data.plugins) do
            local index = p.index
            button(pluginLabel(p), text("配置", "Configure"), function() selectPage(index) end)
        end
        if #data.plugins == 0 then button(text("插件", "Plugins"), text("未发现插件", "No plugins"), function() end, false) end
    elseif page == -2 then
        button(text("返回插件列表", "Back to plugins"), text("返回", "Back"), function() selectPage(-1) end)
        dropdown(text("管理器语言", "Manager language"), {"简体中文", "English"}, chinese and 1 or 2,
            function(i) send("preference/language/" .. (i == 1 and "zh" or "en")) end)
        local labels = {
            {"hide_uid", "隐藏 UID", "Hide UID"}, {"hide_ping", "隐藏延迟", "Hide ping"},
            {"hide_menu_uid", "隐藏菜单 UID", "Hide menu UID"}, {"hide_menu_name", "隐藏菜单名称", "Hide menu name"},
            {"hide_card_uid", "隐藏名片 UID", "Hide card UID"}, {"hide_card_name", "隐藏名片名称", "Hide card name"}}
        for _, item in ipairs(labels) do
            local key = item[1]
            toggle(text(item[2], item[3]), data.preferences[key],
                function(value) send("preference/" .. key .. "/" .. (value and "1" or "0")) end)
        end
    else
        local p = data.plugin
        button(text("返回插件列表", "Back to plugins"), text("返回", "Back"), function() selectPage(-1) end)
        if not p then return end
        button(pluginLabel(p), text("详情", "Details"), function() notice(p.details) end)
        toggle(text("启用插件", "Enable plugin"), p.active,
            function(value) send("enable/" .. p.index .. "/" .. (value and "1" or "0")) end, p.canToggle)
        if p.config then
            local c = p.config
            for _, field in ipairs(c.fields) do
                local f, revision = field, c.revision
                local function set(value) send("set/" .. p.index .. "/" .. revision .. "/" .. f.index .. "/" .. hex(value)) end
                if f.type == 1 then
                    toggle(f.label, f.value == 1, function(value) set(value and "1" or "0") end, not c.pending)
                elseif f.type == 4 then
                    local selected = 1
                    for i, option in ipairs(f.options) do if option == f.value then selected = i end end
                    dropdown(f.label, f.options, selected, function(i) set(f.options[i]) end, not c.pending)
                elseif f.type == 6 then
                    button(f.label, f.display, function()
                        send("key/" .. p.index .. "/" .. revision .. "/" .. f.index)
                        capturePopup = popup.AutoOpen(PanelId.CommonPopUp, nil, false)
                        capturePopup:_ShowPopUp({content = text("请按下按键（Esc 取消）", "Press a key (Esc cancels)"),
                            onCancel = stopCapture})
                        -- Enter/Space are bindable keys, not popup confirmation.
                        local confirm = capturePopup.view.confirmButton.gameObject
                        confirm:SetActive(false)
                        restoreCaptureUI = function() confirm:SetActive(true); restoreCaptureUI = nil end
                    end, not c.pending)
                    button(f.label .. text(" · 清除", " · Clear"), text("清除绑定", "Clear binding"),
                        function() set("0") end, not c.pending and f.value ~= 0)
                else
                    button(f.label, tostring(f.value), function() editValue(p.index, f, revision) end, not c.pending)
                end
            end
            button(text("基础设置", "Basic settings"), c.pending and text("正在保存…", "Saving…") or text("修改后自动保存", "Changes save automatically"), function() end, false)
            if c.error ~= "" then button(text("保存失败", "Save failed"), c.error, function() notice(c.error) end) end
            button(text("恢复默认值", "Restore defaults"), text("重置", "Reset"), function()
                popup.ShowPopUp({content = text("恢复此插件的默认设置？", "Restore this plugin's defaults?"),
                    onConfirm = function() if active and page == p.index then send("defaults/" .. p.index .. "/" .. c.revision) end end})
            end, not c.pending)
        end
    end
    if data.message ~= "" then button(text("操作结果", "Result"), data.message, function() notice(data.message) end) end
    button(text("刷新状态", "Refresh status"), text("刷新", "Refresh"), function() send("refresh") end)
end
-- Current shipped GameSettingCtrl uses GameSettingItemCell widgets. Reuse their
-- templates, cache, state controller and navigation instead of the old beta
-- controller's _GetCachedItem / settingContent fields.
render = function(resetNavigation)
    if not active then return end
    local self = active
    buildRows()
    self.view.tabTitleTxt.text = "Applepie Manager"
    showHeaderVersion(self, true)
    self.view.loadNode.gameObject:SetActive(false)
    self.view.deviceNode.gameObject:SetActive(false)
    self.view.bottomNode.gameObject:SetActive(false)
    self.m_itemCellMap, self.m_qualitySubSettingItemCellMap = {}, {}
    self.m_contentHeight = 0
    local itemDataList, controls = {}, {}
    local types = GEnums.SettingItemType
    -- GameSettingItemCell hides cached controls by iterating this entire map.
    -- Keep native types such as Slider and Key even though our rows do not use them.
    for kind, config in pairs(self.m_itemControlConfigs) do controls[kind] = config end
    for _, kind in ipairs({types.Toggle, types.Dropdown, types.Button}) do
        local nativeConfig = assert(self.m_itemControlConfigs[kind], "Missing native setting control template")
        controls[kind] = {template = nativeConfig.template, initializer = function(cell)
            local row = cell.itemData.applepieRow
            local item = cell.itemControl
            if row.kind == "toggle" then
                item.toggle.view.toggle.checkIsValueValid = nil
                item.toggle:InitCommonToggle(function(value)
                    if active == self and row.enabled then row.change(value) end
                end, row.value, true, {text("开启", "On"), text("关闭", "Off")})
                item.toggle:ToggleInteractable(row.enabled)
            elseif row.kind == "dropdown" then
                item.dropdown:ClearComponent()
                item.dropdown.onToggleOptList:RemoveAllListeners()
                item.dropdown.onValidateSelectCell = nil
                item.dropdown:Init(function(csIndex, option)
                    option:SetText(row.options[LuaIndex(csIndex)])
                    option:SetState("Normal")
                end, function(csIndex)
                    if active == self and row.enabled then row.change(LuaIndex(csIndex)) end
                end)
                item.dropdown.onToggleOptList:AddListener(function(open)
                    if open then self:_DropdownAdjustExpandDirection(cell); item.dropdown:ScrollToSelected() end
                end)
                item.dropdown:Refresh(#row.options, CSIndex(row.selected), false)
                item.dropdown.interactable = row.enabled
            else
                item.buttonText.text = row.value
                item.buttonIcon.gameObject:SetActive(true)
                item.stateCtrl:SetState("NormalState")
                item.button.onClick:RemoveAllListeners()
                item.button.onClick:AddListener(function()
                    if active == self and row.enabled then row.click() end
                end)
                item.button.interactable = row.enabled
            end
        end}
    end
    self.m_itemDataList[self.m_tabIndex] = itemDataList
    self.m_itemCells:Refresh(#rows, function(cell, index)
        local row = rows[index]
        local kind = row.kind == "toggle" and types.Toggle or (row.kind == "dropdown" and types.Dropdown or types.Button)
        local id = "applepie_row_" .. index
        local itemData = {settingId = id, settingText = row.label, settingItemType = kind,
            settingGroupTitle = "", settingRedDot = "", applepieRow = row}
        itemDataList[index] = itemData
        cell:InitGameSettingItemCell(itemData, controls, {})
        self.m_itemCellMap[id] = cell
        local height = self.m_itemCellHeightWithoutTitle
        UIUtils.setSizeDeltaY(cell.rectTransform, height)
        if index > 1 then self.m_contentHeight = self.m_contentHeight + self.view.config.SETTING_ITEM_VERTICAL_SPACE end
        self.m_contentHeight = self.m_contentHeight + height
        local pos = cell.rectTransform.anchoredPosition
        cell.rectTransform.anchoredPosition = Vector2(pos.x, -self.m_contentHeight)
    end)
    UIUtils.setSizeDeltaY(self.view.viewContent, self.m_contentHeight)
    if resetNavigation then
        self:_SetSettingItemControllerNaviTarget()
        self.view.scrollView:ScrollTo(Vector2(0, 0), true)
    end
end

-- Keep integration tied to panel lifecycle, including module hot reloads.
local function addTab(self)
    if disposed then return false end
    if not self.m_tabDataList or not self.m_itemDataList then return false end
    for _, tab in ipairs(self.m_tabDataList) do if tab.tabId == tabId then return false end end
    local icon = self.m_tabDataList[1] and self.m_tabDataList[1].tabIcon or "general"
    table.insert(self.m_tabDataList, {tabId = tabId, tabText = "Applepie Manager", tabIcon = icon,
        tabRedDot = "", tabLeaveFunction = "", tabItems = {}})
    self.m_itemDataList[#self.m_tabDataList] = {}
    return true
end
local attachedPanels = setmetatable({}, {__mode = "k"})
local function ensureTab(self)
    if disposed then return end
    if addTab(self) then self:_InitSettingTabList() end
    assert(self.m_tabDataList and self.m_tabCells, "GameSetting tab controls unavailable")
    for i, tab in ipairs(self.m_tabDataList) do
        if tab.tabId == tabId then
            assert(self.m_tabCells:Get(i), "Applepie tab cell was not created")
            if not attachedPanels[self] then attachedPanels[self] = true; send("tab-ready") end
            return
        end
    end
    error("Applepie tab was not added")
end

local patches = {}
local function patchMethods(className, classObject, definitions)
    local saved = patches[className] or {}
    patches[className] = saved
    local pending = {}
    for name, wrap in pairs(definitions) do
        local current = classObject[name]
        if current ~= saved[name] then
            assert(type(current) == "function", className .. "." .. name .. " unavailable")
            pending[name] = {original = current, wrap = wrap}
        end
    end
    if not next(pending) then return end
    local reloading = HL.IsReloading()
    HL.SetReloading(true)
    local ok, detail = xpcall(function()
        local target = HL.Class(className, baseModule.UICtrl)
        assert(target == classObject, className .. " type identity changed")
        for name, entry in pairs(pending) do target[name] = entry.wrap(entry.original) end
        HL.Commit(target)
    end, debug.traceback)
    HL.SetReloading(reloading)
    if not ok then error(detail) end
    -- HyperLua can log an error without throwing. Read back the installed methods
    -- before acknowledging readiness, and never wrap our own previous wrapper.
    local rejected
    for name, entry in pairs(pending) do
        local current = classObject[name]
        if type(current) == "function" and current ~= entry.original then
            saved[name] = current
        else
            rejected = className .. "." .. name .. " replacement failed"
        end
    end
    assert(not rejected, rejected)
end
local function installHooks()
    ctrl = require_ex("UI/Panels/GameSetting/GameSettingCtrl").GameSettingCtrl
    popup = require_ex("UI/Panels/CommonPopUp/CommonPopUpCtrl").CommonPopUpCtrl
    patchMethods("GameSettingCtrl", ctrl, {
        _BuildSettingDataList = function(original) return HL.Method() << function(self)
            original(self)
            addTab(self)
        end end,
        _RefreshSettingTabCell = function(original) return HL.Method(HL.Table, HL.Number) << function(self, cell, index)
            original(self, cell, index)
            local tab = self.m_tabDataList[index]
            if not disposed and tab and tab.tabId == tabId then
                local sprite = tabSprite()
                if sprite then
                    cell.defaultIcon.sprite = sprite
                    cell.selectedIcon.sprite = sprite
                    cell.defaultIcon.preserveAspect = true
                    cell.selectedIcon.preserveAspect = true
                end
            end
        end end,
        _RefreshSettingTab = function(original) return HL.Method(HL.Number, HL.Boolean, HL.Opt(HL.Boolean)) << function(self, index, init, rebuildData)
            local tab = self.m_tabDataList[index]
            if not disposed and tab and tab.tabId == tabId then
                self.m_tabIndex = index
                if active ~= self or init then active = self; selectPage(-1) else render() end
            else
                close(self)
                original(self, index, init, rebuildData)
            end
        end end,
        OnHide = function(original) return HL.Override() << function(self)
            close(self)
            original(self)
        end end,
        OnShow = function(original) return HL.Override() << function(self)
            original(self)
            if disposed then return end
            ensureTab(self)
            for i, tab in ipairs(self.m_tabDataList) do
                if tab.tabId == tabId then
                    local cell = self.m_tabCells:Get(i)
                    if cell and cell.toggle.isOn then self.m_tabIndex = i; active = self; selectPage(-1) end
                end
            end
        end end,
        OnClose = function(original) return HL.Override() << function(self)
            close(self)
            original(self)
        end end
    })
    -- CommonPopUp is shared: restore the confirmation button on every close
    -- path and before another popup uses it.
    patchMethods("CommonPopUpCtrl", popup, {
        _ShowPopUp = function(original) return HL.Method(HL.Table) << function(self, args)
            if restoreCaptureUI then restoreCaptureUI() end
            original(self, args)
        end end,
        OnHide = function(original) return HL.Override() << function(self)
            if restoreCaptureUI then restoreCaptureUI() end
            original(self)
        end end,
        OnClose = function(original) return HL.Override() << function(self)
            if restoreCaptureUI then restoreCaptureUI() end
            original(self)
        end end
    })
end
local function ensureExisting()
    if disposed then return end
    installHooks()
    local opened, existing = UIManager:IsOpen(PanelId.GameSetting)
    if opened and existing and existing.isFinishedCreation then ensureTab(existing) end
end
local lastPanelError
local function panelEvent(name, opened)
    if disposed then return end
    if name ~= "GameSetting" and name ~= "CommonPopUp" then return end
    local ok, detail = xpcall(function()
        if opened then ensureExisting() else installHooks() end
    end, debug.traceback)
    if not ok then
        if detail ~= lastPanelError then
            lastPanelError = detail
            send("panel-error/" .. hex(tostring(detail):sub(1, 768)))
        end
    else
        lastPanelError = nil
    end
end
local bridge = {
    Ensure = ensureExisting,
    IsDisposed = function() return disposed end,
    OnEvent = function(snapshot)
        if disposed then return end
        if not active or snapshot.page ~= page then return end
        if snapshot.keyFinished and capturePopup then capturePopup:Hide(); capturePopup = nil end
        data = snapshot
        -- Text popup editing remains intact when a background operation finishes.
        render()
    end
}
local function endSession(notifyNative)
    if disposed then return end
    disposed = true
    -- The game's ON_DISPOSE_LUA_ENV may arrive before or after its own UI
    -- cleanup. Drop references without calling an old panel or capture popup.
    active, data, rows, capturePopup, restoreCaptureUI = nil, nil, {}, nil, nil
    attachedPanels = setmetatable({}, {__mode = "k"})
    for _, object in pairs({iconSprite, iconTexture}) do
        pcall(function() CS.UnityEngine.Object.Destroy(object) end)
    end
    iconSprite, iconTexture = nil, nil
    -- Terminal notification bypasses send(), which now rejects old callbacks.
    -- Application quit has already closed the native VM access boundary.
    if notifyNative then LuaManagerInst:LoadLua(prefix .. "session-ended") end
end
assert(MessageConst.ON_UI_PANEL_START_OPEN and MessageConst.ON_UI_PANEL_OPENED
    and MessageConst.ON_DISPOSE_LUA_ENV and MessageConst.ON_APPLICATION_QUIT, "Panel lifecycle contract changed")
Register(MessageConst.ON_UI_PANEL_START_OPEN, function(name) panelEvent(name, false) end, bridge)
Register(MessageConst.ON_UI_PANEL_OPENED, function(name) panelEvent(name, true) end, bridge)
Register(MessageConst.ON_DISPOSE_LUA_ENV, function() endSession(true) end, bridge)
Register(MessageConst.ON_APPLICATION_QUIT, function() endSession(false) end, bridge)
ensureExisting()
_G.ApplepieNativeSettings = bridge
send("ready")
return "ready"
