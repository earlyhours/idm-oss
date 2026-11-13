import QtQuick
import QtQuick.Controls.Basic

MenuSeparator {
    id: control
    required property var appWindow
    topPadding: 5
    bottomPadding: 5
    leftPadding: 10
    rightPadding: 10
    contentItem: Rectangle { implicitWidth: 220; implicitHeight: 1; color: control.appWindow.edge }
}
