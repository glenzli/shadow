pragma ComponentBehavior: Bound

import QtQml

// Canonical defensive projection of the backend analysis map. Both the scope
// renderer and its surrounding presentation consume this owner instead of
// repeating string-key lookup and validation policy.
QtObject {
    id: data

    property var analysis: null
    property string displayGeneration: ""

    readonly property bool hasHistogramData: mapBoolean("valid", false) && binList("red").length === 256 && binList("green").length === 256 && binList("blue").length === 256 && binList("luma").length === 256
    readonly property int displayScopeGridSize: mapNumber("displayScopeGridSize", 0)
    readonly property int displayScopeBinCount: displayScopeGridSize * displayScopeGridSize
    readonly property bool hasDisplayScope: mapBoolean("displayScopeAvailable", false) && displayScopeGridSize > 0 && binList("displayWaveform").length === displayScopeBinCount && binList("displayParadeRed").length === displayScopeBinCount && binList("displayParadeGreen").length === displayScopeBinCount && binList("displayParadeBlue").length === displayScopeBinCount && binList("displayVectorscope").length === displayScopeBinCount

    readonly property bool updating: mapBoolean("updating", false)
    readonly property bool stale: mapBoolean("stale", false)
    readonly property string analysisGeneration: generationText(field("generation", ""))
    readonly property bool generationMatches: hasHistogramData && displayGeneration.length > 0 && analysisGeneration.length > 0 && displayGeneration === analysisGeneration
    readonly property bool presentationStale: stale || (hasHistogramData && !generationMatches)
    readonly property bool approximate: mapBoolean("approximate", true)
    readonly property int sampleWidth: mapNumber("width", 0)
    readonly property int sampleHeight: mapNumber("height", 0)
    readonly property real shadowFraction: mapNumber("shadowClippedFraction", 0)
    readonly property real highlightFraction: mapNumber("highlightClippedFraction", 0)
    readonly property real shadowPixels: mapNumber("shadowClippedPixels", 0)
    readonly property real highlightPixels: mapNumber("highlightClippedPixels", 0)
    readonly property real hdrHeadroomPixels: mapNumber("hdrHeadroomPixels", 0)
    readonly property real hdrPeakHeadroomEv: mapNumber("hdrPeakHeadroomEv", 0)
    readonly property int displayScopeWidth: mapNumber("displayScopeWidth", 0)
    readonly property int displayScopeHeight: mapNumber("displayScopeHeight", 0)
    readonly property real displayScopeSampledPixels: mapNumber("displayScopeSampledPixels", 0)
    readonly property real displayScopeMatchedPixels: mapNumber("displayScopeMatchedPixels", 0)
    readonly property bool pointColorScopeActive: mapBoolean("displayScopePointColorQualified", false)
    readonly property bool referenceSelection: mapBoolean("displayScopeReferenceSelection", false)
    readonly property bool displayScopeCentroidAvailable: mapBoolean("displayScopeCentroidAvailable", false)
    readonly property real displayScopeCentroidCb: mapNumber("displayScopeCentroidCb", 0)
    readonly property real displayScopeCentroidCr: mapNumber("displayScopeCentroidCr", 0)
    readonly property real displayScopeSkinGuideDeviation: mapNumber("displayScopeSkinGuideDeviationDegrees", 0)

    function field(name, fallback) {
        if (analysis === null || analysis === undefined)
            return fallback;
        const value = analysis[name];
        return value === null || value === undefined ? fallback : value;
    }

    function mapBoolean(name, fallback) {
        return Boolean(field(name, fallback));
    }

    function mapNumber(name, fallback) {
        const value = Number(field(name, fallback));
        return Number.isFinite(value) ? value : fallback;
    }

    function generationText(value) {
        if (value === null || value === undefined)
            return "";
        return String(value);
    }

    function binList(name) {
        const value = field(name, []);
        return value !== null && value !== undefined && value.length !== undefined ? value : [];
    }

    function binCount(bins, index) {
        if (index < 0 || index >= bins.length)
            return 0;
        const value = Number(bins[index]);
        return Number.isFinite(value) && value > 0 ? value : 0;
    }

    function skinToneRange(name) {
        const prefix = "displayScopeSkin" + name;
        return {
            available: mapBoolean(prefix + "Available", false),
            matchedPixels: mapNumber(prefix + "MatchedPixels", 0),
            cb: mapNumber(prefix + "CentroidCb", 0),
            cr: mapNumber(prefix + "CentroidCr", 0),
            deviation: mapNumber(prefix + "DeviationDegrees", 0)
        };
    }
}
