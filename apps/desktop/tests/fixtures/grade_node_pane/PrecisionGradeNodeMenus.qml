import QtQuick

Item {
    property var editor
    property var interchangeController
    property var lutExportController

    signal cropGeometryRequested
    signal repairRequested
    signal liquifyRequested

    function openAdd(anchor) {}
    function openExchange(anchor) {}
    function openContext(anchor, pointerY, targetData) {}
}
