pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic

Menu {
    id: control
    required property var appWindow
    implicitWidth: Math.max(252, implicitContentWidth + leftPadding + rightPadding)
    padding: 5
    margins: 8
    overlap: -4
    popupType: Popup.Item
    delegate: IdmMenuItem { appWindow: control.appWindow }
    background: Rectangle {
        color: control.appWindow.panel
        border.color: control.appWindow.edge
        radius: 8
        Rectangle { anchors.fill: parent; anchors.margins: -2; z: -1; radius: 10; color: "#18000000" }
    }
    enter: Transition { NumberAnimation { property: "opacity"; from: 0; to: 1; duration: 90 } }
    exit: Transition { NumberAnimation { property: "opacity"; from: 1; to: 0; duration: 70 } }
}
