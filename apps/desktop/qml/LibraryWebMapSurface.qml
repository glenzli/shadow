pragma ComponentBehavior: Bound

import QtQuick
import QtWebView

Item {
    id: root

    required property var controller

    WebView {
        id: webView
        objectName: "libraryWebMap"
        anchors.fill: parent
        visible: root.controller.active && root.controller.providerAvailable

        onLoadingChanged: loadRequest => {
            root.controller.handleLoadStatus(
                Number(loadRequest.status), String(loadRequest.errorString || ""))
        }

        Component.onCompleted: root.controller.attachWebView(webView)
        Component.onDestruction: root.controller.detachWebView(webView)
    }

    Timer {
        interval: 90
        repeat: true
        running: webView.visible && root.controller.providerAvailable
        onTriggered: {
            webView.runJavaScript(
                "JSON.stringify(window.shadowMapDrainEvents ? window.shadowMapDrainEvents() : [])",
                result => root.controller.consumeEvents(String(result || "[]")))
        }
    }
}
