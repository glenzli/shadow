pragma ComponentBehavior: Bound

import QtQuick
import QtWebView

Item {
    id: root

    required property var controller
    required property bool presentationAllowed
    readonly property bool webViewMaterialized: webViewLoader.item !== null

    Loader {
        id: webViewLoader
        anchors.fill: parent
        active: root.presentationAllowed && root.controller.active
            && root.controller.providerAvailable

        sourceComponent: WebView {
            id: webView
            objectName: "libraryWebMap"
            anchors.fill: parent

            onLoadingChanged: loadRequest => {
                root.controller.handleLoadStatus(
                    Number(loadRequest.status),
                    String(loadRequest.errorString || ""))
            }

            Component.onCompleted: root.controller.attachWebView(webView)
            Component.onDestruction: root.controller.detachWebView(webView)
        }
    }

    Timer {
        interval: 90
        repeat: true
        running: root.webViewMaterialized
        onTriggered: {
            const activeWebView = webViewLoader.item
            if (!activeWebView)
                return
            activeWebView.runJavaScript(
                "JSON.stringify(window.shadowMapDrainEvents ? window.shadowMapDrainEvents() : [])",
                result => root.controller.consumeEvents(String(result || "[]")))
        }
    }
}
