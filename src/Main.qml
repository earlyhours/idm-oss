import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

ApplicationWindow {
    id: win
    width: 1220; height: 760
    minimumWidth: 820; minimumHeight: 500
    visible: typeof startHidden === "undefined" || !startHidden
    title: qsTr("IDM Native")
    color: win.canvas
    font.family: "Adwaita Sans"
    font.pixelSize: 13
    palette.window: win.panel
    palette.windowText: win.ink
    palette.base: win.surface
    palette.text: win.ink
    palette.button: win.panel
    palette.buttonText: win.ink
    palette.highlight: win.selection
    palette.highlightedText: win.ink
    palette.dark: win.edge
    palette.mid: win.edge
    palette.light: win.hoverSurface
    palette.midlight: win.hoverSurface
    palette.alternateBase: win.panel
    palette.placeholderText: win.muted
    palette.accent: win.accent
    palette.toolTipBase: win.panel
    palette.toolTipText: win.ink
    readonly property bool automationAvailable: typeof automation !== "undefined"
    readonly property var controller: automationAvailable ? automation : backend
    readonly property var preferences: automationAvailable ? automation.preferences : ({})
    FontLoader { id: brandFont; source: "qrc:/fonts/GeistPixel-Square.ttf" }
    readonly property string displayFont: brandFont.status === FontLoader.Ready ? brandFont.name : win.font.family
    property bool darkMode: true
    readonly property color canvas: darkMode ? "#121315" : "#f7f6f3"
    readonly property color panel: darkMode ? "#191a1d" : "#f0efeb"
    readonly property color surface: darkMode ? "#141518" : "#fcfbf8"
    readonly property color ink: darkMode ? "#f2f1ec" : "#272522"
    readonly property color muted: darkMode ? "#a3a3a6" : "#6c675f"
    readonly property color edge: darkMode ? "#2c2d31" : "#dedbd4"
    readonly property color selection: darkMode ? "#2b2018" : "#f7e4d5"
    readonly property color hoverSurface: darkMode ? "#222327" : "#e9e6df"
    readonly property color accent: "#ef5a00"
    readonly property color positive: darkMode ? "#79bd92" : "#32704a"
    readonly property color danger: darkMode ? "#ff927e" : "#a73324"
    readonly property string mono: "Adwaita Mono"
    readonly property int controlRadius: 6
    readonly property int rowHeight: 52
    property var droppedUrls: []
    property string filter: "all"
    property string selectedId: ""
    property string transferId: ""
    readonly property var transferJob: backend.downloads.find(d => d.id === transferId) || null
    property string sortKey: "created"
    property bool descending: true
    property bool categoriesVisible: true
    property bool filesExpanded: false
    property bool queuesExpanded: false
    property bool helpVisible: shortcutsDialog.visible
    property int defaultConnections: 4
    property string defaultDirectory: backend.downloadDirectory
    property string lastError: ""
    property string installMessage: ""
    readonly property string currentError: lastError || backend.error || (automationAvailable ? automation.error : "")
    property var pendingRequest: ({})
    readonly property var authMethods: ["auto", "basic", "digest", "ntlm", "negotiate", "kerberos"]
    readonly property var authLabels: [qsTr("Auto"), "Basic", "Digest", "NTLM", "Negotiate", "Kerberos"]
    property var selected: backend.downloads.find(d => d.id === selectedId) || null
    property var shown: {
        let policies = win.automationAvailable ? automation.jobPolicies : {}
        let rows = backend.downloads.filter(d => {
            if (filter === "all") return true
            if (filter === "unfinished") return d.status !== "complete"
            if (filter === "finished") return d.status === "complete"
            if (filter.startsWith("queue:")) return d.status !== "complete" && ((policies[d.id] || {}).queue || "Main download queue") === filter.slice(6)
            return d.category === filter
        })
        return rows.sort((a,b) => {
            let x = a[sortKey], y = b[sortKey]
            let n = typeof x === "number" ? x-y : String(x || "").localeCompare(String(y || ""))
            return descending ? -n : n
        })
    }
    readonly property bool menusOpen: tasksMenu.visible || downloadsMenu.visible || viewMenu.visible || helpMenu.visible || rowMenu.visible || queueMenu.visible || moreActions.visible
    property bool editing: menusOpen || urlDialog.visible || infoDialog.visible || optionsDialog.visible || removeDialog.visible || shortcutsDialog.visible || aboutDialog.visible || schedulerDialog.visible || listDialog.visible || editDialog.visible || grabberDialog.visible || policyDialog.visible || automationProjects.visible || layoutDialog.visible || updatesDialog.visible
    readonly property bool bridgeAvailable: typeof browserBridge !== "undefined" && browserBridge.available
    readonly property string bridgeError: typeof browserBridge !== "undefined" ? browserBridge.error : ""
    readonly property bool desktopAvailable: typeof desktop !== "undefined"
    property bool fallbackDetailsColumns: false
    readonly property bool showDetailsColumns: desktopAvailable ? ["eta","created","description"].every(k => !desktop.layout.columnHidden.includes(k)) : fallbackDetailsColumns
    readonly property var toolbarCatalog: [
        {key:"resume",label:qsTr("Resume"),icon:"resume",object:"resumeButton"},
        {key:"pause",label:qsTr("Pause"),icon:"stop",object:"pauseButton"},
        {key:"delete",label:qsTr("Delete"),icon:"delete",object:"deleteButton"},
        {key:"details",label:qsTr("Details"),icon:"info",object:"selectionDetailsButton"},
        {key:"startQueue",label:qsTr("Start Queue"),icon:"queue",object:"startQueueButton"},
        {key:"add",label:qsTr("Add download"),icon:"add",object:"addDownloadButton"},
        {key:"stopAll",label:qsTr("Stop All"),icon:"stopall"},
        {key:"stopQueue",label:qsTr("Stop Queue"),icon:"queueStop"},
        {key:"scheduler",label:qsTr("Scheduler"),icon:"queue"},
        {key:"options",label:qsTr("Options"),icon:"options"}
    ]
    function layoutItems(kind) {
        let catalog = kind === "toolbar" ? toolbarCatalog : columnCatalog
        if (!desktopAvailable) return catalog
        return desktop.layout[kind+"Order"].map(k => catalog.find(c => c.key === k)).filter(Boolean)
    }
    function layoutHidden(kind, key) { return desktopAvailable ? desktop.layout[kind+"Hidden"].includes(key) : kind === "toolbar" && ["stopAll","stopQueue","scheduler","options"].includes(key) }
    function changeLayout(kind, key, shown, delta) {
        let order = desktop.layout[kind+"Order"].slice(), hidden = desktop.layout[kind+"Hidden"].slice()
        if (delta) {
            let index = order.indexOf(key), target = index+delta
            if (target < 0 || target >= order.length) return
            order.splice(index,1); order.splice(target,0,key)
        } else hidden = shown ? hidden.filter(k => k !== key) : hidden.concat([key])
        if (!desktop.setLayout(kind,order,hidden)) lastError=desktop.error
    }
    function toggleDetailsColumns(shown) {
        if (!desktopAvailable) { fallbackDetailsColumns=shown; return }
        let extra=["eta","created","description"], hidden=desktop.layout.columnHidden.filter(k => !extra.includes(k))
        if (!shown) hidden=hidden.concat(extra)
        if (!desktop.setLayout("column",desktop.layout.columnOrder,hidden)) lastError=desktop.error
    }
    function toolbarVisible(key) {
        if (layoutHidden("toolbar", key)) return false
        if (["resume","pause","delete","details"].includes(key)) return !!selected
        return key !== "startQueue" || filter.startsWith("queue:")
    }
    function toolbarEnabled(key) {
        if (key === "resume") return !!selected && !running(selected) && selected.status !== "complete"
        if (key === "pause") return running(selected)
        return key !== "scheduler" || automationAvailable
    }
    function toolbarAction(key) {
        if (key === "resume") controller.resume(selectedId)
        else if (key === "pause") controller.pause(selectedId)
        else if (key === "delete") deleteSelected()
        else if (key === "details") openTransfer()
        else if (key === "startQueue") startQueue()
        else if (key === "add") newDownload()
        else if (key === "stopAll") controller.pauseAll()
        else if (key === "stopQueue") stopQueue()
        else if (key === "scheduler") schedulerDialog.open()
        else if (key === "options") optionsDialog.open()
    }
    function presentUrl(url) {
        infoDialog.close(); droppedUrls=[]; pendingRequest=({})
        loginInput.clear(); passwordInput.clear(); useAuthorization.checked=false; mediaInput.checked=false
        urlInput.text=url; newDownload()
    }
    readonly property real primaryTableWidth: Math.max(548, tableScroll.width)
    readonly property var columnCatalog: [
        {key:"name", label:qsTr("File"), width:primaryTableWidth-336},
        {key:"status",label:qsTr("Progress"),width:144},
        {key:"total",label:qsTr("Size"),width:88},
        {key:"speed",label:qsTr("Speed"),width:104},
        {key:"eta",label:qsTr("Time left"),width:88},
        {key:"created",label:qsTr("Date added"),width:160},
        {key:"description",label:qsTr("Description"),width:240}
    ]
    readonly property var columns: desktopAvailable ? layoutItems("column").filter(c => !layoutHidden("column", c.key)) : columnCatalog.filter(c => showDetailsColumns || !["eta","created","description"].includes(c.key))
    function navigationCount(key) {
        if (key === "queues") return ""
        let policies = win.automationAvailable ? automation.jobPolicies : {}
        return String(backend.downloads.filter(d => key === "all" || (key === "unfinished" ? d.status !== "complete" : key === "finished" ? d.status === "complete" : key.startsWith("queue:") ? d.status !== "complete" && ((policies[d.id] || {}).queue || "Main download queue") === key.slice(6) : d.category === key)).length)
    }
    function bytes(value) {
        let language = translator.language
        if (value == null || value < 0) return qsTr("Unknown")
        let units = ["B","KB","MB","GB","TB"], n = Number(value), i = 0
        while (n >= 1024 && i < units.length-1) { n /= 1024; i++ }
        return n.toLocaleString(Qt.locale(), 'f', i ? 1 : 0) + " " + units[i]
    }
    function duration(seconds) {
        if (seconds == null || seconds < 0) return ""
        let s = Math.ceil(seconds), h = Math.floor(s/3600), m = Math.floor(s%3600/60)
        return (h ? h+":"+String(m).padStart(2,"0") : m) + ":" + String(s%60).padStart(2,"0")
    }
    function running(d) { return d && (d.status === "downloading" || d.status === "probing") }
    function sourceHost(d) {
        let match = String(d.url || "").match(/^[a-z]+:\/\/(?:[^@/]*@)?(\[[^\]]+\]|[^/:?#]+)/i)
        return match ? match[1] : d.category || ""
    }
    function statusLabel(d) {
        if (d.status === "downloading" && d.total > 0) return (100*d.received/d.total).toFixed(0)+"%"
        return ({probing:qsTr("Connecting..."),downloading:qsTr("Downloading"),paused:qsTr("Paused"),complete:qsTr("Complete"),error:qsTr("Error")})[d.status] || d.status
    }
    function cell(d,key) {
        if (key === "total") return bytes(d.total)
        if (key === "status") return statusLabel(d)
        if (key === "eta") return running(d) ? duration(d.eta) : ""
        if (key === "speed") return d.speed > 0 ? bytes(d.speed)+"/s" : ""
        if (key === "created") return d.created ? new Date(d.created).toLocaleString(Qt.locale(), Locale.ShortFormat) : ""
        return d[key] || ""
    }
    function startQueue() { if (automationAvailable) automation.startQueue("Main download queue"); else backend.resumeAll() }
    function stopQueue() { if (automationAvailable) automation.stopQueue("Main download queue"); else backend.pauseAll() }
    function loadPreferences() {
        if (!automationAvailable) return
        defaultDirectory = preferences.directory || backend.downloadDirectory
        defaultConnections = preferences.connections || 4
        darkMode = preferences.darkMode === undefined ? true : !!preferences.darkMode
    }
    function loadOptions() {
        loadPreferences()
        defaultsDirectory.text = defaultDirectory
        defaultsConnections.value = defaultConnections
        concurrentInput.value = preferences.maxConcurrent || 4
        retriesInput.value = preferences.retryCount || 0
        retryDelayInput.value = preferences.retryDelaySeconds || 5
        timeoutInput.value = preferences.timeoutSeconds || 30
        rateInput.text = String(preferences.rateLimit || 0)
        proxyInput.text = preferences.proxy || ""
        clipboardCheck.checked = !!preferences.clipboardMonitor
        notificationsCheck.checked = !!preferences.notifications
        soundCheck.checked = !!preferences.sound
        antivirusCheck.checked = !!preferences.antivirus
        darkCheck.checked = darkMode
        typeFilterInput.text = preferences.typeFilter || ""
        completionChoice.currentIndex = ["none","shutdown","disconnect","program"].indexOf(preferences.completionAction || "none")
        completionProgram.text=(preferences.completionCommand || [])[0] || ""
        completionArguments.text=(preferences.completionCommand || []).slice(1).join("\n")
        completionDelayInput.value = preferences.completionDelaySeconds || 30
        let command = preferences.scannerCommand || []
        scannerProgram.text = command.length ? command[0] : ""
        scannerArguments.text = command.slice(1).join("\n")
    }
    function parseHeaders(text) {
        let value
        try { value = JSON.parse(text.trim() || "{}") }
        catch (error) { throw new Error(qsTr("Enter a JSON object with header names and string values.")) }
        if (value === null || typeof value !== "object" || Array.isArray(value))
            throw new Error(qsTr("Enter a JSON object with header names and string values."))
        if (text.length > 65536 || Object.keys(value).length > 64)
            throw new Error(qsTr("Use at most 64 headers and 64 KB of JSON text."))
        for (let name of Object.keys(value))
            if (typeof value[name] !== "string" || /[\x00-\x1f\x7f]/.test(value[name]))
                throw new Error(qsTr("Header values must be strings without control characters."))
        return value
    }
    function siteForUrl(url) {
        return (preferences.siteSettings || {})[automationAvailable ? automation.siteOrigin(url) : ""] || {}
    }
    function loadSiteSettings() {
        let site=automation.siteSettingsForEditing(siteHost.text), custom={}
        siteLogin.text=site.username || ""; sitePassword.text=site.password || ""
        siteAuthMethod.currentIndex=Math.max(0,authMethods.indexOf(site.authMethod || "auto"))
        siteDirectory.text=site.directory || ""; siteConnections.value=site.connections || 4; siteProxy.text=site.proxy || ""
        siteAgent.text=""; siteReferer.text=""
        for (let name of Object.keys(site.headers || {})) {
            if (name.toLowerCase() === "user-agent") siteAgent.text=site.headers[name]
            else if (name.toLowerCase() === "referer") siteReferer.text=site.headers[name]
            else custom[name]=site.headers[name]
        }
        siteHeaders.text=JSON.stringify(custom,null,2); siteStatus.text=""; lastError=""
    }
    function saveSiteSettings() {
        let headers
        try { headers=parseHeaders(siteHeaders.text) }
        catch (error) { lastError=error.message; return }
        for (let name of Object.keys(headers))
            if (["user-agent","referer"].includes(name.toLowerCase())) delete headers[name]
        if (siteAgent.text) headers["User-Agent"]=siteAgent.text
        if (siteReferer.text) headers["Referer"]=siteReferer.text
        let site={connections:siteConnections.value,headers:headers,authMethod:authMethods[siteAuthMethod.currentIndex]}
        if (siteDirectory.text) site.directory=siteDirectory.text
        if (siteProxy.text) site.proxy=siteProxy.text
        if (siteLogin.text) { site.username=siteLogin.text; site.password=sitePassword.text }
        let sites=Object.assign({},preferences.siteSettings || {})
        delete sites[automation.siteOrigin(siteHost.text)]
        sites[siteHost.text.trim()]=site
        if (!automation.configure({siteSettings:sites})) { lastError=automation.error; return }
        siteHost.text=automation.siteOrigin(siteHost.text)
        lastError=""; siteStatus.text=qsTr("Site settings saved.")
    }
    function saveOptions() {
        let command = scannerProgram.text.trim() ? [scannerProgram.text.trim()].concat(scannerArguments.text.split("\n").filter(x => x.length)) : []
        let value = {directory:defaultsDirectory.text,connections:defaultsConnections.value,maxConcurrent:concurrentInput.value,
            retryCount:retriesInput.value,retryDelaySeconds:retryDelayInput.value,timeoutSeconds:timeoutInput.value,
            rateLimit:Number(rateInput.text),proxy:proxyInput.text,clipboardMonitor:clipboardCheck.checked,
            notifications:notificationsCheck.checked,sound:soundCheck.checked,antivirus:antivirusCheck.checked,
            scannerCommand:command,darkMode:darkCheck.checked,typeFilter:typeFilterInput.text,
            completionCommand:completionProgram.text.trim() ? [completionProgram.text.trim()].concat(completionArguments.text.split("\n").filter(x => x.length)) : [],
            completionAction:["none","shutdown","disconnect","program"][completionChoice.currentIndex],completionDelaySeconds:completionDelayInput.value}
        if (automationAvailable && !automation.configure(value)) { lastError = automation.error; return }
        defaultDirectory = value.directory; defaultConnections = value.connections; darkMode = value.darkMode
        optionsDialog.close()
    }
    function scheduleDate(value) {
        if (!value.trim()) return ""
        let date = new Date(value.trim().replace(" ","T"))
        return isNaN(date.getTime()) ? value : date.toISOString()
    }
    function displayScheduleDate(value) { return value ? Qt.formatDateTime(new Date(value),"yyyy-MM-dd HH:mm") : "" }
    function loadQueue() {
        let queue = automationAvailable ? automation.queues.find(q => q.name === queueName.editText) : null
        queueConcurrent.value = queue ? queue.concurrent : 1
        queueStart.text = queue ? displayScheduleDate(queue.startAt) : ""
        queueStop.text = queue ? displayScheduleDate(queue.stopAt) : ""
        queueRepeat.value = queue && queue.repeatMinutes ? queue.repeatMinutes : 0
    }
    function saveQueue(start) {
        if (!automationAvailable) return
        if (!automation.setQueue(queueName.editText,{concurrent:queueConcurrent.value,startAt:scheduleDate(queueStart.text),stopAt:scheduleDate(queueStop.text),repeatMinutes:queueRepeat.value})) { lastError=automation.error; return }
        if (start) automation.startQueue(queueName.editText)
        schedulerDialog.close()
    }
    Component.onCompleted: loadPreferences()
    Connections {
        target: typeof automation !== "undefined" ? automation : null
        function onChanged() { if (!optionsDialog.visible) win.loadPreferences() }
        function onClipboardUrl(url) { urlInput.text=url; win.newDownload() }
    }
    function newDownload() { urlDialog.open() }
    function acceptDroppedUrls(urls) {
        let links = Array.from(urls).map(String).filter(url => /^https?:\/\//i.test(url))
        if (!links.length) { lastError = qsTr("Drop HTTP or HTTPS links here."); return }
        urlInput.text = links.shift()
        droppedUrls = links
        newDownload()
    }
    function prepareInformation() {
        pendingRequest = {url:urlInput.text.trim(), directory:defaultDirectory, connections:defaultConnections, media:mediaInput.checked}
        addressInput.text = pendingRequest.url
        let site=siteForUrl(pendingRequest.url)
        directoryInput.text = site.directory || defaultDirectory
        let path = pendingRequest.url.split(/[?#]/)[0]
        try { filenameInput.text = decodeURIComponent(path.substring(path.lastIndexOf("/")+1)) || "download" } catch(e) { filenameInput.text = "download" }
        descriptionInput.text = ""
        infoMedia.checked = mediaInput.checked
        connections.value = site.connections || defaultConnections
        urlDialog.close()
        infoDialog.open()
    }
    function submit(start) {
        let request = {url:addressInput.text.trim(),directory:directoryInput.text,filename:filenameInput.text,
            connections:connections.value,media:infoMedia.checked,format:["best","1080p","720p","audio"][mediaFormat.currentIndex],description:descriptionInput.text,start:start}
        if (useRequestHeaders.checked) {
            let custom
            try { custom=parseHeaders(requestHeadersInput.text) }
            catch (error) { lastError=error.message; return }
            let headers={}, inherited=siteForUrl(request.url).headers || {}
            for (let name of Object.keys(inherited)) headers[name.toLowerCase()]=inherited[name]
            for (let name of Object.keys(custom)) headers[name.toLowerCase()]=custom[name]
            request.headers=headers
        }
        if (useAuthorization.checked) { request.username = loginInput.text; request.password = passwordInput.text; request.authMethod=authMethods[authMethodChoice.currentIndex] }
        let id = win.automationAvailable ? automation.enqueue(request) : backend.addRequest(request)
        if (!id) { lastError = win.automationAvailable ? automation.error : backend.error; return }
        selectedId = id; lastError = ""; urlInput.clear(); loginInput.clear(); passwordInput.clear(); useAuthorization.checked=false; authMethodChoice.currentIndex=0; infoDialog.close()
        if (droppedUrls.length) {
            let remaining = droppedUrls.slice()
            urlInput.text = remaining.shift()
            droppedUrls = remaining
            urlDialog.open()
        } else if (start) win.openTransfer()
    }
    function openTransfer() { transferId = selectedId; transferDialog.open() }
    function toggleSelected() { if (selected) running(selected) ? win.controller.pause(selected.id) : win.controller.resume(selected.id) }
    function deleteSelected() { if (selected) removeDialog.open() }
    function moveSelection(delta) {
        if (!shown.length) return
        let index = shown.findIndex(d => d.id === selectedId)
        selectedId = shown[Math.max(0,Math.min(shown.length-1,index+delta))].id
        rows.positionViewAtIndex(Math.max(0,index+delta), ListView.Contain)
    }
    onClosing: function(event) { event.accepted = false; win.hide() }
    Shortcut { sequence: "Ctrl+N"; onActivated: win.newDownload() }
    Shortcut { sequence: "Ctrl+Q"; onActivated: { win.controller.pauseAll(); Qt.quit() } }
    Shortcut { sequence: "Delete"; enabled: !win.editing && !transferDialog.visible; onActivated: win.deleteSelected() }
    Shortcut { sequence: "Space"; enabled: !win.editing && !transferDialog.visible; onActivated: win.toggleSelected() }
    Shortcut { sequence: "Return"; enabled: !win.editing && !!win.selected && !transferDialog.visible; onActivated: win.openTransfer() }
    Shortcut { sequence: "Up"; enabled: !win.editing && !transferDialog.visible; onActivated: win.moveSelection(-1) }
    Shortcut { sequence: "Down"; enabled: !win.editing && !transferDialog.visible; onActivated: win.moveSelection(1) }
    Shortcut { sequence: "F1"; onActivated: shortcutsDialog.open() }
    Shortcut {
        sequence: "Menu"; enabled: !win.editing && !!win.selected
        onActivated: {
            let item = rows.itemAtIndex(win.shown.findIndex(d => d.id === win.selectedId))
            let point = (item || rows).mapToItem(win.contentItem, 20, item ? item.height : 0)
            rowMenu.popup(win.contentItem, point.x, point.y)
        }
    }

    component AppIcon: Image {
        property string kind: "add"
        property color tint: win.ink
        sourceSize.width: 24; sourceSize.height: 24
        width: 20; height: 20; fillMode: Image.PreserveAspectFit
        source: win.iconSource(kind, tint)
    }
    function iconSource(kind, tint) {
            let paths = {
                add:'<path d="M12 5v14M5 12h14"/>',
                resume:'<path d="m8 5 11 7-11 7z"/>',
                stop:'<path d="M8 5v14M16 5v14"/>',
                stopall:'<path d="M5 5h14v14H5z"/>',
                delete:'<path d="M4 6h16M9 6V3h6v3M6 6l1 15h10l1-15M10 10v7M14 10v7"/>',
                completed:'<path d="M4 6h16M6 6l1 15h10l1-15M9 3h6M9 13l2 2 4-4"/>',
                options:'<path d="M4 6h16M4 12h16M4 18h16M8 3v6M16 9v6M10 15v6"/>',
                queue:'<path d="M3 5h11M3 11h8M3 17h8m5-7 6 4-6 4z"/>',
                queueStop:'<path d="M3 5h11M3 11h8M3 17h8m5-7h6v8h-6z"/>',
                folder:'<path d="M3 6h7l2 2h9v12H3z"/>',
                file:'<path d="M6 3h8l4 4v14H6zM14 3v5h4M9 12h6M9 16h6"/>',
                video:'<path d="M3 5h18v14H3zm7 4 5 3-5 3z"/>',
                music:'<path d="M10 18V5l10-2v13M10 8l10-2"/><ellipse cx="6.5" cy="18" rx="3.5" ry="2.5"/><ellipse cx="16.5" cy="16" rx="3.5" ry="2.5"/>',
                download:'<path d="M12 3v12m-5-5 5 5 5-5M4 17v4h16v-4"/>',
                more:'<circle cx="5" cy="12" r="1"/><circle cx="12" cy="12" r="1"/><circle cx="19" cy="12" r="1"/>',
                info:'<circle cx="12" cy="12" r="9"/><path d="M12 11v6M12 7h.01"/>',
                chevron:'<path d="m9 6 6 6-6 6"/>'
            }
            return "data:image/svg+xml;utf8,"+encodeURIComponent('<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="'+tint+'" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round">'+(paths[kind]||paths.file)+'</svg>')
        }

    component AppButton: Button {
        id: button
        property bool primary: false
        implicitHeight: 36; implicitWidth: Math.max(82,contentItem.implicitWidth+28)
        padding: 7; font.weight: Font.Medium
        background: Rectangle {
            color: button.primary ? (button.down ? "#e56219" : button.hovered ? "#ff8147" : win.accent) : button.down ? win.selection : button.hovered ? win.hoverSurface : win.panel
            border.width: button.activeFocus ? 2 : 1
            border.color: button.activeFocus ? theme.accent : button.primary ? win.accent : win.edge
            radius: win.controlRadius; opacity: button.enabled ? 1 : .4
            Behavior on color { ColorAnimation { duration: 110 } }
        }
        contentItem: Text { text: button.text; font: button.font; color: button.enabled ? (button.primary ? "#1a1008" : win.ink) : win.muted; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
    }
    component AppField: TextField {
        implicitHeight: 34; padding: 9; selectByMouse: true; color: win.ink
        placeholderTextColor: win.muted; selectionColor: win.selection; selectedTextColor: win.ink
        background: Rectangle { color: win.surface; radius: win.controlRadius; border.color: parent.activeFocus ? theme.accent : win.edge; border.width: parent.activeFocus ? 2 : 1 }
    }
    component AppTextArea: TextArea {
        id: area
        color: win.ink; placeholderTextColor: win.muted; selectByMouse: true; padding: 10
        selectionColor: win.selection; selectedTextColor: win.ink
        background: Rectangle { color: win.surface; radius: win.controlRadius; border.color: area.activeFocus ? theme.accent : win.edge; border.width: area.activeFocus ? 2 : 1 }
    }
    component AppTabButton: TabButton {
        id: tab
        implicitHeight: 36; padding: 8
        background: Rectangle {
            color: tab.hovered ? win.hoverSurface : "transparent"
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 2; color: tab.checked ? win.accent : win.edge }
        }
        contentItem: Text { text: tab.text; color: tab.checked ? win.ink : win.muted; font.pixelSize: 12; font.weight: tab.checked ? Font.Medium : Font.Normal; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter; elide: Text.ElideRight }
    }
    component AppCheckBox: CheckBox {
        id: check
        implicitHeight: Math.max(32, contentItem.implicitHeight+6); spacing: 10; padding: 3; hoverEnabled: true
        indicator: Rectangle {
            x: check.leftPadding; y: (check.height-height)/2; width: 17; height: 17
            color: check.checked ? win.accent : win.surface; radius: win.controlRadius
            border.color: check.activeFocus ? theme.accent : check.checked ? win.accent : win.edge
            border.width: check.activeFocus ? 2 : 1
            Text { anchors.centerIn: parent; text: "✓"; color: "#fffaf5"; font.pixelSize: 12; font.bold: true; visible: check.checked }
        }
        contentItem: Text { text: check.text; color: check.enabled ? win.ink : win.muted; font: check.font; lineHeight: 1.35; leftPadding: check.indicator.width+check.spacing; verticalAlignment: Text.AlignVCenter; wrapMode: Text.WordWrap }
    }
    component NativeDialog: Dialog {
        id: dialog
        padding: 20; topPadding: 16
        background: Rectangle { color: win.panel; border.color: win.edge; radius: 10 }
        header: Item {
            implicitHeight: 58
            RowLayout {
                anchors.fill: parent; anchors.leftMargin: 20; anchors.rightMargin: 14; spacing: 10
                Label { text: dialog.title; color: win.ink; font.pixelSize: 15; font.weight: Font.DemiBold; elide: Text.ElideRight; Layout.fillWidth: true }
                AppButton { text: "×"; Accessible.name: qsTr("Close dialog"); implicitWidth: 30; implicitHeight: 30; onClicked: dialog.reject() }
            }
            Rectangle { anchors.bottom: parent.bottom; anchors.left: parent.left; anchors.right: parent.right; anchors.leftMargin: 20; anchors.rightMargin: 20; height: 1; color: win.edge }
        }
        footer: DialogButtonBox {
            visible: count > 0; standardButtons: dialog.standardButtons
            leftPadding: 20; rightPadding: 20; bottomPadding: 16; topPadding: 8; spacing: 8
            background: Item {}
            delegate: AppButton {}
            onAccepted: dialog.accept()
            onRejected: dialog.reject()
        }
        Overlay.modal: Rectangle { color: win.darkMode ? "#99000000" : "#55000000" }
    }
    component Tool: Button {
        id: tool
        property string kind: "add"
        property bool showLabel: false
        property bool prominent: false
        implicitWidth: prominent ? Math.max(132,contentItem.implicitWidth+28) : showLabel ? contentItem.implicitWidth+20 : 36; implicitHeight: 36
        Accessible.name: text
        padding: 6; hoverEnabled: true
        background: Rectangle { color: tool.prominent ? win.accent : tool.down ? win.selection : tool.hovered ? win.hoverSurface : "transparent"; border.color: tool.activeFocus ? theme.accent : "transparent"; radius: win.controlRadius }
        contentItem: RowLayout {
            spacing: 7; opacity: tool.enabled ? 1 : .3
            AppIcon { visible: !tool.prominent; kind: tool.kind; tint: win.muted; Layout.preferredWidth: 17; Layout.preferredHeight: 17; Layout.alignment: Qt.AlignVCenter }
            Text { visible: tool.showLabel || tool.prominent; text: tool.text; font.pixelSize: 12; font.weight: Font.Medium; color: tool.prominent ? "#1a1008" : win.ink; Layout.alignment: Qt.AlignVCenter }
        }
        ToolTip.visible: hovered
        ToolTip.delay: 500
        ToolTip.text: text
    }
    component ToolbarTool: Tool {
        property string actionKey
        readonly property var actionInfo: win.toolbarCatalog.find(c => c.key === actionKey)
        objectName: actionInfo.object || "toolbar-"+actionKey
        text: actionInfo.label; kind: actionInfo.icon; prominent: actionKey === "add"
        visible: win.toolbarVisible(actionKey); enabled: win.toolbarEnabled(actionKey)
        onClicked: win.toolbarAction(actionKey)
    }
    menuBar: MenuBar {
        objectName: "mainMenuBar"
        leftPadding: 16; rightPadding: 16; topPadding: 3; bottomPadding: 3
        spacing: 2
        background: Rectangle {
            color: win.panel
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: win.edge }
        }
        delegate: MenuBarItem {
            id: menuItem
            implicitWidth: menuLabel.implicitWidth + 24; implicitHeight: 30
            contentItem: Text {
                id: menuLabel
                text: menuItem.text; font.pixelSize: 12; color: win.ink
                horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
            }
            background: Rectangle {
                radius: win.controlRadius
                color: menuItem.highlighted ? win.hoverSurface : "transparent"
                border.color: menuItem.activeFocus ? theme.accent : "transparent"
            }
        }
        IdmMenu { appWindow: win; id: tasksMenu; objectName: "tasksMenu"; title: qsTr("Tasks")
            IdmMenuItem { appWindow: win; shortcutHint: "Ctrl+N"; text: qsTr("Add URL"); icon.source: win.iconSource("add", win.muted); onTriggered: win.newDownload() }
            IdmMenuSeparator { appWindow: win; }
            Action { text: qsTr("Import downloads"); icon.source: win.iconSource("download", win.muted); enabled: win.automationAvailable; onTriggered: { listDialog.importing=true; listDialog.open() } }
            Action { text: qsTr("Export downloads"); icon.source: win.iconSource("file", win.muted); enabled: win.automationAvailable; onTriggered: { listDialog.importing=false; listDialog.open() } }
            Action { objectName: "automationProjectsAction"; text: qsTr("Synchronization and quotas"); enabled: win.automationAvailable; onTriggered: automationProjects.open() }
            Action { text: qsTr("Site Grabber"); icon.source: win.iconSource("folder", win.muted); enabled: win.automationAvailable; onTriggered: grabberDialog.open() }
            IdmMenuSeparator { appWindow: win; }
            IdmMenuItem { appWindow: win; shortcutHint: "Ctrl+Q"; text: qsTr("Exit"); icon.source: win.iconSource("stopall", win.muted); onTriggered: { win.controller.pauseAll(); Qt.quit() } }
        }
        IdmMenu { appWindow: win; id: downloadsMenu; title: qsTr("Downloads")
            Action { text: qsTr("Resume"); icon.source: win.iconSource("resume", win.muted); enabled: !!win.selected && !win.running(win.selected) && win.selected.status !== "complete"; onTriggered: win.controller.resume(win.selectedId) }
            Action { text: qsTr("Stop"); icon.source: win.iconSource("stop", win.muted); enabled: win.running(win.selected); onTriggered: win.controller.pause(win.selectedId) }
            Action { text: qsTr("Stop All"); icon.source: win.iconSource("stopall", win.muted); onTriggered: win.controller.pauseAll() }
            IdmMenuSeparator { appWindow: win; }
            Action { text: qsTr("Delete"); icon.source: win.iconSource("delete", win.muted); enabled: !!win.selected; onTriggered: win.deleteSelected() }
            Action { text: qsTr("Delete Completed"); icon.source: win.iconSource("completed", win.muted); onTriggered: win.controller.removeCompleted() }
            IdmMenuSeparator { appWindow: win; }
            Action { text: qsTr("Start Queue"); icon.source: win.iconSource("queue", win.muted); onTriggered: win.startQueue() }
            Action { text: qsTr("Stop Queue"); icon.source: win.iconSource("queueStop", win.muted); onTriggered: win.stopQueue() }
            Action { text: qsTr("Scheduler"); icon.source: win.iconSource("queue", win.muted); enabled: win.automationAvailable; onTriggered: schedulerDialog.open() }
            Action { text: qsTr("Options"); icon.source: win.iconSource("options", win.muted); onTriggered: optionsDialog.open() }
        }
        IdmMenu { appWindow: win; id: viewMenu; title: qsTr("View")
            Action { objectName: "advancedColumnsAction"; text: qsTr("More columns"); checkable: true; checked: win.showDetailsColumns; onTriggered: win.toggleDetailsColumns(checked) }
            Action { objectName: "customizeLayoutAction"; text: qsTr("Customize toolbar and columns"); enabled: win.desktopAvailable; onTriggered: layoutDialog.open() }
            Action { text: qsTr("Categories"); icon.source: win.iconSource("folder", win.muted); checkable: true; checked: win.categoriesVisible; onTriggered: win.categoriesVisible = checked }
            Action { text: qsTr("Download progress"); icon.source: win.iconSource("download", win.muted); enabled: !!win.selected; onTriggered: win.openTransfer() }
        }
        IdmMenu { appWindow: win; id: helpMenu; title: qsTr("Help")
            IdmMenuItem { appWindow: win; shortcutHint: "F1"; text: qsTr("Keyboard shortcuts"); icon.source: win.iconSource("file", win.muted); onTriggered: shortcutsDialog.open() }
            Action { text: qsTr("Browser integration"); icon.source: win.iconSource("download", win.muted); onTriggered: { optionsTabs.currentIndex = 6; optionsDialog.open() } }
            Action { objectName: "checkUpdatesAction"; text: qsTr("Check for updates"); enabled: win.desktopAvailable; onTriggered: { updatesDialog.open(); desktop.checkForUpdates() } }
            Action { text: qsTr("About"); icon.source: win.iconSource("file", win.muted); onTriggered: aboutDialog.open() }
        }
    }
    RowLayout {
        LayoutMirroring.enabled: Qt.application.layoutDirection === Qt.RightToLeft
        LayoutMirroring.childrenInherit: true
        anchors.fill: parent; spacing: 0
        Rectangle {
            visible: win.categoriesVisible
            Layout.preferredWidth: 204; Layout.fillHeight: true
            color: win.panel
            Rectangle { anchors.right: parent.right; width: 1; height: parent.height; color: win.edge }
            ColumnLayout {
                anchors.fill: parent; anchors.margins: 16; spacing: 28
                RowLayout {
                    Layout.fillWidth: true; Layout.topMargin: 4; spacing: 9
                    Rectangle {
                        width: 30; height: 30; color: win.accent; radius: win.controlRadius
                        AppIcon { kind: "download"; tint: "#1a1008"; anchors.centerIn: parent; width: 20; height: 20 }
                    }
                    Column {
                        spacing: 1
                        Text { text: "IDM Native"; font.family: win.displayFont; font.pixelSize: 17; font.weight: Font.Medium; font.letterSpacing: -.4; color: win.ink }
                    }
                }
                ScrollView {
                    id: navigationScroll
                    Layout.fillWidth: true; Layout.fillHeight: true; clip: true; contentWidth: availableWidth
                    Column {
                        width: navigationScroll.availableWidth; spacing: 2
                        Repeater {
                            model: [{key:"all",label:qsTr("All Downloads"),depth:0,icon:"download"},
                                {key:"unfinished",label:qsTr("Unfinished"),depth:0,icon:"stop"},{key:"finished",label:qsTr("Finished"),depth:0,icon:"completed"},
                                {key:"types",label:qsTr("File types"),depth:0,icon:"chevron",section:true},
                                {key:"Compressed",label:qsTr("Compressed"),depth:1,icon:"folder"},{key:"Documents",label:qsTr("Documents"),depth:1,icon:"file"},
                                {key:"Music",label:qsTr("Music"),depth:1,icon:"music"},{key:"Programs",label:qsTr("Programs"),depth:1,icon:"file"},{key:"Video",label:qsTr("Video"),depth:1,icon:"video"},{key:"Other",label:qsTr("Other"),depth:1,icon:"folder"},
                                {key:"queues",label:qsTr("Queues"),depth:0,icon:"queue"}].concat((win.automationAvailable ? automation.queues : [{name:"Main download queue"}]).map(q => ({key:"queue:"+q.name,label:q.name === "Main download queue" ? qsTr("Main download queue") : q.name,depth:1,icon:"queue"})))
                            delegate: Rectangle {
                                id: navItem
                                objectName: "navigation-" + modelData.key
                                required property var modelData
                                readonly property bool selected: win.filter === modelData.key
                                readonly property bool section: !!modelData.section
                                function activate() {
                                    if (section) win.filesExpanded = !win.filesExpanded
                                    else if (modelData.key === "queues") win.queuesExpanded = !win.queuesExpanded
                                    else win.filter = modelData.key
                                }
                                activeFocusOnTab: true
                                Accessible.role: Accessible.Button
                                Accessible.name: modelData.label
                                Accessible.onPressAction: activate()
                                Keys.onShortcutOverride: event => { if ([Qt.Key_Return, Qt.Key_Enter, Qt.Key_Space].includes(event.key)) event.accepted = true }
                                Keys.onReturnPressed: activate()
                                Keys.onEnterPressed: activate()
                                Keys.onSpacePressed: activate()
                                visible: modelData.depth === 0 || (modelData.key.startsWith("queue:") ? win.queuesExpanded : win.filesExpanded)
                                width: parent.width; height: visible ? 38 : 0
                                color: selected ? win.selection : navMouse.containsMouse ? win.hoverSurface : "transparent"
                                radius: win.controlRadius
                                border.color: activeFocus ? theme.accent : "transparent"
                                Rectangle { visible: navItem.selected; width: 2; height: 17; anchors.left: parent.left; anchors.verticalCenter: parent.verticalCenter; color: win.accent }
                                RowLayout {
                                    anchors.fill: parent; anchors.leftMargin: 10; anchors.rightMargin: 10; spacing: 9
                                    AppIcon { visible: !navItem.section; kind: navItem.modelData.icon; Layout.preferredWidth: 15; Layout.preferredHeight: 15; tint: navItem.selected ? win.accent : win.muted }
                                    Text { text: navItem.modelData.label; font.pixelSize: 12; font.weight: navItem.selected || navItem.section ? Font.Medium : Font.Normal; color: navItem.selected ? win.ink : win.muted; elide: Text.ElideRight; Layout.fillWidth: true }
                                    Text { text: navItem.section ? "" : win.navigationCount(navItem.modelData.key); font.family: win.mono; font.pixelSize: 10; color: navItem.selected ? win.ink : win.muted; visible: text !== "" && text !== "0" }
                                    AppIcon { visible: navItem.section || navItem.modelData.key === "queues"; kind: "chevron"; Layout.preferredWidth: 11; Layout.preferredHeight: 11; tint: win.muted; rotation: (navItem.section ? win.filesExpanded : win.queuesExpanded) ? 90 : 0 }
                                }
                                ToolTip.visible: navMouse.containsMouse && navItem.modelData.key.startsWith("queue:")
                                ToolTip.text: navItem.modelData.label
                                MouseArea {
                                    id: navMouse
                                    anchors.fill: parent; acceptedButtons: Qt.LeftButton | Qt.RightButton; hoverEnabled: true
                                    onClicked: mouse => {
                                        if (mouse.button === Qt.LeftButton) navItem.activate()
                                        if (mouse.button === Qt.RightButton && navItem.modelData.key.startsWith("queue:")) {
                                            queueMenu.queue = navItem.modelData.key.slice(6)
                                            let point = navItem.mapToItem(win.contentItem, mouse.x, mouse.y)
                                            queueMenu.popup(win.contentItem, point.x, point.y)
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
                Rectangle { Layout.fillWidth: true; height: 1; color: win.edge }
                Item {
                    Layout.fillWidth: true; Layout.preferredHeight: 24; Layout.bottomMargin: 4
                    RowLayout {
                        anchors.fill: parent; spacing: 8
                        Rectangle { width: 5; height: 5; radius: 2.5; color: win.bridgeAvailable ? win.positive : win.muted }
                        Label { text: win.bridgeAvailable ? qsTr("Browser connected") : qsTr("Browser setup"); font.pixelSize: 11; color: win.muted; elide: Text.ElideRight; Layout.fillWidth: true }
                        AppIcon { kind: "chevron"; tint: win.muted; Layout.preferredWidth: 12; Layout.preferredHeight: 12 }
                    }
                    MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: { optionsTabs.currentIndex=6; optionsDialog.open() } }
                }
            }
        }
        ColumnLayout {
            Layout.fillWidth: true; Layout.fillHeight: true; spacing: 0
            Item {
                Layout.fillWidth: true; Layout.preferredHeight: 88
                RowLayout {
                    objectName: "downloadToolbar"
                    anchors.fill: parent; anchors.leftMargin: 24; anchors.rightMargin: 24; spacing: 8
                    property ColumnLayout heading: ColumnLayout {
                        Layout.fillWidth: true; spacing: 5
                        Label {
                            text: win.filter === "all" ? qsTr("Downloads") : win.filter.startsWith("queue:") ? win.filter.slice(6) : win.filter === "unfinished" ? qsTr("Unfinished") : win.filter === "finished" ? qsTr("Finished") : qsTr(win.filter)
                            font.pixelSize: 24; font.weight: Font.DemiBold; font.letterSpacing: -.4; color: win.ink
                            Layout.fillWidth: true; elide: Text.ElideRight
                        }
                        Label {
                            readonly property int activeCount: backend.downloads.filter(d => win.running(d)).length
                            visible: activeCount > 0
                            text: qsTr("%1 active").arg(activeCount); font.pixelSize: 12; color: win.muted
                        }
                    }
                    property ToolbarTool resumeTool: ToolbarTool { actionKey: "resume" }
                    property ToolbarTool pauseTool: ToolbarTool { actionKey: "pause" }
                    property ToolbarTool deleteTool: ToolbarTool { actionKey: "delete" }
                    property ToolbarTool detailsTool: ToolbarTool { actionKey: "details" }
                    property ToolbarTool startQueueTool: ToolbarTool { actionKey: "startQueue" }
                    property ToolbarTool addTool: ToolbarTool { actionKey: "add" }
                    property ToolbarTool stopAllTool: ToolbarTool { actionKey: "stopAll" }
                    property ToolbarTool stopQueueTool: ToolbarTool { actionKey: "stopQueue" }
                    property ToolbarTool schedulerTool: ToolbarTool { actionKey: "scheduler" }
                    property ToolbarTool optionsTool: ToolbarTool { actionKey: "options" }
                    readonly property var buttons: ({resume:resumeTool,pause:pauseTool,delete:deleteTool,details:detailsTool,startQueue:startQueueTool,add:addTool,stopAll:stopAllTool,stopQueue:stopQueueTool,scheduler:schedulerTool,options:optionsTool})
                    children: [heading].concat(win.layoutItems("toolbar").map(c => buttons[c.key])).concat([moreButton])
                    property Tool moreTool: Tool {
                        id: moreButton; objectName: "moreActionsButton"
                        text: qsTr("More"); kind: "more"
                        onClicked: moreActions.popup(moreButton, 0, moreButton.height)
                        IdmMenu { appWindow: win;
                            id: moreActions; objectName: "moreActionsMenu"
                            IdmMenuItem { appWindow: win; text: qsTr("Preferences"); icon.source: win.iconSource("options", win.muted); onTriggered: optionsDialog.open() }
                            IdmMenuItem { appWindow: win; text: qsTr("Stop All"); icon.source: win.iconSource("stopall", win.muted); onTriggered: win.controller.pauseAll() }
                            IdmMenuItem { appWindow: win; objectName: "deleteCompletedButton"; text: qsTr("Delete Completed"); icon.source: win.iconSource("completed", win.muted); onTriggered: win.controller.removeCompleted() }
                            IdmMenuSeparator { appWindow: win; }
                            IdmMenuItem { appWindow: win; text: qsTr("Start Queue"); icon.source: win.iconSource("queue", win.muted); onTriggered: win.startQueue() }
                            IdmMenuItem { appWindow: win; text: qsTr("Stop Queue"); icon.source: win.iconSource("queueStop", win.muted); onTriggered: win.stopQueue() }
                            IdmMenuItem { appWindow: win; text: qsTr("Scheduler"); icon.source: win.iconSource("queue", win.muted); enabled: win.automationAvailable; onTriggered: schedulerDialog.open() }
                        }
                    }
                }
            }
            Item {
                Layout.fillWidth: true; Layout.fillHeight: true
                Flickable {
                    id: tableScroll; objectName: "tableScroll"
                    anchors.fill: parent; anchors.leftMargin: 14; anchors.rightMargin: 14
                    clip: true; contentWidth: Math.max(width,win.columns.reduce((n,c) => n+c.width,0)); contentHeight: height; flickableDirection: Flickable.HorizontalFlick
                    ScrollBar.horizontal: IdmScrollBar { appWindow: win; policy: ScrollBar.AsNeeded }
                    Column {
                        width: tableScroll.contentWidth; height: tableScroll.height
                        Row {
                            height: 34
                            Repeater { model: win.columns
                                delegate: Rectangle {
                                    required property var modelData
                                    objectName: "tableHeader-"+modelData.key
                                    width: modelData.width; height: 34; color: headerMouse.containsMouse ? win.hoverSurface : win.canvas
                                    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: win.edge }
                                    Text { color: win.muted; anchors.fill: parent; anchors.leftMargin: modelData.key === "name" ? 46 : 12; anchors.rightMargin: 12; verticalAlignment: Text.AlignVCenter; horizontalAlignment: ["total","eta","speed"].includes(modelData.key) ? Text.AlignRight : Text.AlignLeft; font.pixelSize: 11; font.weight: Font.Medium; text: modelData.label + (win.sortKey === modelData.key ? (win.descending ? "  ↓" : "  ↑") : "") }
                                    MouseArea { id: headerMouse; anchors.fill: parent; hoverEnabled: true; onClicked: { win.descending = win.sortKey === modelData.key ? !win.descending : false; win.sortKey = modelData.key } }
                                }
                            }
                        }
                        ListView {
                            id: rows; objectName: "downloadTable"
                            width: parent.width; height: Math.max(0,parent.height-46); clip: true
                            model: win.shown
                            ScrollBar.vertical: IdmScrollBar { appWindow: win;
                                parent: tableScroll.parent
                                anchors.right: parent.right; anchors.top: parent.top; anchors.topMargin: 34
                                anchors.bottom: parent.bottom; anchors.bottomMargin: 14
                                policy: ScrollBar.AsNeeded
                            }
                            delegate: Rectangle {
                                id: row
                                required property var modelData
                                required property int index
                                width: rows.width; height: win.rowHeight
                                color: win.selectedId === modelData.id ? win.selection : rowMouse.containsMouse ? win.hoverSurface : win.canvas
                                Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: win.edge; opacity: .45 }
                                Rectangle { visible: win.selectedId === row.modelData.id; width: 2; height: parent.height; color: win.accent }
                                Row {
                                    Repeater { model: win.columns
                                        delegate: Item {
                                            required property var modelData
                                            readonly property bool numeric: ["total","eta","speed"].includes(modelData.key)
                                            width: modelData.width; height: win.rowHeight
                                            AppIcon {
                                                visible: modelData.key === "name"; x: 14; width: 18; height: 18; anchors.verticalCenter: parent.verticalCenter
                                                kind: row.modelData.category === "Video" ? "video" : row.modelData.category === "Music" ? "music" : "file"
                                                tint: win.selectedId === row.modelData.id ? win.accent : win.muted
                                            }
                                            Text {
                                                id: cellLabel
                                                x: modelData.key === "name" ? 46 : 12; width: parent.width-x-12; height: parent.height
                                                text: win.cell(row.modelData,modelData.key); elide: Text.ElideRight; verticalAlignment: Text.AlignVCenter
                                                horizontalAlignment: parent.numeric ? Text.AlignRight : Text.AlignLeft
                                                font.family: parent.numeric ? win.mono : win.font.family
                                                font.pixelSize: parent.numeric || modelData.key === "created" ? 12 : 13
                                                font.weight: modelData.key === "name" ? Font.Medium : Font.Normal
                                                color: row.modelData.status === "error" ? win.danger : modelData.key === "status" && row.modelData.status === "complete" ? win.positive : modelData.key === "status" && win.running(row.modelData) ? win.accent : modelData.key === "name" ? win.ink : win.muted
                                                ToolTip {
                                                    objectName: "downloadCellTip"
                                                    visible: rowMouse.containsMouse && rowMouse.mouseX >= cellLabel.parent.x && rowMouse.mouseX < cellLabel.parent.x+cellLabel.parent.width && cellLabel.truncated
                                                    delay: 700; text: cellLabel.text; width: Math.min(600,win.width-24)
                                                }
                                            }
                                            Rectangle {
                                                visible: modelData.key === "status" && win.running(row.modelData) && row.modelData.total > 0
                                                x: 12; y: parent.height-13; width: parent.width-24; height: 3; radius: 1.5; color: win.edge
                                                Rectangle { width: parent.width*Math.min(1,row.modelData.received/Math.max(1,row.modelData.total)); height: parent.height; radius: 1.5; color: win.accent }
                                            }
                                        }
                                    }
                                }
                                MouseArea {
                                    id: rowMouse; objectName: "rowMouse-"+row.modelData.id; hoverEnabled: true
                                    property point pressPoint
                                    property bool dragStarted: false
                                    onPressed: mouse => { pressPoint=Qt.point(mouse.x,mouse.y); dragStarted=false; win.selectedId=row.modelData.id }
                                    onPositionChanged: mouse => {
                                        if (pressed && (pressedButtons & Qt.LeftButton) && !dragStarted && win.desktopAvailable && row.modelData.status === "complete" && Math.hypot(mouse.x-pressPoint.x,mouse.y-pressPoint.y) >= desktop.dragDistance) {
                                            dragStarted=true
                                            desktop.dragCompleted(row.modelData,win)
                                        }
                                    }
                                    anchors.fill: parent; acceptedButtons: Qt.LeftButton | Qt.RightButton
                                    onClicked: mouse => {
                                        win.selectedId = row.modelData.id
                                        if (mouse.button === Qt.RightButton) {
                                            let point = row.mapToItem(win.contentItem, mouse.x, mouse.y)
                                            rowMenu.popup(win.contentItem, point.x, point.y)
                                        }
                                    }
                                    onDoubleClicked: { win.selectedId = row.modelData.id; win.openTransfer() }
                                }
                            }
                        }
                    }
                }
                Column {
                    objectName: "emptyState"
                    visible: win.shown.length === 0
                    width: Math.min(320,parent.width-48); anchors.centerIn: parent; spacing: 14
                    Rectangle {
                        width: 42; height: 42; color: "transparent"; radius: win.controlRadius
                        anchors.horizontalCenter: parent.horizontalCenter
                        AppIcon { kind: "download"; tint: win.muted; width: 24; height: 24; anchors.centerIn: parent }
                    }
                    Text { width: parent.width; horizontalAlignment: Text.AlignHCenter; text: backend.downloads.length ? qsTr("Nothing in this view") : qsTr("No downloads yet"); font.pixelSize: 18; font.weight: Font.DemiBold; font.letterSpacing: -.4; color: win.ink; wrapMode: Text.WordWrap }
                    Text { width: parent.width; horizontalAlignment: Text.AlignHCenter; text: qsTr("Add a link or send one from your browser."); font.pixelSize: 12; color: win.muted; wrapMode: Text.WordWrap; lineHeight: 1.4 }

                }
            }
            Rectangle {
                visible: !!win.currentError
                Layout.fillWidth: true; Layout.preferredHeight: 30; color: win.canvas
                Rectangle { anchors.top: parent.top; width: parent.width; height: 1; color: win.edge }
                RowLayout {
                    anchors.fill: parent; anchors.leftMargin: 26; anchors.rightMargin: 26; spacing: 10
                    Label { text: win.currentError; elide: Text.ElideRight; Layout.fillWidth: true; color: win.currentError ? win.danger : win.muted; font.pixelSize: 11 }
                }
            }
        }
    }
    DropArea {
        objectName: "urlDropArea"
        anchors.fill: parent
        enabled: !win.editing
        onDropped: drop => {
            if (drop.hasUrls) win.acceptDroppedUrls(drop.urls)
            else if (drop.hasText) win.acceptDroppedUrls(drop.text.split(/\s+/))
        }
    }
    IdmMenu { appWindow: win;
        id: queueMenu; objectName: "queueContextMenu"
        property string queue: "Main download queue"
        Action { text: qsTr("Start Queue"); icon.source: win.iconSource("queue", win.muted); enabled: win.automationAvailable; onTriggered: automation.startQueue(queueMenu.queue) }
        Action { text: qsTr("Stop Queue"); icon.source: win.iconSource("queueStop", win.muted); enabled: win.automationAvailable; onTriggered: automation.stopQueue(queueMenu.queue) }
        Action { text: qsTr("Scheduler"); icon.source: win.iconSource("queue", win.muted); enabled: win.automationAvailable; onTriggered: { queueName.editText=queueMenu.queue; schedulerDialog.open(); win.loadQueue() } }
    }
    IdmMenu { appWindow: win;
        id: rowMenu; objectName: "rowContextMenu"
        Action { text: qsTr("Download progress"); icon.source: win.iconSource("download", win.muted); onTriggered: win.openTransfer() }
        Action { text: qsTr("Resume"); icon.source: win.iconSource("resume", win.muted); enabled: !!win.selected && !win.running(win.selected) && win.selected.status !== "complete"; onTriggered: win.controller.resume(win.selectedId) }
        Action { text: qsTr("Stop"); icon.source: win.iconSource("stop", win.muted); enabled: win.running(win.selected); onTriggered: win.controller.pause(win.selectedId) }
        IdmMenuSeparator { appWindow: win; }
        Action { text: qsTr("Open containing folder"); icon.source: win.iconSource("folder", win.muted); enabled: !!win.selected; onTriggered: Qt.openUrlExternally("file://"+win.selected.path.substring(0,win.selected.path.lastIndexOf("/"))) }
        Action { text: qsTr("Refresh download address"); icon.source: win.iconSource("download", win.muted); enabled: !!win.selected && win.automationAvailable; onTriggered: { editDialog.renaming=false; editDialog.open() } }
        Action { text: qsTr("Rename file"); icon.source: win.iconSource("file", win.muted); enabled: !!win.selected && win.automationAvailable; onTriggered: { editDialog.renaming=true; editDialog.open() } }
        Action { text: qsTr("Queue and priority"); icon.source: win.iconSource("queue", win.muted); enabled: !!win.selected && win.automationAvailable; onTriggered: policyDialog.open() }
        IdmMenuSeparator { appWindow: win; }
        Action { text: qsTr("Delete"); icon.source: win.iconSource("delete", win.muted); enabled: !!win.selected; onTriggered: win.deleteSelected() }
    }
    NativeDialog {
        id: urlDialog; objectName: "urlDialog"
        title: qsTr("Add download"); onRejected: win.droppedUrls = []
        modal: true; width: 550; height: Math.min(urlOptions.checked ? 430 : 248, win.contentItem.height-24); anchors.centerIn: parent
        onOpened: {
            urlOptions.checked = useAuthorization.checked || mediaInput.checked
            urlInput.forceActiveFocus(); urlInput.selectAll()
        }
        ColumnLayout {
            anchors.fill: parent; spacing: 12
            ScrollView {
                id: urlScroll
                Layout.fillWidth: true; Layout.fillHeight: true; clip: true; contentWidth: availableWidth
                ColumnLayout {
                    width: urlScroll.availableWidth; spacing: 8
                    Label { text: qsTr("Address"); font.pixelSize: 11; color: win.muted }
                    AppField { id: urlInput; objectName: "urlInput"; Layout.fillWidth: true; placeholderText: "https://"; onAccepted: win.prepareInformation() }
                    Label { visible: win.droppedUrls.length > 0; text: qsTr("%1 more links will be confirmed next.").arg(win.droppedUrls.length); font.pixelSize: 12; color: win.muted }
                    FormDisclosure { id: urlOptions; objectName: "urlOptionsDisclosure"; text: qsTr("Download options"); Layout.fillWidth: true; Layout.topMargin: 12 }
                    ColumnLayout {
                        visible: urlOptions.checked; Layout.fillWidth: true; spacing: 12
                        AppCheckBox {
                            id: mediaInput; objectName: "mediaInput"; text: qsTr("Video or audio")
                            onCheckedChanged: if (checked) urlOptions.checked = true
                        }
                        AppCheckBox {
                            id: useAuthorization; objectName: "useAuthorization"; text: qsTr("Use authorization")
                            onCheckedChanged: if (checked) urlOptions.checked = true
                        }
                        GridLayout {
                            visible: useAuthorization.checked; columns: 2; columnSpacing: 12; rowSpacing: 8; Layout.fillWidth: true
                            Label { text: qsTr("Login"); font.pixelSize: 11; color: win.muted }
                            Label { text: qsTr("Password"); font.pixelSize: 11; color: win.muted }
                            AppField { id: loginInput; objectName: "loginInput"; maximumLength: 512; Layout.fillWidth: true }
                            AppField { id: passwordInput; objectName: "passwordInput"; maximumLength: 4096; echoMode: TextInput.Password; Layout.fillWidth: true }
                            Label { text: qsTr("Authentication method"); font.pixelSize: 11; color: win.muted }
                            IdmComboBox { appWindow: win; id: authMethodChoice; objectName: "authMethodChoice"; model: win.authLabels; Layout.fillWidth: true }
                            Label { text: qsTr("Negotiate and Kerberos use your system ticket. Explicit methods use one connection."); wrapMode: Text.WordWrap; Layout.columnSpan: 2; Layout.fillWidth: true; color: win.muted; font.pixelSize: 11 }
                        }
                    }
                }
            }
            RowLayout { Layout.alignment: Qt.AlignRight; spacing: 8
                AppButton { text: qsTr("Cancel"); onClicked: { win.droppedUrls = []; urlDialog.close() } }
                AppButton { primary: true; text: qsTr("Continue"); enabled: urlInput.text.trim().length > 0; onClicked: win.prepareInformation() }
            }
        }
    }
    component FormDisclosure: Button {
        id: disclosure
        checkable: true; hoverEnabled: true; implicitHeight: 34; leftPadding: 0; rightPadding: 0
        contentItem: RowLayout {
            spacing: 8
            AppIcon { kind: "chevron"; tint: win.muted; rotation: disclosure.checked ? 90 : 0; Layout.preferredWidth: 12; Layout.preferredHeight: 12 }
            Text { text: disclosure.text; font.pixelSize: 12; font.weight: Font.Medium; color: win.ink; Layout.fillWidth: true; elide: Text.ElideRight }
        }
        background: Rectangle { color: disclosure.hovered ? win.hoverSurface : "transparent"; radius: 5; border.color: disclosure.activeFocus ? theme.accent : "transparent" }
    }
    NativeDialog {
        id: infoDialog; objectName: "infoDialog"
        onOpened: infoOptions.checked = infoMedia.checked || useRequestHeaders.checked
        onClosed: { requestHeadersInput.clear(); useRequestHeaders.checked=false }
        title: qsTr("Save download"); onRejected: win.droppedUrls = []
        modal: true; width: 600; height: Math.min(infoOptions.checked ? 590 : 360, win.contentItem.height-24); anchors.centerIn: parent
        ColumnLayout {
            anchors.fill: parent; spacing: 12
            ScrollView {
                id: infoScroll; objectName: "infoScroll"
                Layout.fillWidth: true; Layout.fillHeight: true; clip: true; contentWidth: availableWidth
                ColumnLayout {
                    width: infoScroll.availableWidth; spacing: 8
                    Label { text: win.sourceHost({url:addressInput.text}); font.pixelSize: 12; color: win.muted; elide: Text.ElideRight; Layout.fillWidth: true; Layout.bottomMargin: 12 }
                    Label { text: qsTr("File name"); font.pixelSize: 11; color: win.muted }
                    AppField { id: filenameInput; objectName: "filenameInput"; Layout.fillWidth: true }
                    Label { text: qsTr("Save to"); font.pixelSize: 11; color: win.muted; Layout.topMargin: 8 }
                    RowLayout { Layout.fillWidth: true; spacing: 8
                        AppField { id: directoryInput; objectName: "directoryInput"; Layout.fillWidth: true }
                        AppButton { text: qsTr("Browse..."); onClicked: picker.open() }
                    }
                    FormDisclosure { id: infoOptions; objectName: "infoOptionsDisclosure"; text: qsTr("Download options"); Layout.fillWidth: true; Layout.topMargin: 12 }
                    ColumnLayout {
                        visible: infoOptions.checked; Layout.fillWidth: true; spacing: 12
                        AppCheckBox {
                            id: infoMedia; objectName: "infoMedia"; text: qsTr("Video or audio")
                            onCheckedChanged: if (checked) infoOptions.checked = true
                        }
                        IdmComboBox { appWindow: win; id: mediaFormat; objectName: "mediaFormat"; visible: infoMedia.checked; Layout.fillWidth: true; model: [qsTr("Best quality"),"1080p","720p",qsTr("Audio only")] }
                        Label { visible: infoMedia.checked; text: qsTr("Requires yt-dlp and ffmpeg. Protected streams are not supported."); font.pixelSize: 11; wrapMode: Text.WordWrap; Layout.fillWidth: true; color: win.muted }
                        GridLayout {
                            columns: 2; columnSpacing: 12; rowSpacing: 12; Layout.fillWidth: true; Layout.topMargin: 8
                            Label { text: qsTr("URL"); font.pixelSize: 11; color: win.muted }
                            AppField { id: addressInput; objectName: "addressInput"; Layout.fillWidth: true }
                            Label { text: qsTr("Description"); font.pixelSize: 11; color: win.muted }
                            AppField { id: descriptionInput; objectName: "descriptionInput"; Layout.fillWidth: true }
                            Label { text: qsTr("Connections"); font.pixelSize: 11; color: win.muted }
                            IdmSpinBox { appWindow: win; id: connections; from: 1; to: 16; value: 4; editable: true }
                        }
                        AppCheckBox {
                            id: useRequestHeaders; objectName: "useRequestHeaders"; text: qsTr("Additional HTTP headers")
                            onCheckedChanged: if (checked) infoOptions.checked = true
                        }
                        ScrollView {
                            visible: useRequestHeaders.checked; Layout.fillWidth: true; Layout.preferredHeight: 90
                            AppTextArea { id: requestHeadersInput; objectName: "requestHeadersInput"; selectByMouse: true; font.family: win.mono; placeholderText: '{"X-Api-Key": "value"}' }
                        }
                    }
                }
            }
            Label { visible: win.lastError.length > 0; text: win.lastError; color: win.danger; font.pixelSize: 12; wrapMode: Text.WordWrap; Layout.fillWidth: true }
            RowLayout {
                Layout.alignment: Qt.AlignRight; spacing: 8
                AppButton { text: qsTr("Cancel"); onClicked: { win.droppedUrls = []; infoDialog.close() } }
                AppButton { objectName: "downloadLaterButton"; text: qsTr("Later"); onClicked: win.submit(false) }
                AppButton { objectName: "startDownloadButton"; primary: true; text: qsTr("Download"); onClicked: win.submit(true) }
            }
        }
    }
    Connections {
        target: picker
        function onSelected(directory) { if (infoDialog.visible) directoryInput.text = directory; else defaultsDirectory.text = directory }
        function onError(message) { win.lastError = message }
    }
    NativeDialog {
        id: transferDialog; objectName: "transferDialog"
        title: qsTr("Download progress")
        onClosed: if (win.running(win.transferJob)) win.controller.pause(win.transferId)
        modal: false; width: 650; height: Math.min(transferDetails.checked ? 550 : 345,win.contentItem.height-24); anchors.centerIn: parent
        ColumnLayout {
            anchors.fill: parent; spacing: 12
            RowLayout {
                Layout.fillWidth: true; spacing: 12
                AppIcon { kind: win.transferJob && win.transferJob.media ? "video" : "file"; tint: win.accent; Layout.preferredWidth: 24; Layout.preferredHeight: 24 }
                ColumnLayout {
                    Layout.fillWidth: true; spacing: 3
                    Label { text: win.transferJob ? win.transferJob.name : ""; font.pixelSize: 15; font.weight: Font.DemiBold; elide: Text.ElideMiddle; Layout.fillWidth: true }
                    Label { text: win.transferJob ? win.statusLabel(win.transferJob) : ""; color: win.transferJob && win.transferJob.status === "error" ? win.danger : win.muted; font.pixelSize: 11 }
                }
            }
            RowLayout {
                Layout.fillWidth: true; Layout.topMargin: 8; Layout.bottomMargin: 8; spacing: 16
                ColumnLayout { Layout.fillWidth: true; Layout.preferredWidth: 1; spacing: 4
                    Label { Layout.fillWidth: true; text: qsTr("Transfer rate"); font.pixelSize: 11; color: win.muted }
                    Label { text: win.transferJob ? win.bytes(win.transferJob.speed)+"/s" : ""; font.family: win.mono; font.pixelSize: 18 }
                }
                ColumnLayout { Layout.fillWidth: true; Layout.preferredWidth: 1; spacing: 4
                    Label { Layout.fillWidth: true; text: qsTr("Time left"); font.pixelSize: 11; color: win.muted }
                    Label { text: win.transferJob ? win.duration(win.transferJob.eta) || "—" : "—"; font.family: win.mono; font.pixelSize: 18 }
                }
                ColumnLayout { Layout.fillWidth: true; Layout.preferredWidth: 1; spacing: 4
                    Label { Layout.fillWidth: true; text: qsTr("Downloaded"); font.pixelSize: 11; color: win.muted }
                    Label { text: win.transferJob ? win.bytes(win.transferJob.received) : ""; font.family: win.mono; font.pixelSize: 18 }
                }
            }
            ProgressBar {
                id: overallProgress
                Layout.fillWidth: true; implicitHeight: 6; from: 0; to: 1
                value: win.transferJob && win.transferJob.total > 0 ? win.transferJob.received/win.transferJob.total : 0
                background: Rectangle { color: win.edge }
                contentItem: Item { Rectangle { width: parent.width*overallProgress.position; height: parent.height; color: win.accent } }
            }
            RowLayout { Layout.fillWidth: true; Layout.bottomMargin: 4
                Label { text: win.transferJob ? win.bytes(win.transferJob.total) : ""; color: win.muted; font.pixelSize: 11; Layout.fillWidth: true }
                Label { text: qsTr("Resume capability")+": "+(win.transferJob && win.transferJob.resumable ? qsTr("Yes") : qsTr("No")); color: win.muted; font.pixelSize: 11 }
            }
            FormDisclosure { id: transferDetails; objectName: "transferDetailsDisclosure"; text: qsTr("Connection details"); Layout.fillWidth: true }
            ColumnLayout {
                visible: transferDetails.checked; objectName: "transferConnections"; Layout.fillWidth: true; Layout.fillHeight: true; spacing: 8
                RowLayout { Layout.fillWidth: true; spacing: 12
                    Label { text: qsTr("Source"); font.pixelSize: 11; color: win.muted }
                    Label { text: win.transferJob ? win.sourceHost(win.transferJob) : ""; font.pixelSize: 12; elide: Text.ElideRight; Layout.fillWidth: true }
                }
            Rectangle {
                Layout.fillWidth: true; Layout.preferredHeight: 18; color: win.surface
                Row {
                    anchors.fill: parent; spacing: 3
                    Repeater { model: win.transferJob ? (win.transferJob.parts || []) : []
                        delegate: Rectangle {
                            required property var modelData
                            width: Math.max(1,(parent.width-Math.max(0,(win.transferJob.parts || []).length-1)*3)/Math.max(1,(win.transferJob.parts || []).length)); height: parent.height
                            color: win.edge
                            Rectangle { width: parent.width*(modelData.total > 0 ? Math.min(1,modelData.received/modelData.total) : 0); height: parent.height; color: win.accent; opacity: .75 }
                        }
                    }
                }
            }
            Rectangle {
                Layout.fillWidth: true; Layout.fillHeight: true; Layout.minimumHeight: 28; Layout.preferredHeight: 120; color: win.surface; border.color: win.edge
                Flickable {
                    anchors.fill: parent; anchors.margins: 8
                    clip: true; contentWidth: width; contentHeight: connectionRows.implicitHeight
                    ScrollBar.vertical: IdmScrollBar { appWindow: win; policy: ScrollBar.AsNeeded }
                    Column {
                        id: connectionRows; width: parent.width; spacing: 7
                        Row { spacing: 12
                            Label { text: qsTr("Connection"); width: 90; font.pixelSize: 11; color: win.muted }
                            Label { text: qsTr("Downloaded"); width: 120; font.pixelSize: 11; color: win.muted }
                            Label { text: qsTr("Information"); font.pixelSize: 11; color: win.muted }
                        }
                        Repeater { model: win.transferJob ? (win.transferJob.parts || []) : []
                            delegate: Row {
                                required property var modelData
                                required property int index
                                spacing: 12
                                Label { text: index+1; width: 90; font.family: win.mono; font.pixelSize: 11 }
                                Label { text: win.bytes(modelData.received); width: 120; font.family: win.mono; font.pixelSize: 11 }
                                Label { text: modelData.total > 0 && modelData.received >= modelData.total ? qsTr("Complete") : win.transferJob && win.running(win.transferJob) ? qsTr("Receiving data...") : qsTr("Stopped"); font.pixelSize: 11; color: win.muted }
                            }
                        }
                    }
                }
            }
            }
            Label { visible: win.transferJob && win.transferJob.error.length > 0; text: win.transferJob ? win.transferJob.error : ""; color: win.danger; wrapMode: Text.WordWrap; Layout.fillWidth: true }
            RowLayout { Layout.alignment: Qt.AlignRight; Layout.topMargin: 8
                AppButton { primary: true; text: win.running(win.transferJob) ? qsTr("Pause") : qsTr("Resume"); enabled: win.transferJob && win.transferJob.status !== "complete"; onClicked: { if (win.running(win.transferJob)) win.controller.pause(win.transferId); else win.controller.resume(win.transferId) } }
                AppButton { text: qsTr("Cancel"); onClicked: { if (win.running(win.transferJob)) win.controller.pause(win.transferId); transferDialog.close() } }
                AppButton { text: qsTr("Close"); onClicked: transferDialog.close() }
            }
        }
    }
    NativeDialog {
        id: removeDialog; objectName: "removeDialog"; title: qsTr("Delete download"); modal: true; width: 440; anchors.centerIn: parent
        standardButtons: Dialog.Ok | Dialog.Cancel
        Label { text: qsTr("Remove the selected download from the list? Partial data will be deleted. Completed files will be kept."); wrapMode: Text.WordWrap; width: parent.width }
        onAccepted: { win.controller.remove(win.selectedId); win.selectedId = "" }
    }
    NativeDialog {
        id: optionsDialog; objectName: "optionsDialog"; title: qsTr("Preferences"); modal: true; width: 750; height: Math.min(implicitHeight,win.contentItem.height-12); anchors.centerIn: parent
        onOpened: { win.lastError=""; win.loadOptions() }
        onRejected: win.loadPreferences()
        onClosed: { siteHeaders.clear(); sitePassword.clear(); siteAgent.clear(); siteReferer.clear(); siteStatus.text="" }
        ColumnLayout {
            anchors.fill: parent; spacing: 12
            TabBar { id: optionsTabs; objectName: "optionsTabs"; Layout.fillWidth: true
                AppTabButton { text: qsTr("General") }
                AppTabButton { text: qsTr("Connection") }
                AppTabButton { text: qsTr("File types") }
                AppTabButton { text: qsTr("Downloads") }
                AppTabButton { text: qsTr("Sites") }
                AppTabButton { text: qsTr("Actions") }
                AppTabButton { text: qsTr("Browser") }
            }
            StackLayout {
                currentIndex: optionsTabs.currentIndex; Layout.fillWidth: true; Layout.fillHeight: true; Layout.minimumHeight: 0; implicitHeight: 310
                ColumnLayout {
                    spacing: 6
                    AppCheckBox { id: darkCheck; objectName: "darkModeToggle"; text: qsTr("Dark mode"); onToggled: win.darkMode = checked }
                    AppCheckBox { id: clipboardCheck; text: qsTr("Watch clipboard"); ToolTip.visible: hovered; ToolTip.delay: 500; ToolTip.text: qsTr("Monitor clipboard for download links") }
                    AppCheckBox { id: notificationsCheck; text: qsTr("Completion notifications") }
                    AppCheckBox { id: soundCheck; text: qsTr("Completion sound") }
                    AppCheckBox {
                        objectName: "autostartCheck"; text: qsTr("Start in background when I log in"); enabled: win.desktopAvailable
                        checked: win.desktopAvailable && desktop.autostart
                        onClicked: { if (!desktop.setAutostart(checked)) { win.lastError=desktop.error; checked=desktop.autostart } }
                    }
                    Label { text: qsTr("Login startup is saved immediately."); font.pixelSize: 11; color: win.muted }
                    AppCheckBox {
                        objectName: "weeklyUpdateCheck"; text: qsTr("Check for updates weekly"); enabled: win.desktopAvailable
                        checked: win.desktopAvailable && desktop.automaticUpdateChecks
                        onClicked: { if (!desktop.setAutomaticUpdateChecks(checked)) { win.lastError=desktop.error; checked=desktop.automaticUpdateChecks } }
                    }
                    AppButton { text: qsTr("Customize toolbar and columns"); enabled: win.desktopAvailable; onClicked: layoutDialog.open() }
                    Item { Layout.fillHeight: true }
                }
                ColumnLayout {
                    GridLayout {
                        columns: 2; rowSpacing: 10; Layout.fillWidth: true
                        Label { text: qsTr("Connections per download"); font.pixelSize: 11; color: win.muted }
                        IdmSpinBox { appWindow: win; id: defaultsConnections; from: 1; to: 16; editable: true }
                        Label { text: qsTr("Simultaneous downloads") }
                        IdmSpinBox { appWindow: win; id: concurrentInput; objectName: "concurrentInput"; from: 1; to: 32; editable: true }
                        Label { text: qsTr("Retries on failure") }
                        IdmSpinBox { appWindow: win; id: retriesInput; from: 0; to: 20; editable: true }
                        Label { text: qsTr("Retry delay (seconds)") }
                        IdmSpinBox { appWindow: win; id: retryDelayInput; from: 1; to: 3600; editable: true }
                        Label { text: qsTr("Timeout (seconds)"); font.pixelSize: 11; color: win.muted }
                        IdmSpinBox { appWindow: win; id: timeoutInput; from: 1; to: 3600; editable: true }
                        Label { text: qsTr("Speed limit (bytes/s, 0 = unlimited)"); font.pixelSize: 11; color: win.muted }
                        AppField { id: rateInput; objectName: "rateInput"; Layout.fillWidth: true; inputMethodHints: Qt.ImhDigitsOnly }
                        Label { text: qsTr("Proxy URL") }
                        AppField { id: proxyInput; Layout.fillWidth: true; placeholderText: "http://proxy:8080 or https://proxy:443" }
                    }
                    Item { Layout.fillHeight: true }
                }
                ColumnLayout {
                    Label { text: qsTr("File-type filter"); }
                    AppField { id: typeFilterInput; Layout.fillWidth: true; placeholderText: "ZIP PDF MP4 EXE" }
                    Label { text: qsTr("Empty allows all types. Set browser interception in the extension."); wrapMode: Text.WordWrap; Layout.fillWidth: true }
                    Item { Layout.fillHeight: true }
                }
                ColumnLayout {
                    Label { text: qsTr("Save to") }
                    RowLayout { Layout.fillWidth: true
                        AppField { id: defaultsDirectory; objectName: "defaultsDirectory"; Layout.fillWidth: true }
                        AppButton { text: qsTr("Browse..."); onClicked: picker.open() }
                    }
                    Label { text: qsTr("Speed schedule"); font.bold: true }
                    RowLayout {
                        AppField { id: speedStart; placeholderText: "09:00"; Layout.preferredWidth: 90 }
                        Label { text: qsTr("to") }
                        AppField { id: speedEnd; placeholderText: "17:00"; Layout.preferredWidth: 90 }
                        AppField { id: scheduledRate; placeholderText: qsTr("bytes/s"); Layout.fillWidth: true }
                        AppButton { text: qsTr("Add schedule"); enabled: win.automationAvailable; onClicked: {
                            let schedules=(win.preferences.speedSchedule || []).slice()
                            schedules.push({start:speedStart.text,end:speedEnd.text,rateLimit:Number(scheduledRate.text)})
                            if (!automation.configure({speedSchedule:schedules})) win.lastError=automation.error
                        } }
                    }
                    Label { text: qsTr("%1 speed schedules configured").arg((win.preferences.speedSchedule || []).length) }
                    AppButton { text: qsTr("Clear speed schedules"); enabled: win.automationAvailable; onClicked: { if (!automation.configure({speedSchedule:[]})) win.lastError=automation.error } }
                    Item { Layout.fillHeight: true }
                }
                ColumnLayout {
                    spacing: 8; Layout.minimumHeight: 0
                    RowLayout {
                        Layout.fillWidth: true
                        Label { text: qsTr("Site origin") }
                        AppField { id: siteHost; objectName: "siteHost"; Layout.fillWidth: true; placeholderText: "https://example.com" }
                    }
                    Label { visible: win.automationAvailable && automation.hasLegacySiteSettings; text: qsTr("Old site profiles are inactive. Enter the full origin, then Load and Save to use one."); color: win.muted; font.pixelSize: 11; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                    ScrollView {
                    id: siteScroll; objectName: "siteScroll"; clip: true; contentWidth: availableWidth
                    implicitHeight: 275; Layout.minimumHeight: 0; Layout.fillWidth: true; Layout.fillHeight: true
                    ColumnLayout {
                    width: siteScroll.availableWidth; spacing: 8
                    GridLayout {
                        columns: 2; columnSpacing: 12; rowSpacing: 12; Layout.fillWidth: true
                        Label { text: qsTr("Save to"); font.pixelSize: 11; color: win.muted }
                        AppField { id: siteDirectory; Layout.fillWidth: true }
                        Label { text: qsTr("Connections"); font.pixelSize: 11; color: win.muted }
                        IdmSpinBox { appWindow: win; id: siteConnections; from: 1; to: 16; value: 4 }
                        Label { text: qsTr("Proxy URL"); font.pixelSize: 11; color: win.muted }
                        AppField { id: siteProxy; Layout.fillWidth: true }
                    }
                    FormDisclosure { id: siteAuthentication; objectName: "siteAuthenticationDisclosure"; text: qsTr("Authentication"); Layout.fillWidth: true; Layout.topMargin: 12 }
                    GridLayout {
                        visible: siteAuthentication.checked; columns: 2; columnSpacing: 12; rowSpacing: 12; Layout.fillWidth: true
                        Label { text: qsTr("Authentication method"); font.pixelSize: 11; color: win.muted }
                        IdmComboBox { appWindow: win; id: siteAuthMethod; objectName: "siteAuthMethod"; model: win.authLabels; Layout.fillWidth: true }
                        Label { text: qsTr("Login"); font.pixelSize: 11; color: win.muted }
                        AppField { id: siteLogin; Layout.fillWidth: true; maximumLength: 512; onTextChanged: if (text) siteAuthentication.checked = true }
                        Label { text: qsTr("Password"); font.pixelSize: 11; color: win.muted }
                        AppField { id: sitePassword; Layout.fillWidth: true; maximumLength: 4096; echoMode: TextInput.Password; onTextChanged: if (text) siteAuthentication.checked = true }
                    }
                    FormDisclosure { id: siteRequestHeaders; objectName: "siteHeadersDisclosure"; text: qsTr("Request headers"); Layout.fillWidth: true }
                    ColumnLayout {
                        visible: siteRequestHeaders.checked; Layout.fillWidth: true; spacing: 12
                        GridLayout {
                            columns: 2; columnSpacing: 12; rowSpacing: 12; Layout.fillWidth: true
                            Label { text: qsTr("User agent"); font.pixelSize: 11; color: win.muted }
                            AppField { id: siteAgent; objectName: "siteAgent"; Layout.fillWidth: true; onTextChanged: if (text) siteRequestHeaders.checked = true }
                            Label { text: qsTr("Referer"); font.pixelSize: 11; color: win.muted }
                            AppField { id: siteReferer; objectName: "siteReferer"; Layout.fillWidth: true; onTextChanged: if (text) siteRequestHeaders.checked = true }
                        }
                        Label { text: qsTr("Custom HTTP headers (JSON object)"); font.pixelSize: 11; color: win.muted }
                        AppTextArea { id: siteHeaders; objectName: "siteHeaders"; Layout.fillWidth: true; Layout.preferredHeight: 90; selectByMouse: true; font.family: win.mono; wrapMode: TextEdit.WrapAnywhere; placeholderText: '{"X-Api-Key": "value"}'; onTextChanged: if (text.trim() && text.trim() !== "{}") siteRequestHeaders.checked = true }
                        Label { text: qsTr("Set User-Agent and Referer above. Other headers stay in the JSON object."); font.pixelSize: 11; color: win.muted; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                    }
                    RowLayout {
                        AppButton { objectName: "loadSiteButton"; text: qsTr("Load site settings"); enabled: win.automationAvailable; onClicked: win.loadSiteSettings() }
                        AppButton { objectName: "saveSiteButton"; text: qsTr("Save site settings"); enabled: win.automationAvailable; onClicked: win.saveSiteSettings() }
                    }
                    Label { id: siteStatus; text: ""; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                    }
                    }
                }
                ScrollView {
                    id: actionsScroll; clip: true; contentWidth: availableWidth; Layout.fillWidth: true; Layout.fillHeight: true
                    ColumnLayout {
                    width: actionsScroll.availableWidth; spacing: 5
                    AppCheckBox { id: antivirusCheck; text: qsTr("Scan completed files for viruses") }
                    RowLayout { visible: antivirusCheck.checked; Label { text: qsTr("Scanner program") } AppField { id: scannerProgram; Layout.fillWidth: true; placeholderText: "/usr/bin/clamdscan" } }
                    Label { visible: antivirusCheck.checked; text: qsTr("Scanner arguments, one per line. %f is the downloaded file."); font.pixelSize: 11; color: win.muted }
                    AppTextArea { visible: antivirusCheck.checked; id: scannerArguments; Layout.fillWidth: true; Layout.preferredHeight: 65; selectByMouse: true; placeholderText: "--\n%f" }
                    RowLayout { Label { text: qsTr("After the queue finishes") } IdmComboBox { appWindow: win; id: completionChoice; model: [qsTr("Do nothing"),qsTr("Shut down computer"),qsTr("Disconnect network"),qsTr("Run a program")] } }
                    RowLayout { visible: completionChoice.currentIndex === 3; Label { text: qsTr("Program") } AppField { id: completionProgram; objectName: "completionProgram"; Layout.fillWidth: true; placeholderText: "/usr/bin/program" } }
                    AppTextArea { visible: completionChoice.currentIndex === 3; id: completionArguments; objectName: "completionArguments"; Layout.fillWidth: true; Layout.preferredHeight: 54; placeholderText: qsTr("Arguments, one per line. No shell expansion.") }
                    RowLayout { visible: completionChoice.currentIndex > 0; Label { text: qsTr("Confirmation delay (seconds)") } IdmSpinBox { appWindow: win; id: completionDelayInput; from: 10; to: 3600 } }
                    Label { visible: completionChoice.currentIndex > 0; text: qsTr("Shutdown and network disconnect are opt-in. Cancel pending actions in the notification bar."); font.pixelSize: 11; color: win.muted; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                }
                }
                ColumnLayout {
                    Label { text: win.bridgeAvailable ? qsTr("Native browser bridge is ready.") : qsTr("Native browser bridge is not connected."); font.bold: true; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                    Label { text: qsTr("Install the host, then load the extension in your browser."); wrapMode: Text.WordWrap; Layout.fillWidth: true }
                    RowLayout {
                        AppButton { objectName: "browserInstallButton"; text: qsTr("Install browser integration"); enabled: typeof browserBridge !== "undefined"; onClicked: { let path = browserBridge.install(); win.installMessage = path ? qsTr("Extension folder: ")+path : "" } }
                        AppButton { text: qsTr("Open extension folder"); enabled: typeof browserBridge !== "undefined" && browserBridge.extensionDirectory.length > 0; onClicked: Qt.openUrlExternally("file://"+browserBridge.extensionDirectory) }
                    }
                    AppButton {
                        objectName: "installUrlHandlerButton"; text: qsTr("Enable desktop URL handler"); enabled: win.desktopAvailable
                        onClicked: { if (desktop.installUrlHandler()) win.installMessage=qsTr("URL handler enabled. Use idm-native://download/?url= followed by a percent-encoded URL to confirm a download."); else win.lastError=desktop.error }
                    }
                    Label { text: qsTr("Opt-in URL handling opens the Add URL dialog. Your default browser stays unchanged. Other apps must send a URL to IDM Native; their downloads are not intercepted."); wrapMode: Text.WordWrap; Layout.fillWidth: true; font.pixelSize: 11; color: win.muted }
                    Label { id: integrationMessage; text: win.bridgeError || win.installMessage; color: win.danger; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                    FormDisclosure { id: browserSetup; objectName: "browserSetupDisclosure"; text: qsTr("Browser setup instructions"); Layout.fillWidth: true }
                    Label { objectName: "browserSetupInstructions"; visible: browserSetup.checked; text: qsTr("Chromium: open Extensions, enable Developer mode, choose Load unpacked, and select the extension folder. Firefox: follow the included browser setup instructions."); font.pixelSize: 12; color: win.muted; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                    Label { visible: browserSetup.checked; text: qsTr("Video downloads require yt-dlp and ffmpeg. Enable Video or audio when adding the URL."); font.pixelSize: 11; color: win.muted; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                    Item { Layout.fillHeight: true }
                }
            }
            Label { text: win.lastError; visible: text.length > 0; color: win.danger; wrapMode: Text.WordWrap; Layout.fillWidth: true }
            RowLayout { Layout.alignment: Qt.AlignRight
                AppButton { objectName: "saveOptionsButton"; primary: true; text: qsTr("OK"); onClicked: win.saveOptions() }
                AppButton { text: qsTr("Cancel"); onClicked: { win.loadPreferences(); optionsDialog.close() } }
            }
        }
    }
    NativeDialog {
        id: schedulerDialog; objectName: "schedulerDialog"; title: qsTr("Scheduler")
        modal: true; width: 600; height: Math.min(scheduleTiming.checked ? 490 : 320,win.contentItem.height-24); anchors.centerIn: parent
        onOpened: { win.lastError=""; win.loadQueue(); scheduleTiming.checked = !!queueStart.text || !!queueStop.text || queueRepeat.value > 0 }
        ColumnLayout {
            anchors.fill: parent; spacing: 12
            ScrollView {
                id: scheduleScroll; Layout.fillWidth: true; Layout.fillHeight: true; clip: true; contentWidth: availableWidth
                ColumnLayout {
                    width: scheduleScroll.availableWidth; spacing: 12
                    GridLayout {
                        columns: 2; Layout.fillWidth: true; columnSpacing: 12; rowSpacing: 12
                        Label { text: qsTr("Queue"); font.pixelSize: 11; color: win.muted }
                        IdmComboBox { appWindow: win; id: queueName; objectName: "queueName"; editable: true; Layout.fillWidth: true; model: win.automationAvailable ? automation.queues.map(q => q.name) : ["Main download queue"]; onActivated: win.loadQueue() }
                        Label { text: qsTr("Simultaneous downloads"); font.pixelSize: 11; color: win.muted }
                        IdmSpinBox { appWindow: win; id: queueConcurrent; objectName: "queueConcurrent"; from: 1; to: 32 }
                    }
                    FormDisclosure { id: scheduleTiming; objectName: "scheduleTimingDisclosure"; text: qsTr("Schedule timing"); Layout.fillWidth: true; Layout.topMargin: 12 }
                    GridLayout {
                        visible: scheduleTiming.checked; columns: 2; Layout.fillWidth: true; columnSpacing: 12; rowSpacing: 12
                        Label { text: qsTr("Start date and time"); font.pixelSize: 11; color: win.muted }
                        AppField { id: queueStart; objectName: "queueStart"; Layout.fillWidth: true; placeholderText: "YYYY-MM-DD HH:mm"; onTextChanged: if (text) scheduleTiming.checked = true }
                        Label { text: qsTr("Stop date and time"); font.pixelSize: 11; color: win.muted }
                        AppField { id: queueStop; objectName: "queueStop"; Layout.fillWidth: true; placeholderText: "YYYY-MM-DD HH:mm"; onTextChanged: if (text) scheduleTiming.checked = true }
                        Label { text: qsTr("Repeat every (minutes, 0 = once)"); font.pixelSize: 11; color: win.muted }
                        IdmSpinBox { appWindow: win; id: queueRepeat; from: 0; to: 525600; editable: true; onValueChanged: if (value > 0) scheduleTiming.checked = true }
                    }
                }
            }
            Label { text: win.lastError; visible: text.length > 0; color: win.danger; font.pixelSize: 12; wrapMode: Text.WordWrap; Layout.fillWidth: true }
            RowLayout { Layout.alignment: Qt.AlignRight; spacing: 8
                AppButton { text: qsTr("Close"); onClicked: schedulerDialog.close() }
                AppButton { text: qsTr("Stop"); onClicked: { automation.stopQueue(queueName.editText); schedulerDialog.close() } }
                AppButton { objectName: "saveQueueButton"; text: qsTr("Save schedule"); onClicked: win.saveQueue(false) }
                AppButton { primary: true; text: qsTr("Start now"); onClicked: win.saveQueue(true) }
            }
        }
    }
    NativeDialog {
        id: policyDialog; objectName: "policyDialog"; title: qsTr("Queue and priority"); width: 480; modal: true; anchors.centerIn: parent
        onOpened: {
            let policy = win.automationAvailable ? automation.jobPolicies[win.selectedId] || {} : {}
            jobQueue.editText = policy.queue || "Main download queue"
            jobPriority.currentIndex = policy.priority > 0 ? 1 : policy.priority < 0 ? 2 : 0
        }
        ColumnLayout { anchors.fill: parent; spacing: 12
            IdmComboBox { appWindow: win; id: jobQueue; objectName: "jobQueue"; editable: true; Layout.fillWidth: true; model: win.automationAvailable ? automation.queues.map(q => q.name) : ["Main download queue"] }
            IdmComboBox { appWindow: win; id: jobPriority; objectName: "jobPriority"; model: [qsTr("Normal priority"),qsTr("High priority"),qsTr("Low priority")] }
            Label { text: win.lastError; visible: text.length > 0; color: win.danger; wrapMode: Text.WordWrap; Layout.fillWidth: true }
            AppButton { objectName: "savePolicyButton"; text: qsTr("OK"); onClicked: { if (automation.setJobPolicy(win.selectedId,{queue:jobQueue.editText,priority:[0,10,-10][jobPriority.currentIndex]})) policyDialog.close(); else win.lastError=automation.error } }
        }
    }
    NativeDialog {
        id: listDialog; objectName: "listDialog"; property bool importing: true
        title: importing ? qsTr("Import downloads") : qsTr("Export downloads"); modal: true; width: 590; anchors.centerIn: parent
        ColumnLayout { anchors.fill: parent; spacing: 12
            Label { text: listDialog.importing ? qsTr("Choose a JSON list or a text file with one URL per line.") : qsTr("Export URLs, filenames, descriptions and queue policies. Cookies and credentials are excluded."); wrapMode: Text.WordWrap; Layout.fillWidth: true }
            AppField { id: listPath; objectName: "listPath"; Layout.fillWidth: true; placeholderText: "/home/user/Downloads/download-list.json" }
            Label { text: win.lastError; visible: text.length > 0; color: win.danger; wrapMode: Text.WordWrap; Layout.fillWidth: true }
            RowLayout { Layout.alignment: Qt.AlignRight
                AppButton { objectName: "listSubmitButton"; text: listDialog.importing ? qsTr("Import") : qsTr("Export"); onClicked: { if (listDialog.importing ? automation.importList(listPath.text) : automation.exportList(listPath.text)) listDialog.close(); else win.lastError=automation.error } }
                AppButton { text: qsTr("Cancel"); onClicked: listDialog.close() }
            }
        }
    }
    NativeDialog {
        id: editDialog; objectName: "editDialog"; property bool renaming: false
        title: renaming ? qsTr("Rename file") : qsTr("Refresh download address"); width: 590; modal: true; anchors.centerIn: parent
        onOpened: { win.lastError=""; editValue.text=win.selected ? (renaming ? win.selected.name : win.selected.url) : "" }
        ColumnLayout { anchors.fill: parent; spacing: 12
            AppField { id: editValue; objectName: "editValue"; Layout.fillWidth: true }
            Label { text: win.lastError; visible: text.length > 0; color: win.danger; wrapMode: Text.WordWrap; Layout.fillWidth: true }
            RowLayout { Layout.alignment: Qt.AlignRight
                AppButton { objectName: "saveEditButton"; text: qsTr("OK"); onClicked: { if (editDialog.renaming ? automation.rename(win.selectedId,editValue.text) : automation.refresh(win.selectedId,editValue.text)) editDialog.close(); else win.lastError=automation.error } }
                AppButton { text: qsTr("Cancel"); onClicked: editDialog.close() }
            }
        }
    }
    NativeDialog {
        id: grabberDialog; objectName: "grabberDialog"; title: qsTr("Site Grabber"); modal: true; width: 650; anchors.centerIn: parent
        ColumnLayout { anchors.fill: parent; spacing: 12
            GridLayout {
                columns: 2; Layout.fillWidth: true
                Label { text: qsTr("Website URL") }
                AppField { id: grabberUrl; objectName: "grabberUrl"; Layout.fillWidth: true }
                Label { text: qsTr("Save to") }
                AppField { id: grabberDirectory; objectName: "grabberDirectory"; Layout.fillWidth: true; text: win.defaultDirectory }
                Label { text: qsTr("Link depth") }
                IdmSpinBox { appWindow: win; id: grabberDepth; from: 0; to: 5; value: 1 }
            }
            AppCheckBox { id: synchronizeCheck; text: qsTr("Synchronize this website periodically") }
            RowLayout { visible: synchronizeCheck.checked; Label { text: qsTr("Interval (minutes)") } IdmSpinBox { appWindow: win; id: synchronizeInterval; from: 1; to: 525600; value: 60; editable: true } }
            Label { text: win.automationAvailable ? String(automation.grabber.status || "") + "  " + win.bytes(automation.grabber.received || 0) : "" }
            Label { text: win.automationAvailable ? automation.grabber.error || automation.error : ""; color: win.danger; wrapMode: Text.WordWrap; Layout.fillWidth: true }
            RowLayout { Layout.alignment: Qt.AlignRight
                AppButton { objectName: "grabberStartButton"; text: qsTr("Start"); onClicked: {
                    if (!automation.mirror(grabberUrl.text,grabberDirectory.text,grabberDepth.value)) return
                    if (synchronizeCheck.checked) {
                        let jobs=(win.preferences.syncJobs || []).slice().filter(j => j.url !== grabberUrl.text || j.directory !== grabberDirectory.text)
                        jobs.push({url:grabberUrl.text,directory:grabberDirectory.text,depth:grabberDepth.value,intervalMinutes:synchronizeInterval.value,nextRun:new Date(Date.now()+synchronizeInterval.value*60000).toISOString()})
                        if (!automation.configure({syncJobs:jobs})) win.lastError=automation.error
                    }
                } }
                AppButton { text: qsTr("Stop"); onClicked: automation.cancelMirror() }
                AppButton { text: qsTr("Close"); onClicked: grabberDialog.close() }
            }
        }
    }
    Popup {
        visible: win.automationAvailable && automation.completionNotice.length > 0
        anchors.centerIn: parent; modal: false; width: 500; closePolicy: Popup.NoAutoClose
        contentItem: RowLayout {
            Label { text: win.automationAvailable ? automation.completionNotice : ""; wrapMode: Text.WordWrap; Layout.fillWidth: true }
            AppButton { text: qsTr("Cancel action"); onClicked: automation.cancelCompletionAction() }
        }
    }
    NativeDialog {
        id: shortcutsDialog; title: qsTr("Keyboard shortcuts"); width: 450; anchors.centerIn: parent; modal: true; standardButtons: Dialog.Close
        ColumnLayout {
            width: parent.width; spacing: 16
            Repeater {
                model: [
                    {keys:"Ctrl+N", action:qsTr("Add download")},
                    {keys:"Enter", action:qsTr("Download progress")},
                    {keys:"Space", action:qsTr("Pause or resume")},
                    {keys:"Delete", action:qsTr("Remove download")},
                    {keys:"↑ / ↓", action:qsTr("Select download")},
                    {keys:"F1", action:qsTr("Keyboard shortcuts")},
                    {keys:"Ctrl+Q", action:qsTr("Save and quit")}
                ]
                delegate: RowLayout {
                    required property var modelData
                    Layout.fillWidth: true; spacing: 20
                    Label { text: modelData.keys; font.family: win.mono; font.pixelSize: 12; color: win.muted; Layout.preferredWidth: 100 }
                    Label { text: modelData.action; font.pixelSize: 13; Layout.fillWidth: true; wrapMode: Text.WordWrap }
                }
            }
        }
    }
    NativeDialog {
        id: layoutDialog; objectName: "layoutDialog"; title: qsTr("Toolbar and columns"); width: 700; height: Math.min(540,win.contentItem.height-20)
        anchors.centerIn: parent; modal: true; standardButtons: Dialog.Close
        ColumnLayout {
            anchors.fill: parent
            Label { text: qsTr("Changes are saved immediately. Use arrows to reorder items."); wrapMode: Text.WordWrap; Layout.fillWidth: true }
            RowLayout {
                Layout.fillWidth: true; Layout.fillHeight: true
                Repeater {
                    model: ["toolbar","column"]
                    delegate: ColumnLayout {
                        id: layoutGroup; required property string modelData
                        Layout.fillWidth: true; Layout.fillHeight: true
                        Label { text: layoutGroup.modelData === "toolbar" ? qsTr("Toolbar") : qsTr("Columns"); font.bold: true }
                        ScrollView {
                            Layout.fillWidth: true; Layout.fillHeight: true; clip: true; contentWidth: availableWidth
                            Column {
                                width: parent.width
                                Repeater {
                                    model: win.layoutItems(layoutGroup.modelData)
                                    delegate: RowLayout {
                                        id: layoutRow; required property var modelData; required property int index
                                        width: parent.width
                                        AppCheckBox {
                                            objectName: "layout-"+layoutGroup.modelData+"-"+layoutRow.modelData.key
                                            text: layoutRow.modelData.label; checked: !win.layoutHidden(layoutGroup.modelData,layoutRow.modelData.key); Layout.fillWidth: true
                                            onClicked: win.changeLayout(layoutGroup.modelData,layoutRow.modelData.key,checked,0)
                                        }
                                        AppButton { objectName: "layout-up-"+layoutGroup.modelData+"-"+layoutRow.modelData.key; text: "↑"; Accessible.name: qsTr("Move up"); enabled: layoutRow.index > 0; onClicked: win.changeLayout(layoutGroup.modelData,layoutRow.modelData.key,true,-1) }
                                        AppButton { objectName: "layout-down-"+layoutGroup.modelData+"-"+layoutRow.modelData.key; text: "↓"; Accessible.name: qsTr("Move down"); enabled: layoutRow.index < win.layoutItems(layoutGroup.modelData).length-1; onClicked: win.changeLayout(layoutGroup.modelData,layoutRow.modelData.key,true,1) }
                                    }
                                }
                            }
                        }
                    }
                }
            }
            AppButton { objectName: "resetLayoutButton"; text: qsTr("Restore default layout"); onClicked: { if (!desktop.resetLayout()) win.lastError=desktop.error } }
            Label { text: win.lastError; visible: text.length > 0; color: win.danger; wrapMode: Text.WordWrap; Layout.fillWidth: true }
        }
    }
    NativeDialog {
        id: updatesDialog; objectName: "updatesDialog"; title: qsTr("Updates"); width: 600; height: Math.min(470,win.contentItem.height-20)
        anchors.centerIn: parent; modal: true; standardButtons: Dialog.Close
        ColumnLayout {
            anchors.fill: parent
            Label { text: qsTr("Installed version: %1").arg(Qt.application.version); textFormat: Text.PlainText }
            Label { objectName: "updateStatus"; text: win.desktopAvailable ? desktop.updateStatus : ""; wrapMode: Text.WordWrap; Layout.fillWidth: true; textFormat: Text.PlainText }
            Label { objectName: "releaseName"; text: win.desktopAvailable && desktop.release.tag ? desktop.release.tag+"  "+desktop.release.name+"  "+desktop.release.published : ""; wrapMode: Text.WordWrap; Layout.fillWidth: true; textFormat: Text.PlainText }
            ScrollView {
                Layout.fillWidth: true; Layout.fillHeight: true; clip: true; contentWidth: availableWidth
                TextArea { objectName: "releaseNotes"; text: win.desktopAvailable ? desktop.release.notes || "" : ""; readOnly: true; selectByMouse: true; wrapMode: TextEdit.Wrap; textFormat: TextEdit.PlainText; color: win.ink; background: Item {} }
            }
            Label { text: qsTr("Open the release page to review installation instructions. Updates are never installed automatically."); font.pixelSize: 11; color: win.muted; wrapMode: Text.WordWrap; Layout.fillWidth: true }
            RowLayout {
                AppButton { objectName: "retryUpdateButton"; text: qsTr("Check again"); enabled: win.desktopAvailable && !desktop.checking; onClicked: desktop.checkForUpdates() }
                AppButton { objectName: "openReleaseButton"; text: qsTr("Open release page"); enabled: win.desktopAvailable && !!desktop.release.url; onClicked: desktop.openRelease() }
            }
        }
    }
    NativeDialog {
        id: aboutDialog; title: qsTr("About"); width: 430; anchors.centerIn: parent; modal: true; standardButtons: Dialog.Close
        ColumnLayout {
            width: parent.width; spacing: 12
            Label { text: "IDM Native"; font.pixelSize: 18; font.weight: Font.DemiBold }
            Label { text: qsTr("Independent download manager for files and media."); wrapMode: Text.WordWrap; Layout.fillWidth: true }
            Label { text: qsTr("Not affiliated with Tonec."); color: win.muted; font.pixelSize: 12 }
        }
    }
    AutomationDialog {
        id: automationProjects
        controller: win.automationAvailable ? automation : null
        appWindow: win
        parent: Overlay.overlay
        anchors.centerIn: parent
    }

}
