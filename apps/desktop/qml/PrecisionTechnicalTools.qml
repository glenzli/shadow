pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts

// Index for the technical correction pipeline. Each child owns one independent
// editing responsibility; this surface owns only ordering and intent routing.
ColumnLayout {
    id: technical

    required property var inspector
    required property int currentTabIndex
    required property bool foundationSelected

    signal openOpticsProfileLibraryRequested()

    spacing: 8

    PrecisionDetailSection {
        Layout.fillWidth: true
        visible: !technical.foundationSelected
            && technical.currentTabIndex === 0
        inspector: technical.inspector
    }

    PrecisionOpticsSection {
        Layout.fillWidth: true
        visible: technical.currentTabIndex === 0
        inspector: technical.inspector
        onOpenProfileLibraryRequested:
            technical.openOpticsProfileLibraryRequested()
    }

    PrecisionEffectsSection {
        Layout.fillWidth: true
        visible: !technical.foundationSelected
            && technical.currentTabIndex === 1
        inspector: technical.inspector
    }
}
