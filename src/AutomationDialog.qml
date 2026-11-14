import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Dialog {
    id: dialog
    objectName: "automationProjectsDialog"
    property var controller
    property var appWindow
    property string failure: ""
    readonly property var preferences: controller ? controller.preferences : ({})
    readonly property var entries: projectTabs.currentIndex === 1 ? preferences.fileSyncJobs || [] : preferences.syncJobs || []
    palette: appWindow ? appWindow.palette : undefined
    modal: true
    title: qsTr("Synchronization and quotas")
    width: Math.min(700, appWindow ? appWindow.width - 24 : 700)
    height: Math.min(640, appWindow ? appWindow.height - 36 : 640)
    standardButtons: Dialog.Close
    onOpened: {
        failure = ""
        quotaMB.value = preferences.quotaMB || 0
        quotaPeriod.value = preferences.quotaPeriodMinutes || 1440
        loadProject(-1)
    }
    function patterns(text) { return text.split(",").map(p => p.trim()).filter(p => p.length > 0) }
    function loadProject(index) {
        if (!projectUrl) return
        const job = entries[index] || {}
        projectChoice.currentIndex = index + 1
        projectUrl.text = job.url || ""
        projectDirectory.text = job.directory || preferences.directory || ""
        projectFilename.text = job.filename || ""
        projectInterval.value = job.intervalMinutes === undefined ? 60 : job.intervalMinutes
        projectDepth.value = job.depth === undefined ? 1 : job.depth
        projectStart.text = job.startAt || ""
        projectStop.text = job.stopAt || ""
        projectEnabled.checked = job.enabled !== false
        projectInclude.text = (job.include || []).join(", ")
        projectExclude.text = (job.exclude || []).join(", ")
    }
    function saveProject() {
        const files = projectTabs.currentIndex === 1
        const job = {url: projectUrl.text.trim(), directory: projectDirectory.text.trim(), intervalMinutes: projectInterval.value,
                     enabled: projectEnabled.checked, startAt: projectStart.text.trim(), stopAt: projectStop.text.trim()}
        if (files) job.filename = projectFilename.text.trim()
        else { job.depth = projectDepth.value; job.include = patterns(projectInclude.text); job.exclude = patterns(projectExclude.text) }
        const jobs = entries.slice(), index = projectChoice.currentIndex - 1
        if (index < 0) jobs.push(job)
        else jobs[index] = job
        const update = {}; update[files ? "fileSyncJobs" : "syncJobs"] = jobs
        if (!controller.configure(update)) failure = controller.error
        else { failure = ""; projectChoice.currentIndex = index < 0 ? jobs.length : index + 1 }
    }
    function removeProject() {
        const jobs = entries.slice(), index = projectChoice.currentIndex - 1
        if (index < 0) return
        jobs.splice(index, 1)
        const update = {}; update[projectTabs.currentIndex === 1 ? "fileSyncJobs" : "syncJobs"] = jobs
        if (!controller.configure(update)) failure = controller.error
        else { failure = ""; loadProject(-1) }
    }
    component ProjectTab: TabButton {
        id: tabControl
        contentItem: Text {
            text: tabControl.text
            color: dialog.appWindow.ink
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            font: tabControl.font
        }
        background: Rectangle {
            color: tabControl.checked ? dialog.appWindow.selection : dialog.appWindow.panel
            border.color: tabControl.activeFocus ? dialog.appWindow.accent : dialog.appWindow.edge
            radius: 4
        }
    }
    ColumnLayout {
        anchors.fill: parent
        spacing: 12
        TabBar {
            id: projectTabs; objectName: "automationProjectTabs"; Layout.fillWidth: true
            onCurrentIndexChanged: dialog.loadProject(-1)
            ProjectTab { text: qsTr("Quota") }
            ProjectTab { text: qsTr("File synchronization") }
            ProjectTab { text: qsTr("Website projects") }
        }
        ScrollView {
            id: projectScroll
            Layout.fillWidth: true; Layout.fillHeight: true; clip: true
            contentWidth: availableWidth
            ColumnLayout {
                width: projectScroll.availableWidth; spacing: 12
                ColumnLayout {
                    visible: projectTabs.currentIndex === 0; Layout.fillWidth: true; spacing: 12
                    Label { text: qsTr("Pause transfers after a data allowance and resume next period."); wrapMode: Text.WordWrap; Layout.fillWidth: true }
                    RowLayout {
                        Label { text: qsTr("Allowance (MiB, 0 disables)") }
                        IdmSpinBox { id: quotaMB; objectName: "quotaMB"; appWindow: dialog.appWindow; from: 0; to: 1000000000; editable: true }
                    }
                    RowLayout {
                        Label { text: qsTr("Period (minutes)") }
                        IdmSpinBox { id: quotaPeriod; objectName: "quotaPeriodMinutes"; appWindow: dialog.appWindow; from: 1; to: 525600; editable: true; value: 1440 }
                    }
                    Label {
                        Layout.fillWidth: true; wrapMode: Text.WordWrap
                        text: dialog.controller ? qsTr("Received this period: %1 MiB. %2").arg((Number(dialog.controller.quota.usedBytes || 0) / 1048576).toFixed(2)).arg(dialog.controller.quota.paused ? qsTr("Waiting until %1").arg(dialog.controller.quota.resumeAt) : qsTr("Transfers allowed")) : ""
                    }
                    Label { text: qsTr("Accounting uses received file bytes. Buffered traffic and retries can exceed this allowance."); wrapMode: Text.WordWrap; Layout.fillWidth: true }
                    Button {
                        objectName: "saveQuotaButton"; text: qsTr("Save quota")
                        onClicked: {
                            if (!dialog.controller.configure({quotaMB: quotaMB.value, quotaPeriodMinutes: quotaPeriod.value})) dialog.failure = dialog.controller.error
                            else dialog.failure = ""
                        }
                    }
                }
                ColumnLayout {
                    visible: projectTabs.currentIndex !== 0; Layout.fillWidth: true; spacing: 10
                    IdmComboBox {
                        id: projectChoice; objectName: "syncProjectChoice"; appWindow: dialog.appWindow; Layout.fillWidth: true
                        model: [qsTr("New project")].concat(dialog.entries.map(j => j.filename || j.url))
                        onActivated: dialog.loadProject(currentIndex - 1)
                    }
                    GridLayout {
                        columns: 2; columnSpacing: 12; rowSpacing: 10; Layout.fillWidth: true
                        Label { text: qsTr("URL") }
                        TextField { id: projectUrl; objectName: "syncProjectUrl"; Layout.fillWidth: true; placeholderText: "https://example.com/file.zip"; selectByMouse: true }
                        Label { text: qsTr("Save to") }
                        TextField { id: projectDirectory; objectName: "syncProjectDirectory"; Layout.fillWidth: true; selectByMouse: true }
                        Label { text: qsTr("Filename"); visible: projectTabs.currentIndex === 1 }
                        TextField { id: projectFilename; objectName: "syncProjectFilename"; Layout.fillWidth: true; visible: projectTabs.currentIndex === 1; selectByMouse: true }
                        Label { text: qsTr("Link depth"); visible: projectTabs.currentIndex === 2 }
                        IdmSpinBox { id: projectDepth; appWindow: dialog.appWindow; from: 0; to: 5; value: 1; visible: projectTabs.currentIndex === 2 }
                        Label { text: qsTr("Repeat (minutes)") }
                        IdmSpinBox { id: projectInterval; objectName: "syncProjectInterval"; appWindow: dialog.appWindow; from: projectTabs.currentIndex === 1 ? 1 : 0; to: 525600; value: 60; editable: true }
                        Label { text: qsTr("Start (ISO date, optional)") }
                        TextField { id: projectStart; objectName: "syncProjectStart"; Layout.fillWidth: true; placeholderText: "2026-12-01T09:00:00"; selectByMouse: true }
                        Label { text: qsTr("Stop (ISO date, optional)") }
                        TextField { id: projectStop; objectName: "syncProjectStop"; Layout.fillWidth: true; placeholderText: "2026-12-01T18:00:00"; selectByMouse: true }
                        Label { text: qsTr("Include patterns"); visible: projectTabs.currentIndex === 2 }
                        TextField { id: projectInclude; objectName: "syncProjectInclude"; Layout.fillWidth: true; visible: projectTabs.currentIndex === 2; placeholderText: "*.png, *.jpg"; selectByMouse: true }
                        Label { text: qsTr("Exclude patterns"); visible: projectTabs.currentIndex === 2 }
                        TextField { id: projectExclude; objectName: "syncProjectExclude"; Layout.fillWidth: true; visible: projectTabs.currentIndex === 2; placeholderText: "private/*"; selectByMouse: true }
                    }
                    CheckBox { id: projectEnabled; text: qsTr("Enabled"); checked: true }
                    Label {
                        Layout.fillWidth: true; wrapMode: Text.WordWrap
                        text: projectTabs.currentIndex === 1 ? qsTr("Completed files stay in this list. Changed remote files replace only unchanged files owned by this project. Use a new filename for an existing unrelated local file.") : qsTr("Repeat 0 runs once. Filters are comma-separated, case-sensitive URL path patterns. Discovery pages and CSS may still be fetched.")
                    }
                    Repeater {
                        model: projectTabs.currentIndex === 1 && dialog.controller ? dialog.controller.syncQueue : []
                        Label { required property var modelData; Layout.fillWidth: true; wrapMode: Text.WordWrap; text: modelData.filename + ": " + modelData.status + (modelData.error ? ": " + modelData.error : "") }
                    }
                }
            }
        }
        RowLayout {
            visible: projectTabs.currentIndex !== 0
            Button { objectName: "saveSyncProjectButton"; text: qsTr("Save project"); onClicked: dialog.saveProject() }
            Button { text: qsTr("Remove project"); enabled: projectChoice.currentIndex > 0; onClicked: dialog.removeProject() }
        }
        Label { text: dialog.failure; visible: text.length > 0; wrapMode: Text.WordWrap; Layout.fillWidth: true; color: dialog.appWindow ? dialog.appWindow.danger : "red" }
    }
}
