import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

MenuItem {
    id: control
    required property var appWindow
    property string shortcutHint: ""
    implicitHeight: 34
    implicitWidth: contentItem.implicitWidth + leftPadding + rightPadding
    leftPadding: 12
    rightPadding: control.subMenu ? 30 : 12
    hoverEnabled: true
    font.pixelSize: 13
    contentItem: RowLayout {
        spacing: 10
        opacity: control.enabled ? 1 : 0.38
        Image {
            visible: !control.checkable && source.toString().length > 0
            source: control.icon.source
            sourceSize.width: 18; sourceSize.height: 18
            Layout.preferredWidth: 16; Layout.preferredHeight: 16
            Layout.alignment: Qt.AlignVCenter
        }
        Text {
            visible: control.checkable
            text: control.checked ? "✓" : ""
            color: control.appWindow.accent
            font.pixelSize: 14
            Layout.preferredWidth: 16
        }
        Text {
            text: control.text
            font: control.font
            color: control.appWindow.ink
            Layout.fillWidth: true
            elide: Text.ElideRight
            verticalAlignment: Text.AlignVCenter
        }
        Text {
            text: control.shortcutHint
            visible: text.length > 0
            font.pixelSize: 11
            font.family: control.appWindow.mono
            color: control.appWindow.muted
        }
    }
    indicator: Item {}
    arrow: Text {
        visible: control.subMenu !== null
        text: control.mirrored ? "‹" : "›"
        color: control.appWindow.muted
        font.pixelSize: 20
        x: control.mirrored ? control.leftPadding : control.width - width - control.rightPadding
        y: (control.height - height) / 2
    }
    background: Rectangle {
        radius: 5
        color: control.down ? control.appWindow.selection : control.highlighted ? control.appWindow.hoverSurface : "transparent"
        border.color: control.activeFocus ? control.appWindow.accent : "transparent"
        Behavior on color { ColorAnimation { duration: 90 } }
    }
}
