import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

ApplicationWindow {
    id: win
    width: 1060; height: 720
    minimumWidth: 760; minimumHeight: 520
    visible: true
    title: qsTr("IDM Native")
    color: "#111518"
    Material.theme: Material.Dark
    Material.accent: theme.accent
    Material.roundedScale: theme.radius
    property string filter: "all"
    property string selectedId: ""
    property bool helpVisible: false
    property int activeCount: backend.downloads.filter(d => d.status === "downloading" || d.status === "probing").length
    function bytes(value) {
        let language = translator.language
        if (value < 0) return qsTr("Unknown size")
        let units = ["B", "KB", "MB", "GB", "TB"]
        let n = Number(value), i = 0
        while (n >= 1024 && i < units.length - 1) { n /= 1024; i++ }
        return n.toLocaleString(Qt.locale(), 'f', i ? 1 : 0) + " " + units[i]
    }
    function statusLabel(status) {
        return ({"probing": qsTr("Connecting"), "downloading": qsTr("Downloading"), "paused": qsTr("Paused"), "complete": qsTr("Complete"), "error": qsTr("Needs attention")})[status] || status
    }
    function toggleSelected() {
        for (const d of backend.downloads) if (d.id === selectedId) {
            if (d.status === "downloading" || d.status === "probing") backend.pause(d.id)
            else if (d.status !== "complete") backend.resume(d.id)
        }
    }
    onClosing: backend.pauseAll()
    Shortcut { sequence: "Ctrl+N"; onActivated: addDialog.open() }
    Shortcut { sequence: "?"; enabled: !addDialog.visible; onActivated: helpVisible = !helpVisible }
    Shortcut { sequence: "Space"; enabled: !addDialog.visible; onActivated: win.toggleSelected() }
    Shortcut { sequence: "Q"; enabled: !addDialog.visible; onActivated: win.close() }
    Shortcut { sequence: "Ctrl+Q"; onActivated: win.close() }

    component ActionButton: Button {
        id: control
        background: Rectangle {
            implicitHeight: 40
            implicitWidth: 100
            radius: theme.radius
            color: control.down ? "#3a454b" : control.highlighted ? theme.accent : control.hovered ? "#30393e" : "transparent"
            border.width: control.visualFocus ? 2 : 0
            border.color: theme.accent
            opacity: control.enabled ? 1 : 0.4
        }
        contentItem: Text {
            text: control.text
            font: control.font
            color: control.highlighted ? theme.accentForeground : "#e5ecef"
            opacity: control.enabled ? 1 : 0.4
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }
    }

    Item {
        anchors.fill: parent
        LayoutMirroring.enabled: Qt.application.layoutDirection === Qt.RightToLeft
        LayoutMirroring.childrenInherit: true
        RowLayout {
            anchors.fill: parent; spacing: 0
            Rectangle {
                Layout.preferredWidth: 210; Layout.fillHeight: true; color: "#191e22"
                ColumnLayout {
                    anchors.fill: parent; anchors.margins: 22; spacing: 12
                    Label { text: "↓"; color: theme.accent; font.pixelSize: 44 }
                    Label { text: qsTr("IDM Native"); font.pixelSize: 23; font.weight: Font.DemiBold }
                    Label { text: qsTr("Your files, on the way."); color: "#8c989e"; font.pixelSize: 12 }
                    Item { Layout.preferredHeight: 24 }
                    Repeater {
                        model: [{key:"all",label:qsTr("All downloads")},{key:"active",label:qsTr("In progress")},{key:"complete",label:qsTr("Completed")}]
                        delegate: ActionButton {
                            required property var modelData
                            Layout.fillWidth: true; text: modelData.label
                            flat: true; highlighted: win.filter === modelData.key
                            onClicked: win.filter = modelData.key
                        }
                    }
                    Item { Layout.fillHeight: true }
                    Label { text: qsTr("Saved automatically"); color: "#b4c0c4"; font.pixelSize: 12 }
                    Label { text: qsTr("Close anytime. Resume later."); color: "#8c989e"; font.pixelSize: 11; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                    ActionButton { text: qsTr("Shortcuts") + "  ?"; flat: true; Layout.fillWidth: true; onClicked: win.helpVisible = true }
                }
            }
            ColumnLayout {
                Layout.fillWidth: true; Layout.fillHeight: true; Layout.margins: 30; spacing: 18
                RowLayout {
                    Layout.fillWidth: true
                    ColumnLayout {
                        spacing: 5
                        Label { text: qsTr("Downloads"); font.pixelSize: 32; font.weight: Font.DemiBold }
                        Label { text: activeCount ? qsTr("%1 active transfers").arg(activeCount) : qsTr("Ready when you are."); color: "#8c989e" }
                    }
                    Item { Layout.fillWidth: true }
                    ActionButton { text: qsTr("Pause all"); enabled: win.activeCount > 0; flat: true; onClicked: backend.pauseAll() }
                    ActionButton { text: "+  " + qsTr("Add download"); highlighted: true; onClicked: addDialog.open() }
                }
                Rectangle { Layout.fillWidth: true; height: 1; color: "#30393e" }
                Label {
                    visible: backend.error.length > 0; text: backend.error; color: "#ffb4ab"
                    wrapMode: Text.WordWrap; Layout.fillWidth: true
                }
                Item {
                    Layout.fillHeight: true; Layout.fillWidth: true
                    ColumnLayout {
                        anchors.centerIn: parent; spacing: 16
                        visible: backend.downloads.length === 0
                        Label { text: "↓"; color: theme.accent; font.pixelSize: 64; Layout.alignment: Qt.AlignHCenter }
                        Label { text: qsTr("Make room for what's next."); font.pixelSize: 24; Layout.alignment: Qt.AlignHCenter }
                        Label { text: qsTr("Paste a link. We'll take care of the download."); color: "#8c989e"; Layout.alignment: Qt.AlignHCenter }
                        ActionButton { text: qsTr("Add your first download"); highlighted: true; Layout.alignment: Qt.AlignHCenter; onClicked: addDialog.open() }
                    }
                    ListView {
                        id: list
                        anchors.fill: parent; spacing: 10; clip: true
                        model: backend.downloads.filter(d => win.filter === "all" || (win.filter === "complete" ? d.status === "complete" : d.status !== "complete"))
                        ScrollBar.vertical: ScrollBar {}
                        delegate: Rectangle {
                            required property var modelData
                            width: list.width; height: details.implicitHeight + 34
                            radius: theme.radius; color: win.selectedId === modelData.id ? "#263139" : "#1b2328"
                            border.color: win.selectedId === modelData.id ? theme.accent : "#30393e"
                            TapHandler { onTapped: win.selectedId = modelData.id }
                            ColumnLayout {
                                id: details
                                anchors { left: parent.left; right: parent.right; top: parent.top; margins: 17 }
                                spacing: 9
                                RowLayout {
                                    Layout.fillWidth: true
                                    Label { text: modelData.name; font.pixelSize: 15; font.weight: Font.DemiBold; elide: Text.ElideMiddle; Layout.fillWidth: true }
                                    Label { text: win.statusLabel(modelData.status); color: modelData.status === "error" ? "#ffb4ab" : modelData.status === "complete" ? "#a9d7b2" : "#9dabb2"; font.pixelSize: 12 }
                                }
                                ProgressBar { Layout.fillWidth: true; value: modelData.total > 0 ? modelData.received / modelData.total : 0; indeterminate: modelData.status === "probing" || (modelData.status === "downloading" && modelData.total < 0) }
                                RowLayout {
                                    Layout.fillWidth: true
                                    Label {
                                        text: win.bytes(modelData.received) + " / " + win.bytes(modelData.total) + (modelData.status === "downloading" ? "    ·    " + win.bytes(modelData.speed) + "/s" : "")
                                        color: "#9dabb2"; font.pixelSize: 12; Layout.fillWidth: true
                                    }
                                    ActionButton {
                                        text: modelData.status === "downloading" || modelData.status === "probing" ? qsTr("Pause") : qsTr("Resume")
                                        visible: modelData.status !== "complete"; flat: true
                                        onClicked: { win.selectedId = modelData.id; win.toggleSelected() }
                                    }
                                    ActionButton { text: qsTr("Show file"); visible: modelData.status === "complete"; flat: true; onClicked: Qt.openUrlExternally("file://" + modelData.path.substring(0, modelData.path.lastIndexOf("/"))) }
                                    ActionButton { text: "×"; flat: true; Accessible.name: qsTr("Remove from list"); onClicked: { removeDialog.downloadId = modelData.id; removeDialog.open() } }
                                }
                                Label { visible: modelData.error.length > 0; text: modelData.error; color: "#ffb4ab"; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                            }
                        }
                    }
                }
                RowLayout {
                    Label { text: qsTr("Downloads folder"); color: "#8c989e"; font.pixelSize: 11; elide: Text.ElideMiddle; Layout.fillWidth: true }
                    Label { text: qsTr("HTTP / HTTPS") + "   ·   " + qsTr("Up to 16 connections"); color: "#8c989e"; font.pixelSize: 11 }
                }
            }
        }
    }
    Dialog {
        id: addDialog
        title: qsTr("Add download")
        anchors.centerIn: parent; width: Math.min(580, win.width - 48); modal: true
        standardButtons: Dialog.Ok | Dialog.Cancel
        onOpened: { urlInput.forceActiveFocus(); validation.text = "" }
        onAccepted: {
            const id = backend.add(urlInput.text.trim(), directoryInput.text, connections.value)
            if (!id) { validation.text = backend.error; open() }
            else { win.selectedId = id; urlInput.clear() }
        }
        ColumnLayout {
            anchors.fill: parent; spacing: 14
            TextField { id: urlInput; objectName: "urlInput"; placeholderText: qsTr("Download URL"); Layout.fillWidth: true; selectByMouse: true; onAccepted: addDialog.accept() }
            RowLayout {
                TextField { id: directoryInput; text: backend.downloadDirectory; placeholderText: qsTr("Save to folder"); Layout.fillWidth: true; selectByMouse: true }
                ActionButton { text: qsTr("Browse"); onClicked: picker.open() }
            }
            RowLayout {
                Label { text: qsTr("Connections"); Layout.fillWidth: true }
                SpinBox { id: connections; from: 1; to: 16; value: 4 }
            }
            Label { text: qsTr("Parallel transfers require server range support. Existing files are kept safe."); color: "#9dabb2"; wrapMode: Text.WordWrap; Layout.fillWidth: true }
            Label { id: validation; color: "#ffb4ab"; wrapMode: Text.WordWrap; Layout.fillWidth: true }
        }
    }
    Connections {
        target: picker
        function onSelected(directory) { directoryInput.text = directory }
        function onError(message) { validation.text = message }
    }
    Dialog {
        id: removeDialog; width: Math.min(520, win.width - 48); property string downloadId: ""
        anchors.centerIn: parent; title: qsTr("Remove download?"); modal: true
        standardButtons: Dialog.Ok | Dialog.Cancel
        Label { text: qsTr("Partial data will be removed. Completed files are kept."); wrapMode: Text.WordWrap }
        onAccepted: backend.remove(downloadId)
    }
    Dialog {
        width: Math.min(520, win.width - 48); visible: win.helpVisible; anchors.centerIn: parent; title: qsTr("Keyboard shortcuts"); modal: true
        standardButtons: Dialog.Close
        onClosed: win.helpVisible = false
        Label { text: qsTr("Ctrl+N    Add download\nSpace     Pause or resume selected\n?             Show shortcuts\nQ             Save queue and quit"); lineHeight: 1.6 }
    }
}
