import QtQuick
import QtQuick.Controls.Basic

SpinBox {
    id: control
    required property var appWindow
    implicitHeight: 36
    implicitWidth: 136
    leftPadding: 32
    rightPadding: 32
    font.pixelSize: 13
    contentItem: TextInput {
        text: control.displayText
        font: control.font
        opacity: control.enabled ? 1 : 0.4
        color: control.appWindow.ink
        selectionColor: control.appWindow.selection
        selectedTextColor: control.appWindow.ink
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        readOnly: !control.editable
        validator: control.validator
        inputMethodHints: control.inputMethodHints
        selectByMouse: true
        clip: true
    }
    up.indicator: Rectangle {
        x: control.mirrored ? 1 : control.width - width - 1
        y: 1
        implicitWidth: 30
        implicitHeight: 32
        height: control.height - 2
        radius: 5
        color: control.up.pressed ? control.appWindow.selection : control.up.hovered ? control.appWindow.hoverSurface : "transparent"
        Text { anchors.centerIn: parent; text: "+"; font.pixelSize: 17; color: control.up.indicator.enabled ? control.appWindow.ink : control.appWindow.muted; opacity: control.up.indicator.enabled ? 1 : 0.4 }
    }
    down.indicator: Rectangle {
        x: control.mirrored ? control.width - width - 1 : 1
        y: 1
        implicitWidth: 30
        implicitHeight: 32
        height: control.height - 2
        radius: 5
        color: control.down.pressed ? control.appWindow.selection : control.down.hovered ? control.appWindow.hoverSurface : "transparent"
        Text { anchors.centerIn: parent; text: "−"; font.pixelSize: 17; color: control.down.indicator.enabled ? control.appWindow.ink : control.appWindow.muted; opacity: control.down.indicator.enabled ? 1 : 0.4 }
    }
    background: Rectangle {
        color: control.appWindow.surface
        border.color: control.activeFocus ? control.appWindow.accent : control.appWindow.edge
        border.width: control.activeFocus ? 2 : 1
        radius: 6
        opacity: control.enabled ? 1 : 0.4
    }
}
