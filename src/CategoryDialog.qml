import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Dialog {
    id: dialog
    objectName: "categoryDialog"
    property var controller
    property var appWindow
    property string failure: ""
    readonly property var entries: controller ? controller.preferences.categories || [] : []
    palette: appWindow ? appWindow.palette : undefined
    modal: true
    title: qsTr("Custom categories")
    width: Math.min(640, appWindow ? appWindow.width - 24 : 640)
    height: Math.min(560, appWindow ? appWindow.height - 36 : 560)
    standardButtons: Dialog.Close
    onOpened: loadCategory(-1)
    function loadCategory(index) {
        const rule = entries[index] || {}
        choice.currentIndex = index + 1
        categoryName.text = rule.name || ""
        extensions.text = (rule.extensions || []).join(", ")
        origins.text = (rule.origins || []).join(", ")
        directory.text = rule.directory || ""
        failure = ""
    }
    function tokens(text) { return text.split(/[,;\s]+/).filter(s => s.length > 0) }
    function saveCategory() {
        const rules = entries.slice(), index = choice.currentIndex - 1
        const rule = {name: categoryName.text.trim(), extensions: tokens(extensions.text), origins: tokens(origins.text), directory: directory.text.trim()}
        if (index < 0) rules.push(rule)
        else rules[index] = rule
        if (!controller.configure({categories: rules})) failure = controller.error
        else loadCategory(index < 0 ? rules.length - 1 : index)
    }
    function removeCategory() {
        const rules = entries.slice(), index = choice.currentIndex - 1
        if (index < 0) return
        rules.splice(index, 1)
        if (!controller.configure({categories: rules})) failure = controller.error
        else loadCategory(-1)
    }
    ColumnLayout {
        anchors.fill: parent
        spacing: 12
        IdmComboBox {
            id: choice; objectName: "categoryChoice"; appWindow: dialog.appWindow
            Layout.fillWidth: true
            model: [qsTr("New category")].concat(dialog.entries.map(c => c.name))
            onActivated: dialog.loadCategory(currentIndex - 1)
        }
        ScrollView {
            id: scroll; Layout.fillWidth: true; Layout.fillHeight: true; clip: true
            contentWidth: availableWidth
            ColumnLayout {
                width: scroll.availableWidth; spacing: 10
                Label { text: qsTr("Match any extension or exact site origin. The first matching category wins. Unmatched files keep their built-in category."); wrapMode: Text.WordWrap; Layout.fillWidth: true }
                Label { text: qsTr("Name") }
                TextField { id: categoryName; objectName: "categoryName"; Layout.fillWidth: true; selectByMouse: true; Accessible.name: qsTr("Category name") }
                Label { text: qsTr("File extensions (separate with commas)") }
                TextField { id: extensions; objectName: "categoryExtensions"; Layout.fillWidth: true; placeholderText: "pdf, .zip, *.mp3"; selectByMouse: true; Accessible.name: qsTr("File extensions") }
                Label { text: qsTr("Exact site origins (separate with commas)") }
                TextField { id: origins; objectName: "categoryOrigins"; Layout.fillWidth: true; placeholderText: "https://example.com, http://example.com:8080"; selectByMouse: true; Accessible.name: qsTr("Exact site origins") }
                Label { text: qsTr("Download directory (optional)") }
                TextField { id: directory; objectName: "categoryDirectory"; Layout.fillWidth: true; placeholderText: qsTr("Use the default download directory"); selectByMouse: true; Accessible.name: qsTr("Category download directory") }
                Label { text: qsTr("An explicit download destination or saved site directory takes priority. Editing rules updates classification, but does not move existing files."); wrapMode: Text.WordWrap; Layout.fillWidth: true }
            }
        }
        Label { objectName: "categoryError"; text: dialog.failure; visible: text.length > 0; color: dialog.appWindow ? dialog.appWindow.danger : "red"; wrapMode: Text.WordWrap; Layout.fillWidth: true }
        RowLayout {
            Button { objectName: "saveCategoryButton"; text: qsTr("Save category"); onClicked: dialog.saveCategory() }
            Button { objectName: "removeCategoryButton"; text: qsTr("Remove category"); enabled: choice.currentIndex > 0; onClicked: dialog.removeCategory() }
        }
    }
}
