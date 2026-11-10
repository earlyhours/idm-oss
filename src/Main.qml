import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

ApplicationWindow {
    id: win
    width: 1120; height: 650
    minimumWidth: 820; minimumHeight: 500
    visible: true
    title: qsTr("Internet Download Manager")
    color: win.panel
    font.family: "Noto Sans"
    font.pixelSize: 12
    palette.window: win.panel
    palette.windowText: win.ink
    palette.base: win.surface
    palette.text: win.ink
    palette.button: win.panel
    palette.buttonText: win.ink
    palette.highlight: win.selection
    palette.highlightedText: win.ink
    readonly property bool automationAvailable: typeof automation !== "undefined"
    readonly property var controller: automationAvailable ? automation : backend
    readonly property var preferences: automationAvailable ? automation.preferences : ({})
    property bool darkMode: false
    readonly property color panel: darkMode ? "#2c2c2c" : "#f0f0f0"
    readonly property color surface: darkMode ? "#202020" : "white"
    readonly property color ink: darkMode ? "#e6e6e6" : "#202020"
    readonly property color edge: darkMode ? "#5a5a5a" : "#aaa"
    readonly property color selection: darkMode ? "#234c69" : "#cce8ff"
    readonly property color hoverSurface: darkMode ? "#34454f" : "#e8f3fc"
    property var droppedUrls: []
    property string filter: "all"
    property string selectedId: ""
    property string transferId: ""
    readonly property var transferJob: backend.downloads.find(d => d.id === transferId) || null
    property string sortKey: "created"
    property bool descending: true
    property bool categoriesVisible: true
    property bool filesExpanded: true
    property bool queuesExpanded: true
    property bool helpVisible: shortcutsDialog.visible
    property int defaultConnections: 4
    property string defaultDirectory: backend.downloadDirectory
    property string lastError: ""
    property string installMessage: ""
    readonly property string currentError: lastError || backend.error || (automationAvailable ? automation.error : "")
    property var pendingRequest: ({})
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
    property bool editing: urlDialog.visible || infoDialog.visible || optionsDialog.visible || removeDialog.visible || shortcutsDialog.visible || aboutDialog.visible || schedulerDialog.visible || listDialog.visible || editDialog.visible || grabberDialog.visible || policyDialog.visible
    readonly property bool bridgeAvailable: typeof browserBridge !== "undefined" && browserBridge.available
    readonly property string bridgeError: typeof browserBridge !== "undefined" ? browserBridge.error : ""
    readonly property var columns: [
        {key:"name", label:qsTr("File Name"), width:270}, {key:"total",label:qsTr("Size"),width:86},
        {key:"status",label:qsTr("Status"),width:105}, {key:"eta",label:qsTr("Time left"),width:85},
        {key:"speed",label:qsTr("Transfer rate"),width:96}, {key:"created",label:qsTr("Date added"),width:150},
        {key:"description",label:qsTr("Description"),width:220}]
    function bytes(value) {
        let language = translator.language
        if (value == null || value < 0) return qsTr("Unknown")
        let units = ["B","KB","MB","GB","TB"], n = Number(value), i = 0
        while (n >= 1024 && i < units.length-1) { n /= 1024; i++ }
        return n.toLocaleString(Qt.locale(), 'f', i ? 2 : 0) + " " + units[i]
    }
    function duration(seconds) {
        if (seconds == null || seconds < 0) return ""
        let s = Math.ceil(seconds), h = Math.floor(s/3600), m = Math.floor(s%3600/60)
        return (h ? h+":"+String(m).padStart(2,"0") : m) + ":" + String(s%60).padStart(2,"0")
    }
    function running(d) { return d && (d.status === "downloading" || d.status === "probing") }
    function statusLabel(d) {
        if (d.status === "downloading" && d.total > 0) return (100*d.received/d.total).toFixed(2)+"%"
        return ({probing:qsTr("Connecting..."),downloading:qsTr("Receiving data..."),paused:qsTr("Stopped"),complete:qsTr("Complete"),error:qsTr("Error")})[d.status] || d.status
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
        darkMode = !!preferences.darkMode
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
        completionChoice.currentIndex = ["none","shutdown","disconnect"].indexOf(preferences.completionAction || "none")
        completionDelayInput.value = preferences.completionDelaySeconds || 30
        let command = preferences.scannerCommand || []
        scannerProgram.text = command.length ? command[0] : ""
        scannerArguments.text = command.slice(1).join("\n")
    }
    function saveOptions() {
        let command = scannerProgram.text.trim() ? [scannerProgram.text.trim()].concat(scannerArguments.text.split("\n").filter(x => x.length)) : []
        let value = {directory:defaultsDirectory.text,connections:defaultsConnections.value,maxConcurrent:concurrentInput.value,
            retryCount:retriesInput.value,retryDelaySeconds:retryDelayInput.value,timeoutSeconds:timeoutInput.value,
            rateLimit:Number(rateInput.text),proxy:proxyInput.text,clipboardMonitor:clipboardCheck.checked,
            notifications:notificationsCheck.checked,sound:soundCheck.checked,antivirus:antivirusCheck.checked,
            scannerCommand:command,darkMode:darkCheck.checked,typeFilter:typeFilterInput.text,
            completionAction:["none","shutdown","disconnect"][completionChoice.currentIndex],completionDelaySeconds:completionDelayInput.value}
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
        let host = pendingRequest.url.match(/^https?:\/\/([^/:?#]+)/i)
        let site = host ? (preferences.siteSettings || {})[host[1].toLowerCase()] || {} : {}
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
        if (useAuthorization.checked) { request.username = loginInput.text; request.password = passwordInput.text }
        let id = win.automationAvailable ? automation.enqueue(request) : backend.addRequest(request)
        if (!id) { lastError = win.automationAvailable ? automation.error : backend.error; return }
        selectedId = id; lastError = ""; urlInput.clear(); loginInput.clear(); passwordInput.clear(); useAuthorization.checked=false; infoDialog.close()
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
    onClosing: win.controller.pauseAll()
    Shortcut { sequence: "Ctrl+N"; onActivated: win.newDownload() }
    Shortcut { sequence: "Ctrl+Q"; onActivated: win.close() }
    Shortcut { sequence: "Delete"; enabled: !win.editing && !transferDialog.visible; onActivated: win.deleteSelected() }
    Shortcut { sequence: "Space"; enabled: !win.editing && !transferDialog.visible; onActivated: win.toggleSelected() }
    Shortcut { sequence: "Return"; enabled: !win.editing && !!win.selected && !transferDialog.visible; onActivated: win.openTransfer() }
    Shortcut { sequence: "Up"; enabled: !win.editing && !transferDialog.visible; onActivated: win.moveSelection(-1) }
    Shortcut { sequence: "Down"; enabled: !win.editing && !transferDialog.visible; onActivated: win.moveSelection(1) }
    Shortcut { sequence: "F1"; onActivated: shortcutsDialog.open() }
    Shortcut { sequence: "Menu"; enabled: !win.editing && !!win.selected; onActivated: rowMenu.popup() }

    component ClassicIcon: Image {
        property string kind: "add"
        sourceSize.width: 28; sourceSize.height: 28
        width: 28; height: 28; fillMode: Image.PreserveAspectFit
        source: {
            let paths = {
                add:'<path fill="#f9d86a" stroke="#9e7a29" d="M2 5h10l3 3h11v16H2z"/><path fill="#4fa7d4" stroke="#246388" d="M2 12h26l-5 13H2z"/><path fill="#54b143" stroke="#287527" d="M19 1h5v8h4l-6.5 6L15 9h4z"/>',
                resume:'<circle fill="#e8f2dd" stroke="#63913e" cx="14" cy="14" r="12"/><path fill="#4caa35" stroke="#287329" d="M11 6l11 8-11 8z"/>',
                stop:'<circle fill="#e8f2dd" stroke="#63913e" cx="13" cy="15" r="11"/><rect fill="#e64d45" stroke="#a32c2c" x="14" y="2" width="12" height="12" rx="2"/>',
                stopall:'<circle fill="#e8f2dd" stroke="#63913e" cx="10" cy="16" r="9"/><rect fill="#ef6b5e" stroke="#a32c2c" x="11" y="3" width="11" height="11" rx="1"/><rect fill="#e64d45" stroke="#a32c2c" x="16" y="9" width="11" height="11" rx="1"/>',
                delete:'<path fill="#fff" stroke="#6a8592" d="M7 2h12l5 5v19H7z"/><path stroke="#c43a36" stroke-width="4" d="M3 10l14 14M17 10L3 24"/>',
                completed:'<path fill="#7ca5db" stroke="#3d6199" d="M7 8h17l-2 18H9z"/><path fill="#fff" stroke="#68849b" d="M3 3h12v15H3z"/><path fill="none" stroke="#3b932e" stroke-width="3" d="M4 9l4 4 6-7"/>',
                options:'<path fill="#e6b655" stroke="#986f2c" d="M3 4h23v20H3z"/><path stroke="#7c5929" d="M7 9h15M7 14h15M7 19h15"/><path stroke="#fff" stroke-width="3" d="M11 7v4M18 12v4M13 17v4"/>',
                queue:'<path fill="#f6d35c" stroke="#a48227" d="M3 5h14v5H3zM3 12h14v5H3zM3 19h14v5H3z"/><path fill="#54a93a" stroke="#2d7524" d="M18 9l10 7-10 7z"/>',
                queueStop:'<path fill="#f6d35c" stroke="#a48227" d="M3 5h14v5H3zM3 12h14v5H3zM3 19h14v5H3z"/><rect fill="#df4e43" stroke="#a32c2c" x="17" y="10" width="11" height="11"/>',
                folder:'<path fill="#f6d779" stroke="#a58a35" d="M2 6h10l3 3h12v16H2z"/><path fill="#ffe8a2" stroke="#a58a35" d="M2 12h25v13H2z"/>',
                file:'<path fill="#fff" stroke="#7693a3" d="M7 2h11l6 6v18H7z"/><path fill="#d6e9f4" stroke="#7693a3" d="M18 2v6h6"/><path stroke="#4c87b4" d="M10 13h11M10 17h11M10 21h8"/>',
                video:'<path fill="#8ab7d4" stroke="#4e718b" d="M2 5h25v20H2z"/><path fill="#fff" d="M11 9l9 6-9 6z"/>',
                music:'<path fill="none" stroke="#497da0" stroke-width="3" d="M11 21V7l12-3v14M11 9l12-3"/><ellipse fill="#538eae" cx="7" cy="22" rx="5" ry="3"/><ellipse fill="#538eae" cx="19" cy="19" rx="5" ry="3"/>'
            }
            return "data:image/svg+xml;utf8,"+encodeURIComponent('<svg xmlns="http://www.w3.org/2000/svg" width="30" height="30" viewBox="0 0 30 30">'+(paths[kind]||paths.file)+'</svg>')
        }
    }
    component ClassicButton: Button {
        id: button
        implicitHeight: 27; implicitWidth: Math.max(82,contentItem.implicitWidth+24)
        padding: 5
        background: Rectangle { color: button.down ? win.selection : button.hovered ? win.hoverSurface : win.panel; border.width: 1; border.color: button.activeFocus ? "#0078d7" : "#adadad"; radius: 2; opacity: button.enabled ? 1 : .5 }
        contentItem: Text { text: button.text; font: button.font; color: button.enabled ? win.ink : "#888"; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
    }
    component ClassicField: TextField {
        implicitHeight: 27; padding: 5; selectByMouse: true; color: win.ink
        background: Rectangle { color: win.surface; border.color: parent.activeFocus ? "#0078d7" : win.edge; border.width: 1 }
    }
    component Tool: Button {
        id: tool
        property string kind: "add"
        implicitWidth: 77; implicitHeight: 61
        padding: 3; hoverEnabled: true
        background: Rectangle { color: tool.down ? win.selection : tool.hovered ? win.hoverSurface : "transparent"; border.color: tool.hovered ? "#91c9f7" : "transparent"; radius: 2 }
        contentItem: Column {
            spacing: 3; opacity: tool.enabled ? 1 : .36
            ClassicIcon { kind: tool.kind; anchors.horizontalCenter: parent.horizontalCenter }
            Text { text: tool.text; font.pixelSize: 11; color: win.ink; anchors.horizontalCenter: parent.horizontalCenter }
        }
        ToolTip.visible: hovered
        ToolTip.text: text
    }
    menuBar: MenuBar {
        Menu { title: qsTr("Tasks")
            Action { text: qsTr("Add URL"); onTriggered: win.newDownload() }
            MenuSeparator {}
            Action { text: qsTr("Import downloads"); enabled: win.automationAvailable; onTriggered: { listDialog.importing=true; listDialog.open() } }
            Action { text: qsTr("Export downloads"); enabled: win.automationAvailable; onTriggered: { listDialog.importing=false; listDialog.open() } }
            Action { text: qsTr("Site Grabber"); enabled: win.automationAvailable; onTriggered: grabberDialog.open() }
            MenuSeparator {}
            Action { text: qsTr("Exit"); onTriggered: win.close() }
        }
        Menu { title: qsTr("Downloads")
            Action { text: qsTr("Resume"); enabled: !!win.selected && !win.running(win.selected) && win.selected.status !== "complete"; onTriggered: win.controller.resume(win.selectedId) }
            Action { text: qsTr("Stop"); enabled: win.running(win.selected); onTriggered: win.controller.pause(win.selectedId) }
            Action { text: qsTr("Stop All"); onTriggered: win.controller.pauseAll() }
            MenuSeparator {}
            Action { text: qsTr("Delete"); enabled: !!win.selected; onTriggered: win.deleteSelected() }
            Action { text: qsTr("Delete Completed"); onTriggered: win.controller.removeCompleted() }
            MenuSeparator {}
            Action { text: qsTr("Start Queue"); onTriggered: win.startQueue() }
            Action { text: qsTr("Stop Queue"); onTriggered: win.stopQueue() }
            Action { text: qsTr("Scheduler"); enabled: win.automationAvailable; onTriggered: schedulerDialog.open() }
            Action { text: qsTr("Options"); onTriggered: optionsDialog.open() }
        }
        Menu { title: qsTr("View")
            Action { text: qsTr("Categories"); checkable: true; checked: win.categoriesVisible; onTriggered: win.categoriesVisible = checked }
            Action { text: qsTr("Download progress"); enabled: !!win.selected; onTriggered: win.openTransfer() }
        }
        Menu { title: qsTr("Help")
            Action { text: qsTr("Keyboard shortcuts"); onTriggered: shortcutsDialog.open() }
            Action { text: qsTr("Browser integration"); onTriggered: { optionsTabs.currentIndex = 6; optionsDialog.open() } }
            Action { text: qsTr("About"); onTriggered: aboutDialog.open() }
        }
    }
    ColumnLayout {
        LayoutMirroring.enabled: Qt.application.layoutDirection === Qt.RightToLeft
        LayoutMirroring.childrenInherit: true
        anchors.fill: parent; spacing: 0
        Rectangle {
            Layout.fillWidth: true; Layout.preferredHeight: 68; color: win.panel; border.color: "#c8c8c8"
            Row {
                anchors.left: parent.left; anchors.leftMargin: 4; anchors.verticalCenter: parent.verticalCenter; spacing: 1
                Tool { text: qsTr("Add URL"); kind: "add"; onClicked: win.newDownload() }
                Tool { text: qsTr("Resume"); kind: "resume"; enabled: !!win.selected && !win.running(win.selected) && win.selected.status !== "complete"; onClicked: win.controller.resume(win.selectedId) }
                Tool { text: qsTr("Stop"); kind: "stop"; enabled: win.running(win.selected); onClicked: win.controller.pause(win.selectedId) }
                Tool { text: qsTr("Stop All"); kind: "stopall"; onClicked: win.controller.pauseAll() }
                Rectangle { width: 1; height: 49; color: "#c5c5c5"; anchors.verticalCenter: parent.verticalCenter }
                Tool { text: qsTr("Delete"); kind: "delete"; enabled: !!win.selected; onClicked: win.deleteSelected() }
                Tool { objectName: "deleteCompletedButton"; text: qsTr("Delete Completed"); kind: "completed"; implicitWidth: 104; onClicked: win.controller.removeCompleted() }
                Rectangle { width: 1; height: 49; color: "#c5c5c5"; anchors.verticalCenter: parent.verticalCenter }
                Tool { text: qsTr("Scheduler"); kind: "queue"; enabled: win.automationAvailable; onClicked: schedulerDialog.open() }
                Tool { text: qsTr("Options"); kind: "options"; onClicked: optionsDialog.open() }
                Tool { objectName: "startQueueButton"; text: qsTr("Start Queue"); kind: "queue"; onClicked: win.startQueue() }
                Tool { text: qsTr("Stop Queue"); kind: "queueStop"; onClicked: win.stopQueue() }
            }
        }
        RowLayout {
            Layout.fillWidth: true; Layout.fillHeight: true; spacing: 4
            Rectangle {
                visible: win.categoriesVisible; Layout.preferredWidth: 182; Layout.fillHeight: true
                color: win.surface; border.color: win.edge
                Column {
                    width: parent.width
                    Rectangle { width: parent.width; height: 24; color: win.panel; border.color: "#d0d0d0"
                        Text { text: qsTr("Categories"); anchors { left: parent.left; leftMargin: 6; verticalCenter: parent.verticalCenter } font.bold: true; font.pixelSize: 11 }
                    }
                    Repeater {
                        model: [{key:"all",label:qsTr("All Downloads"),depth:0,icon:"folder"},
                            {key:"Compressed",label:qsTr("Compressed"),depth:1,icon:"folder"},{key:"Documents",label:qsTr("Documents"),depth:1,icon:"file"},
                            {key:"Music",label:qsTr("Music"),depth:1,icon:"music"},{key:"Programs",label:qsTr("Programs"),depth:1,icon:"file"},{key:"Video",label:qsTr("Video"),depth:1,icon:"video"},{key:"Other",label:qsTr("Other"),depth:1,icon:"folder"},
                            {key:"unfinished",label:qsTr("Unfinished"),depth:0,icon:"folder"},{key:"finished",label:qsTr("Finished"),depth:0,icon:"folder"},
                            {key:"queues",label:qsTr("Queues"),depth:0,icon:"queue"}].concat((win.automationAvailable ? automation.queues : [{name:"Main download queue"}]).map(q => ({key:"queue:"+q.name,label:q.name === "Main download queue" ? qsTr("Main download queue") : q.name,depth:1,icon:"queue"})))
                        delegate: Rectangle {
                            required property var modelData
                            visible: modelData.depth === 0 || (modelData.key.startsWith("queue:") ? win.queuesExpanded : win.filesExpanded)
                            width: 180; height: visible ? 24 : 0; color: win.filter === modelData.key ? win.selection : "transparent"
                            Row {
                                anchors.verticalCenter: parent.verticalCenter; x: 5+modelData.depth*14; spacing: 4
                                Text { text: modelData.depth || (modelData.key !== "all" && modelData.key !== "queues") ? "" : (modelData.key === "all" ? win.filesExpanded : win.queuesExpanded) ? "⊟" : "⊞"; width: modelData.depth ? 0 : 10; font.pixelSize: 11; color: "#777" }
                                ClassicIcon { kind: modelData.icon; width: 17; height: 17 }
                                Text { text: modelData.label; font.pixelSize: 11; color: win.ink; anchors.verticalCenter: parent.verticalCenter }
                            }
                            MouseArea {
                                anchors.fill: parent; acceptedButtons: Qt.LeftButton | Qt.RightButton
                                onClicked: mouse => {
                                    win.filter = modelData.key === "queues" ? "queue:Main download queue" : modelData.key
                                    if (mouse.button === Qt.RightButton && modelData.key.startsWith("queue:")) {
                                        queueMenu.queue = modelData.key.slice(6); queueMenu.popup()
                                    } else if (mouse.x < 20 && modelData.key === "all") win.filesExpanded = !win.filesExpanded
                                    else if (mouse.x < 20 && modelData.key === "queues") win.queuesExpanded = !win.queuesExpanded
                                }
                            }
                        }
                    }
                }
            }
            Rectangle {
                Layout.fillWidth: true; Layout.fillHeight: true; color: win.surface; border.color: win.edge
                Flickable {
                    id: tableScroll
                    anchors.fill: parent; anchors.margins: 1
                    clip: true; contentWidth: Math.max(width,win.columns.reduce((n,c) => n+c.width,0)); contentHeight: height; flickableDirection: Flickable.HorizontalFlick
                    ScrollBar.horizontal: ScrollBar { policy: ScrollBar.AsNeeded }
                    Column {
                        width: tableScroll.contentWidth; height: tableScroll.height
                        Row {
                            height: 27
                            Repeater { model: win.columns
                                delegate: Rectangle {
                                    required property var modelData
                                    width: modelData.width; height: 27; color: headerMouse.containsMouse ? win.hoverSurface : win.panel; border.color: "#d5d5d5"
                                    Text { color: win.ink; anchors.fill: parent; anchors.leftMargin: 7; verticalAlignment: Text.AlignVCenter; font.pixelSize: 11; text: modelData.label + (win.sortKey === modelData.key ? (win.descending ? "  ▾" : "  ▴") : "") }
                                    MouseArea { id: headerMouse; anchors.fill: parent; hoverEnabled: true; onClicked: { win.descending = win.sortKey === modelData.key ? !win.descending : false; win.sortKey = modelData.key } }
                                }
                            }
                        }
                        ListView {
                            id: rows; objectName: "downloadTable"
                            width: parent.width; height: parent.height-42; clip: true
                            model: win.shown
                            ScrollBar.vertical: ScrollBar {
                                parent: tableScroll.parent
                                anchors.right: parent.right; anchors.top: parent.top; anchors.topMargin: 28
                                anchors.bottom: parent.bottom; anchors.bottomMargin: 15
                                policy: ScrollBar.AsNeeded
                            }
                            delegate: Rectangle {
                                id: row
                                required property var modelData
                                required property int index
                                width: rows.width; height: 25
                                color: win.selectedId === modelData.id ? win.selection : index%2 ? (win.darkMode ? "#252525" : "#fcfcfc") : win.surface
                                Row {
                                    Repeater { model: win.columns
                                        delegate: Item {
                                            required property var modelData
                                            width: modelData.width; height: 25
                                            ClassicIcon { visible: modelData.key === "name"; kind: row.modelData.category === "Video" ? "video" : "file"; width: 16; height: 16; x: 4; anchors.verticalCenter: parent.verticalCenter }
                                            Text { x: modelData.key === "name" ? 25 : 7; width: parent.width-x-6; height: parent.height; text: win.cell(row.modelData,modelData.key); elide: Text.ElideRight; verticalAlignment: Text.AlignVCenter; color: row.modelData.status === "error" ? "#d96767" : win.ink; font.pixelSize: 11 }
                                        }
                                    }
                                }
                                MouseArea {
                                    anchors.fill: parent; acceptedButtons: Qt.LeftButton | Qt.RightButton
                                    onClicked: mouse => { win.selectedId = row.modelData.id; if (mouse.button === Qt.RightButton) rowMenu.popup() }
                                    onDoubleClicked: { win.selectedId = row.modelData.id; win.openTransfer() }
                                }
                            }
                        }
                    }
                }
            }
        }
        Rectangle {
            Layout.fillWidth: true; Layout.preferredHeight: 25; color: win.panel; border.color: "#c8c8c8"
            RowLayout {
                anchors.fill: parent; anchors.leftMargin: 7; anchors.rightMargin: 7
                Label { text: win.currentError || qsTr("%1 downloads").arg(backend.downloads.length); elide: Text.ElideRight; Layout.fillWidth: true; color: win.currentError ? "#d96767" : win.ink; font.pixelSize: 11 }
                Rectangle { width: 7; height: 7; color: theme.accent; border.color: "#8a8a8a" }
                Label { text: win.bridgeAvailable ? qsTr("Browser integration ready") : qsTr("Browser integration not connected"); font.pixelSize: 11 }
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
    Menu {
        id: queueMenu
        property string queue: "Main download queue"
        Action { text: qsTr("Start Queue"); enabled: win.automationAvailable; onTriggered: automation.startQueue(queueMenu.queue) }
        Action { text: qsTr("Stop Queue"); enabled: win.automationAvailable; onTriggered: automation.stopQueue(queueMenu.queue) }
        Action { text: qsTr("Scheduler"); enabled: win.automationAvailable; onTriggered: { queueName.editText=queueMenu.queue; schedulerDialog.open(); win.loadQueue() } }
    }
    Menu {
        id: rowMenu
        Action { text: qsTr("Download progress"); onTriggered: win.openTransfer() }
        Action { text: qsTr("Resume"); enabled: !!win.selected && !win.running(win.selected) && win.selected.status !== "complete"; onTriggered: win.controller.resume(win.selectedId) }
        Action { text: qsTr("Stop"); enabled: win.running(win.selected); onTriggered: win.controller.pause(win.selectedId) }
        MenuSeparator {}
        Action { text: qsTr("Open containing folder"); enabled: !!win.selected; onTriggered: Qt.openUrlExternally("file://"+win.selected.path.substring(0,win.selected.path.lastIndexOf("/"))) }
        Action { text: qsTr("Refresh download address"); enabled: !!win.selected && win.automationAvailable; onTriggered: { editDialog.renaming=false; editDialog.open() } }
        Action { text: qsTr("Rename file"); enabled: !!win.selected && win.automationAvailable; onTriggered: { editDialog.renaming=true; editDialog.open() } }
        Action { text: qsTr("Queue and priority"); enabled: !!win.selected && win.automationAvailable; onTriggered: policyDialog.open() }
        Action { text: qsTr("Delete"); enabled: !!win.selected; onTriggered: win.deleteSelected() }
    }
    Dialog {
        id: urlDialog; objectName: "urlDialog"
        title: qsTr("Enter new URL to download"); onRejected: win.droppedUrls = []; modal: true; width: 570; anchors.centerIn: parent
        onOpened: { urlInput.forceActiveFocus(); urlInput.selectAll() }
        ColumnLayout {
            anchors.fill: parent; spacing: 10
            RowLayout { Layout.fillWidth: true
                Label { text: qsTr("Address"); Layout.preferredWidth: 62 }
                ClassicField { id: urlInput; objectName: "urlInput"; Layout.fillWidth: true; placeholderText: "https://"; onAccepted: win.prepareInformation() }
            }
            CheckBox { id: useAuthorization; objectName: "useAuthorization"; text: qsTr("Use authorization") }
            RowLayout {
                enabled: useAuthorization.checked; Layout.fillWidth: true
                Label { text: qsTr("Login") }
                ClassicField { id: loginInput; objectName: "loginInput"; maximumLength: 512; Layout.fillWidth: true }
                Label { text: qsTr("Password") }
                ClassicField { id: passwordInput; objectName: "passwordInput"; maximumLength: 4096; echoMode: TextInput.Password; Layout.fillWidth: true }
            }
            Label { visible: win.droppedUrls.length > 0; text: qsTr("%1 more links will be confirmed next.").arg(win.droppedUrls.length); color: win.ink }
            CheckBox { id: mediaInput; objectName: "mediaInput"; text: qsTr("Download video / audio stream (HLS, DASH or video page)") }
            RowLayout { Layout.alignment: Qt.AlignRight
                ClassicButton { text: qsTr("OK"); enabled: urlInput.text.trim().length > 0; onClicked: win.prepareInformation() }
                ClassicButton { text: qsTr("Cancel"); onClicked: { win.droppedUrls = []; urlDialog.close() } }
            }
        }
    }
    Dialog {
        id: infoDialog; objectName: "infoDialog"
        title: qsTr("Download File Info"); onRejected: win.droppedUrls = []; modal: true; width: 650; anchors.centerIn: parent
        ColumnLayout {
            anchors.fill: parent; spacing: 9
            GridLayout {
                columns: 2; columnSpacing: 10; rowSpacing: 8; Layout.fillWidth: true
                Label { text: qsTr("URL") }
                ClassicField { id: addressInput; objectName: "addressInput"; Layout.fillWidth: true }
                Label { text: qsTr("Save as") }
                ClassicField { id: filenameInput; objectName: "filenameInput"; Layout.fillWidth: true }
                Label { text: qsTr("Save to") }
                RowLayout { Layout.fillWidth: true
                    ClassicField { id: directoryInput; objectName: "directoryInput"; Layout.fillWidth: true }
                    ClassicButton { text: qsTr("Browse..."); onClicked: picker.open() }
                }
                Label { text: qsTr("Description") }
                ClassicField { id: descriptionInput; objectName: "descriptionInput"; Layout.fillWidth: true }
                Label { text: qsTr("Connections") }
                SpinBox { id: connections; from: 1; to: 16; value: 4; editable: true; implicitHeight: 28 }
            }
            ComboBox { id: mediaFormat; objectName: "mediaFormat"; visible: infoMedia.checked; model: [qsTr("Best quality"),"1080p","720p",qsTr("Audio only")] }
            CheckBox { id: infoMedia; text: qsTr("Video / audio download"); objectName: "infoMedia" }
            Label { visible: infoMedia.checked; text: qsTr("Uses yt-dlp and ffmpeg. Protected streams are not supported."); wrapMode: Text.WordWrap; Layout.fillWidth: true; color: "#555" }
            Label { visible: win.lastError.length > 0; text: win.lastError; color: "#a82020"; wrapMode: Text.WordWrap; Layout.fillWidth: true }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                ClassicButton { objectName: "downloadLaterButton"; text: qsTr("Download Later"); onClicked: win.submit(false) }
                ClassicButton { objectName: "startDownloadButton"; text: qsTr("Start Download"); onClicked: win.submit(true) }
                ClassicButton { text: qsTr("Cancel"); onClicked: { win.droppedUrls = []; infoDialog.close() } }
            }
        }
    }
    Connections {
        target: picker
        function onSelected(directory) { if (infoDialog.visible) directoryInput.text = directory; else defaultsDirectory.text = directory }
        function onError(message) { win.lastError = message }
    }
    Dialog {
        id: transferDialog; objectName: "transferDialog"
        title: win.transferJob ? win.statusLabel(win.transferJob)+"  "+win.transferJob.name : qsTr("Download progress")
        onClosed: if (win.running(win.transferJob)) win.controller.pause(win.transferId)
        modal: false; width: 650; anchors.centerIn: parent
        ColumnLayout {
            anchors.fill: parent; spacing: 10
            GridLayout {
                columns: 2; Layout.fillWidth: true; rowSpacing: 7; columnSpacing: 16
                Label { text: qsTr("File name") }
                Label { text: win.transferJob ? win.transferJob.name : ""; font.bold: true; elide: Text.ElideMiddle; Layout.fillWidth: true }
                Label { text: qsTr("Status") }
                Label { text: win.transferJob ? win.statusLabel(win.transferJob) : ""; color: "#20702b" }
                Label { text: qsTr("File size") }
                Label { text: win.transferJob ? win.bytes(win.transferJob.total) : "" }
                Label { text: qsTr("Downloaded") }
                Label { text: win.transferJob ? win.bytes(win.transferJob.received) : "" }
                Label { text: qsTr("Transfer rate") }
                Label { text: win.transferJob ? win.bytes(win.transferJob.speed)+"/s" : "" }
                Label { text: qsTr("Time left") }
                Label { text: win.transferJob ? win.duration(win.transferJob.eta) : "" }
                Label { text: qsTr("Resume capability") }
                Label { text: win.transferJob && win.transferJob.resumable ? qsTr("Yes") : qsTr("No") }
            }
            ProgressBar {
                id: overallProgress
                Layout.fillWidth: true; implicitHeight: 18; from: 0; to: 1
                value: win.transferJob && win.transferJob.total > 0 ? win.transferJob.received/win.transferJob.total : 0
                background: Rectangle { color: win.surface; border.color: win.edge }
                contentItem: Item { Rectangle { x: 2; y: 2; width: Math.max(0,parent.width-4)*overallProgress.position; height: parent.height-4; color: "#48a549" } }
            }
            Label { text: qsTr("Download status"); font.bold: true }
            Rectangle {
                Layout.fillWidth: true; Layout.preferredHeight: 28; color: win.surface; border.color: win.edge
                Row {
                    anchors.fill: parent; anchors.margins: 3; spacing: 2
                    Repeater { model: win.transferJob ? (win.transferJob.parts || []) : []
                        delegate: Rectangle {
                            required property var modelData
                            width: Math.max(1,(parent.width-Math.max(0,(win.transferJob.parts || []).length-1)*2)/Math.max(1,(win.transferJob.parts || []).length)); height: parent.height
                            color: "#f5f5f5"; border.color: "#c6c6c6"
                            Rectangle { width: parent.width*(modelData.total > 0 ? Math.min(1,modelData.received/modelData.total) : 0); height: parent.height; color: "#418ac5" }
                            Rectangle { width: 2; height: parent.height; color: "#d373a5" }
                        }
                    }
                }
            }
            Rectangle {
                Layout.fillWidth: true; Layout.preferredHeight: 135; color: win.surface; border.color: win.edge
                Flickable {
                    anchors.fill: parent; anchors.margins: 5
                    clip: true; contentWidth: width; contentHeight: connectionRows.implicitHeight
                    ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                    Column {
                    id: connectionRows; width: parent.width; spacing: 3
                    Row { spacing: 8
                        Label { text: qsTr("Connection"); width: 90; font.bold: true }
                        Label { text: qsTr("Downloaded"); width: 120; font.bold: true }
                        Label { text: qsTr("Information"); font.bold: true }
                    }
                    Repeater { model: win.transferJob ? (win.transferJob.parts || []) : []
                        delegate: Row {
                            required property var modelData
                            required property int index
                            spacing: 8
                            Label { text: String(index+1); width: 90 }
                            Label { text: win.bytes(modelData.received); width: 120 }
                            Label { text: modelData.total > 0 && modelData.received >= modelData.total ? qsTr("Complete") : win.transferJob && win.running(win.transferJob) ? qsTr("Receiving data...") : qsTr("Stopped") }
                        }
                    }
                    }
                }
                clip: true
            }
            Label { visible: win.transferJob && win.transferJob.error.length > 0; text: win.transferJob ? win.transferJob.error : ""; color: "#a82020"; wrapMode: Text.WordWrap; Layout.fillWidth: true }
            RowLayout { Layout.alignment: Qt.AlignRight
                ClassicButton { text: win.running(win.transferJob) ? qsTr("Pause") : qsTr("Resume"); enabled: !!win.transferJob && win.transferJob.status !== "complete"; onClicked: { if (win.running(win.transferJob)) win.controller.pause(win.transferId); else win.controller.resume(win.transferId) } }
                ClassicButton { text: qsTr("Cancel"); onClicked: { if (win.running(win.transferJob)) win.controller.pause(win.transferId); transferDialog.close() } }
                ClassicButton { text: qsTr("Close"); onClicked: transferDialog.close() }
            }
        }
    }
    Dialog {
        id: removeDialog; objectName: "removeDialog"; title: qsTr("Delete download"); modal: true; width: 440; anchors.centerIn: parent
        standardButtons: Dialog.Ok | Dialog.Cancel
        Label { text: qsTr("Remove the selected download from the list? Partial data will be deleted. Completed files will be kept."); wrapMode: Text.WordWrap; width: parent.width }
        onAccepted: { win.controller.remove(win.selectedId); win.selectedId = "" }
    }
    Dialog {
        id: optionsDialog; objectName: "optionsDialog"; title: qsTr("Internet Download Manager Configuration"); modal: true; width: 750; anchors.centerIn: parent
        onOpened: { win.lastError=""; win.loadOptions() }
        ColumnLayout {
            anchors.fill: parent; spacing: 10
            TabBar { id: optionsTabs; Layout.fillWidth: true
                TabButton { text: qsTr("General") }
                TabButton { text: qsTr("Connection") }
                TabButton { text: qsTr("File types") }
                TabButton { text: qsTr("Downloads") }
                TabButton { text: qsTr("Sites") }
                TabButton { text: qsTr("Actions") }
                TabButton { text: qsTr("Browser") }
            }
            StackLayout {
                currentIndex: optionsTabs.currentIndex; Layout.fillWidth: true; implicitHeight: 310
                ColumnLayout {
                    spacing: 6
                    Label { text: qsTr("General settings"); font.bold: true }
                    CheckBox { id: darkCheck; objectName: "darkModeToggle"; text: qsTr("Dark mode"); onToggled: win.darkMode = checked }
                    CheckBox { id: clipboardCheck; text: qsTr("Monitor clipboard for download links") }
                    CheckBox { id: notificationsCheck; text: qsTr("Show completion notifications") }
                    CheckBox { id: soundCheck; text: qsTr("Play a sound when a download finishes") }
                    Item { Layout.fillHeight: true }
                    Label { text: qsTr("Language follows your desktop's language setting. English is used when a translation is unavailable."); wrapMode: Text.WordWrap; Layout.fillWidth: true; color: win.ink }
                }
                ColumnLayout {
                    GridLayout {
                        columns: 2; rowSpacing: 10; Layout.fillWidth: true
                        Label { text: qsTr("Default maximum connections") }
                        SpinBox { id: defaultsConnections; from: 1; to: 16; editable: true }
                        Label { text: qsTr("Simultaneous downloads") }
                        SpinBox { id: concurrentInput; objectName: "concurrentInput"; from: 1; to: 32; editable: true }
                        Label { text: qsTr("Retries on failure") }
                        SpinBox { id: retriesInput; from: 0; to: 20; editable: true }
                        Label { text: qsTr("Retry delay (seconds)") }
                        SpinBox { id: retryDelayInput; from: 1; to: 3600; editable: true }
                        Label { text: qsTr("Connection timeout (seconds)") }
                        SpinBox { id: timeoutInput; from: 1; to: 3600; editable: true }
                        Label { text: qsTr("Speed limit (bytes/s, 0 = unlimited)") }
                        ClassicField { id: rateInput; objectName: "rateInput"; Layout.fillWidth: true; inputMethodHints: Qt.ImhDigitsOnly }
                        Label { text: qsTr("Proxy URL") }
                        ClassicField { id: proxyInput; Layout.fillWidth: true; placeholderText: "http://proxy:8080" }
                    }
                    Item { Layout.fillHeight: true }
                }
                ColumnLayout {
                    Label { text: qsTr("Download file types"); font.bold: true }
                    Label { text: qsTr("File-type filter"); }
                    ClassicField { id: typeFilterInput; Layout.fillWidth: true; placeholderText: "ZIP PDF MP4 EXE" }
                    Label { text: qsTr("Leave empty to allow all file types. Browser interception preferences are set in the extension."); wrapMode: Text.WordWrap; Layout.fillWidth: true }
                    Item { Layout.fillHeight: true }
                }
                ColumnLayout {
                    Label { text: qsTr("Save to") }
                    RowLayout { Layout.fillWidth: true
                        ClassicField { id: defaultsDirectory; objectName: "defaultsDirectory"; Layout.fillWidth: true }
                        ClassicButton { text: qsTr("Browse..."); onClicked: picker.open() }
                    }
                    Label { text: qsTr("Speed schedule"); font.bold: true }
                    RowLayout {
                        ClassicField { id: speedStart; placeholderText: "09:00"; Layout.preferredWidth: 90 }
                        Label { text: qsTr("to") }
                        ClassicField { id: speedEnd; placeholderText: "17:00"; Layout.preferredWidth: 90 }
                        ClassicField { id: scheduledRate; placeholderText: qsTr("bytes/s"); Layout.fillWidth: true }
                        ClassicButton { text: qsTr("Add schedule"); enabled: win.automationAvailable; onClicked: {
                            let schedules=(win.preferences.speedSchedule || []).slice()
                            schedules.push({start:speedStart.text,end:speedEnd.text,rateLimit:Number(scheduledRate.text)})
                            if (!automation.configure({speedSchedule:schedules})) win.lastError=automation.error
                        } }
                    }
                    Label { text: qsTr("%1 speed schedules configured").arg((win.preferences.speedSchedule || []).length) }
                    ClassicButton { text: qsTr("Clear speed schedules"); enabled: win.automationAvailable; onClicked: { if (!automation.configure({speedSchedule:[]})) win.lastError=automation.error } }
                    Item { Layout.fillHeight: true }
                }
                ColumnLayout {
                    GridLayout {
                        columns: 2; Layout.fillWidth: true
                        Label { text: qsTr("Site hostname") }
                        ClassicField { id: siteHost; Layout.fillWidth: true; placeholderText: "example.com" }
                        Label { text: qsTr("User agent") }
                        ClassicField { id: siteAgent; Layout.fillWidth: true }
                        Label { text: qsTr("Referer") }
                        ClassicField { id: siteReferer; Layout.fillWidth: true }
                        Label { text: qsTr("Save to") }
                        ClassicField { id: siteDirectory; Layout.fillWidth: true }
                        Label { text: qsTr("Connections") }
                        SpinBox { id: siteConnections; from: 1; to: 16; value: 4 }
                        Label { text: qsTr("Proxy URL") }
                        ClassicField { id: siteProxy; Layout.fillWidth: true }
                        Label { text: qsTr("Login") }
                        ClassicField { id: siteLogin; Layout.fillWidth: true; maximumLength: 512 }
                        Label { text: qsTr("Password") }
                        ClassicField { id: sitePassword; Layout.fillWidth: true; maximumLength: 4096; echoMode: TextInput.Password }
                    }
                    RowLayout {
                        ClassicButton { text: qsTr("Load site settings"); enabled: win.automationAvailable; onClicked: {
                            let site=(win.preferences.siteSettings || {})[siteHost.text.toLowerCase()] || {}
                            siteLogin.text=site.username || ""; sitePassword.text=site.password || ""
                            siteDirectory.text=site.directory || ""; siteConnections.value=site.connections || 4; siteProxy.text=site.proxy || ""
                            siteAgent.text=(site.headers || {})["User-Agent"] || ""; siteReferer.text=(site.headers || {})["Referer"] || ""
                        } }
                        ClassicButton { text: qsTr("Save site settings"); enabled: win.automationAvailable; onClicked: {
                            let sites=Object.assign({},win.preferences.siteSettings || {}), headers={}
                            if (siteAgent.text) headers["User-Agent"]=siteAgent.text
                            if (siteReferer.text) headers["Referer"]=siteReferer.text
                            let site={connections:siteConnections.value,headers:headers}
                            if (siteDirectory.text) site.directory=siteDirectory.text
                            if (siteProxy.text) site.proxy=siteProxy.text
                            if (siteLogin.text) { site.username=siteLogin.text; site.password=sitePassword.text }
                            sites[siteHost.text.trim().toLowerCase()]=site
                            if (!automation.configure({siteSettings:sites})) win.lastError=automation.error
                        } }
                    }
                    Item { Layout.fillHeight: true }
                }
                ColumnLayout {
                    spacing: 5
                    CheckBox { id: antivirusCheck; text: qsTr("Scan completed files for viruses") }
                    RowLayout { Label { text: qsTr("Scanner program") } ClassicField { id: scannerProgram; Layout.fillWidth: true; placeholderText: "/usr/bin/clamdscan" } }
                    Label { text: qsTr("Scanner arguments, one per line. Use %f for the downloaded file.") }
                    TextArea { id: scannerArguments; Layout.fillWidth: true; Layout.preferredHeight: 65; selectByMouse: true; placeholderText: "--\n%f" }
                    RowLayout { Label { text: qsTr("After the queue finishes") } ComboBox { id: completionChoice; model: [qsTr("Do nothing"),qsTr("Shut down computer"),qsTr("Disconnect network")] } }
                    RowLayout { Label { text: qsTr("Confirmation delay (seconds)") } SpinBox { id: completionDelayInput; from: 10; to: 3600 } }
                    Label { text: qsTr("Shutdown and network disconnect are opt-in. A pending action can be cancelled from the notification bar."); wrapMode: Text.WordWrap; Layout.fillWidth: true }
                }
                ColumnLayout {
                    Label { text: win.bridgeAvailable ? qsTr("Native browser bridge is ready.") : qsTr("Native browser bridge is not connected."); font.bold: true; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                    Label { text: qsTr("Install the native host, then load the extension in your browser. Use Download with IDM or the video panel to send downloads here."); wrapMode: Text.WordWrap; Layout.fillWidth: true }
                    RowLayout {
                        ClassicButton { text: qsTr("Install browser integration"); enabled: typeof browserBridge !== "undefined"; onClicked: { let path = browserBridge.install(); win.installMessage = path ? qsTr("Extension folder: ")+path : "" } }
                        ClassicButton { text: qsTr("Open extension folder"); enabled: typeof browserBridge !== "undefined" && browserBridge.extensionDirectory.length > 0; onClicked: Qt.openUrlExternally("file://"+browserBridge.extensionDirectory) }
                    }
                    Label { id: integrationMessage; text: win.bridgeError || win.installMessage; color: "#bd4141"; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                    Label { text: qsTr("Chromium: open Extensions, enable Developer mode, choose Load unpacked, and select the extension folder. Firefox: follow the included browser setup instructions."); wrapMode: Text.WordWrap; Layout.fillWidth: true }
                    Label { text: qsTr("For HLS, DASH and video pages, enable Video / audio download. yt-dlp and ffmpeg must be installed."); wrapMode: Text.WordWrap; Layout.fillWidth: true }
                    Item { Layout.fillHeight: true }
                }
            }
            Label { text: win.lastError; visible: text.length > 0; color: "#bd4141"; wrapMode: Text.WordWrap; Layout.fillWidth: true }
            RowLayout { Layout.alignment: Qt.AlignRight
                ClassicButton { objectName: "saveOptionsButton"; text: qsTr("OK"); onClicked: win.saveOptions() }
                ClassicButton { text: qsTr("Cancel"); onClicked: { win.loadPreferences(); optionsDialog.close() } }
            }
        }
    }
    Dialog {
        id: schedulerDialog; objectName: "schedulerDialog"; title: qsTr("Scheduler"); modal: true; width: 650; anchors.centerIn: parent
        onOpened: { win.lastError=""; win.loadQueue() }
        ColumnLayout {
            anchors.fill: parent; spacing: 10
            GridLayout {
                columns: 2; Layout.fillWidth: true; rowSpacing: 8
                Label { text: qsTr("Queue") }
                ComboBox { id: queueName; objectName: "queueName"; editable: true; Layout.fillWidth: true; model: win.automationAvailable ? automation.queues.map(q => q.name) : ["Main download queue"]; onActivated: win.loadQueue() }
                Label { text: qsTr("Download files at the same time") }
                SpinBox { id: queueConcurrent; objectName: "queueConcurrent"; from: 1; to: 32 }
                Label { text: qsTr("Start date and time") }
                ClassicField { id: queueStart; objectName: "queueStart"; Layout.fillWidth: true; placeholderText: "YYYY-MM-DD HH:mm" }
                Label { text: qsTr("Stop date and time") }
                ClassicField { id: queueStop; objectName: "queueStop"; Layout.fillWidth: true }
                Label { text: qsTr("Repeat every (minutes, 0 = once)") }
                SpinBox { id: queueRepeat; from: 0; to: 525600; editable: true }
            }
            Label { text: win.lastError; visible: text.length > 0; color: "#bd4141"; wrapMode: Text.WordWrap; Layout.fillWidth: true }
            RowLayout { Layout.alignment: Qt.AlignRight
                ClassicButton { objectName: "saveQueueButton"; text: qsTr("Save schedule"); onClicked: win.saveQueue(false) }
                ClassicButton { text: qsTr("Start now"); onClicked: win.saveQueue(true) }
                ClassicButton { text: qsTr("Stop"); onClicked: { automation.stopQueue(queueName.editText); schedulerDialog.close() } }
                ClassicButton { text: qsTr("Close"); onClicked: schedulerDialog.close() }
            }
        }
    }
    Dialog {
        id: policyDialog; objectName: "policyDialog"; title: qsTr("Queue and priority"); width: 480; modal: true; anchors.centerIn: parent
        onOpened: {
            let policy = win.automationAvailable ? automation.jobPolicies[win.selectedId] || {} : {}
            jobQueue.editText = policy.queue || "Main download queue"
            jobPriority.currentIndex = policy.priority > 0 ? 1 : policy.priority < 0 ? 2 : 0
        }
        ColumnLayout { anchors.fill: parent; spacing: 10
            ComboBox { id: jobQueue; objectName: "jobQueue"; editable: true; Layout.fillWidth: true; model: win.automationAvailable ? automation.queues.map(q => q.name) : ["Main download queue"] }
            ComboBox { id: jobPriority; objectName: "jobPriority"; model: [qsTr("Normal priority"),qsTr("High priority"),qsTr("Low priority")] }
            Label { text: win.lastError; visible: text.length > 0; color: "#bd4141"; wrapMode: Text.WordWrap; Layout.fillWidth: true }
            ClassicButton { objectName: "savePolicyButton"; text: qsTr("OK"); onClicked: { if (automation.setJobPolicy(win.selectedId,{queue:jobQueue.editText,priority:[0,10,-10][jobPriority.currentIndex]})) policyDialog.close(); else win.lastError=automation.error } }
        }
    }
    Dialog {
        id: listDialog; objectName: "listDialog"; property bool importing: true
        title: importing ? qsTr("Import downloads") : qsTr("Export downloads"); modal: true; width: 590; anchors.centerIn: parent
        ColumnLayout { anchors.fill: parent; spacing: 10
            Label { text: listDialog.importing ? qsTr("Choose a JSON download list or a text file containing one URL per line.") : qsTr("Export filenames, URLs, descriptions and queue policies. Cookies and credentials are excluded."); wrapMode: Text.WordWrap; Layout.fillWidth: true }
            ClassicField { id: listPath; objectName: "listPath"; Layout.fillWidth: true; placeholderText: "/home/user/Downloads/download-list.json" }
            Label { text: win.lastError; visible: text.length > 0; color: "#bd4141"; wrapMode: Text.WordWrap; Layout.fillWidth: true }
            RowLayout { Layout.alignment: Qt.AlignRight
                ClassicButton { objectName: "listSubmitButton"; text: listDialog.importing ? qsTr("Import") : qsTr("Export"); onClicked: { if (listDialog.importing ? automation.importList(listPath.text) : automation.exportList(listPath.text)) listDialog.close(); else win.lastError=automation.error } }
                ClassicButton { text: qsTr("Cancel"); onClicked: listDialog.close() }
            }
        }
    }
    Dialog {
        id: editDialog; objectName: "editDialog"; property bool renaming: false
        title: renaming ? qsTr("Rename file") : qsTr("Refresh download address"); width: 590; modal: true; anchors.centerIn: parent
        onOpened: { win.lastError=""; editValue.text=win.selected ? (renaming ? win.selected.name : win.selected.url) : "" }
        ColumnLayout { anchors.fill: parent; spacing: 10
            ClassicField { id: editValue; objectName: "editValue"; Layout.fillWidth: true }
            Label { text: win.lastError; visible: text.length > 0; color: "#bd4141"; wrapMode: Text.WordWrap; Layout.fillWidth: true }
            RowLayout { Layout.alignment: Qt.AlignRight
                ClassicButton { objectName: "saveEditButton"; text: qsTr("OK"); onClicked: { if (editDialog.renaming ? automation.rename(win.selectedId,editValue.text) : automation.refresh(win.selectedId,editValue.text)) editDialog.close(); else win.lastError=automation.error } }
                ClassicButton { text: qsTr("Cancel"); onClicked: editDialog.close() }
            }
        }
    }
    Dialog {
        id: grabberDialog; objectName: "grabberDialog"; title: qsTr("Site Grabber"); modal: true; width: 650; anchors.centerIn: parent
        ColumnLayout { anchors.fill: parent; spacing: 10
            GridLayout {
                columns: 2; Layout.fillWidth: true
                Label { text: qsTr("Website URL") }
                ClassicField { id: grabberUrl; objectName: "grabberUrl"; Layout.fillWidth: true }
                Label { text: qsTr("Save to") }
                ClassicField { id: grabberDirectory; objectName: "grabberDirectory"; Layout.fillWidth: true; text: win.defaultDirectory }
                Label { text: qsTr("Link depth") }
                SpinBox { id: grabberDepth; from: 0; to: 5; value: 1 }
            }
            CheckBox { id: synchronizeCheck; text: qsTr("Synchronize this website periodically") }
            RowLayout { enabled: synchronizeCheck.checked; Label { text: qsTr("Interval (minutes)") } SpinBox { id: synchronizeInterval; from: 1; to: 525600; value: 60; editable: true } }
            Label { text: win.automationAvailable ? String(automation.grabber.status || "") + "  " + win.bytes(automation.grabber.received || 0) : "" }
            Label { text: win.automationAvailable ? automation.grabber.error || automation.error : ""; color: "#bd4141"; wrapMode: Text.WordWrap; Layout.fillWidth: true }
            RowLayout { Layout.alignment: Qt.AlignRight
                ClassicButton { objectName: "grabberStartButton"; text: qsTr("Start"); onClicked: {
                    if (!automation.mirror(grabberUrl.text,grabberDirectory.text,grabberDepth.value)) return
                    if (synchronizeCheck.checked) {
                        let jobs=(win.preferences.syncJobs || []).slice().filter(j => j.url !== grabberUrl.text || j.directory !== grabberDirectory.text)
                        jobs.push({url:grabberUrl.text,directory:grabberDirectory.text,depth:grabberDepth.value,intervalMinutes:synchronizeInterval.value,nextRun:new Date(Date.now()+synchronizeInterval.value*60000).toISOString()})
                        if (!automation.configure({syncJobs:jobs})) win.lastError=automation.error
                    }
                } }
                ClassicButton { text: qsTr("Stop"); onClicked: automation.cancelMirror() }
                ClassicButton { text: qsTr("Close"); onClicked: grabberDialog.close() }
            }
        }
    }
    Popup {
        visible: win.automationAvailable && automation.completionNotice.length > 0
        anchors.centerIn: parent; modal: false; width: 500; closePolicy: Popup.NoAutoClose
        contentItem: RowLayout {
            Label { text: win.automationAvailable ? automation.completionNotice : ""; wrapMode: Text.WordWrap; Layout.fillWidth: true }
            ClassicButton { text: qsTr("Cancel action"); onClicked: automation.cancelCompletionAction() }
        }
    }
    Dialog {
        id: shortcutsDialog; title: qsTr("Keyboard shortcuts"); width: 430; anchors.centerIn: parent; modal: true; standardButtons: Dialog.Close
        Label { text: qsTr("Ctrl+N    Add URL\nEnter       Download progress\nSpace      Stop or resume selected\nDelete     Remove selected download\n↑ / ↓       Select download\nF1            Keyboard shortcuts\nCtrl+Q    Exit"); lineHeight: 1.7 }
    }
    Dialog {
        id: aboutDialog; title: qsTr("About"); width: 430; anchors.centerIn: parent; modal: true; standardButtons: Dialog.Close
        Label { width: parent.width; wrapMode: Text.WordWrap; text: qsTr("IDM Native\nIndependent native download manager with an IDM-style interface. It is not an official Tonec product.\nHTTP / HTTPS, browser handoff and video / audio downloads.") }
    }
}
