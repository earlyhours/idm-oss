import QtQuick
import QtQuick.Controls.Basic

ScrollBar {
    id: control
    required property var appWindow
    padding: 3
    contentItem: Rectangle {
        implicitWidth: 5
        implicitHeight: 5
        radius: 3
        color: control.pressed ? control.appWindow.accent : control.hovered ? control.appWindow.muted : control.appWindow.edge
        opacity: control.policy === ScrollBar.AlwaysOn || control.active || control.hovered ? 1 : 0.45
        Behavior on opacity { NumberAnimation { duration: 120 } }
    }
    background: null
}
