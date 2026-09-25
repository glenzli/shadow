import QtQuick

Item {
    property var editor
    readonly property int currentNodeDestination: 0

    signal maskCreated
    signal aiMaskRequested
    signal editExistingRequested

    function openFor(anchor, destination) {}
}
