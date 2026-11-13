pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic

ComboBox {
    id: control
    required property var appWindow
    implicitHeight: 36
    implicitWidth: Math.max(150, contentItem.implicitWidth + leftPadding + rightPadding)
    leftPadding: mirrored ? 30 : 10
    rightPadding: mirrored ? 10 : 30
    font.pixelSize: 13
    hoverEnabled: true
    contentItem: TextField {
        text: control.editable ? control.editText : control.displayText
        enabled: control.editable
        autoScroll: control.editable
        readOnly: control.down
        selectByMouse: control.selectTextByMouse
        validator: control.validator
        inputMethodHints: control.inputMethodHints
        font: control.font
        opacity: control.enabled ? 1 : 0.4
        color: control.appWindow.ink
        selectionColor: control.appWindow.selection
        selectedTextColor: control.appWindow.ink
        verticalAlignment: Text.AlignVCenter
        padding: 0
        background: null
    }
    indicator: Text {
        text: "⌄"
        color: control.appWindow.muted
        font.pixelSize: 18
        x: control.mirrored ? 10 : control.width - width - 10
        y: (control.height - height) / 2 - 2
    }
    background: Rectangle {
        color: control.hovered ? control.appWindow.hoverSurface : control.appWindow.surface
        border.color: control.activeFocus ? control.appWindow.accent : control.appWindow.edge
        border.width: control.activeFocus ? 2 : 1
        radius: 6
        opacity: control.enabled ? 1 : 0.4
    }
    delegate: ItemDelegate {
        id: option
        required property var model
        required property int index
        width: ListView.view.width
        height: 36
        text: model[control.textRole]
        highlighted: control.highlightedIndex === index
        hoverEnabled: true
        font: control.font
        contentItem: Text { text: option.text; color: control.appWindow.ink; font: control.font; verticalAlignment: Text.AlignVCenter; elide: Text.ElideRight }
        background: Rectangle { color: option.highlighted || option.hovered ? control.appWindow.hoverSurface : "transparent"; radius: 5 }
    }
    popup: Popup {
        y: control.height + 4
        width: control.width
        padding: 5
        implicitHeight: Math.min(contentItem.implicitHeight + 10, 260)
        contentItem: ListView {
            implicitHeight: contentHeight
            model: control.popup.visible ? control.delegateModel : null
            currentIndex: control.highlightedIndex
            clip: true
            ScrollBar.vertical: IdmScrollBar { appWindow: control.appWindow }
        }
        background: Rectangle { color: control.appWindow.panel; border.color: control.appWindow.edge; radius: 8 }
    }
}
