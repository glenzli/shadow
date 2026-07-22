pragma Singleton

import QtQuick

QtObject {
    // The host will bind `mode` and, while in System mode, `effectiveDark` to
    // the platform appearance. Until that integration is installed, System
    // deliberately falls back to dark so startup never flashes a light theme.
    enum AppearanceMode {
        System,
        Light,
        Dark
    }

    property int mode: 0
    property bool effectiveDark: mode !== 1

    // Stable desktop density. Components consume these tokens instead of
    // drifting into slightly different heights, radii and type scales.
    readonly property int compactControlHeight: 30
    readonly property int controlHeight: 34
    readonly property int compactControlRadius: 5
    readonly property int controlRadius: 6
    readonly property int sectionSpacing: 14
    readonly property int panelPadding: 16
    readonly property int fontBody: 12
    readonly property int fontMeta: 10
    readonly property int fontSection: 11

    // Base surfaces
    readonly property color window: effectiveDark ? "#0f1114" : "#f1f3f5"
    readonly property color chrome: effectiveDark ? "#121519" : "#fcfcfd"
    readonly property color panel: effectiveDark ? "#171a1f" : "#f8f9fb"
    readonly property color panelRaised: effectiveDark ? "#1e2329" : "#ffffff"
    readonly property color panelInset: effectiveDark ? "#13171b" : "#f3f5f7"
    readonly property color menuSurface: effectiveDark ? "#20252b" : "#ffffff"
    readonly property color surfaceSubtle: effectiveDark ? "#20252b" : "#f3f5f7"
    readonly property color surfaceSelected: effectiveDark ? "#252b32" : "#e9eef3"
    readonly property color surfaceSelectedStrong: effectiveDark ? "#2a3037" : "#e1e7ed"
    readonly property color surfaceNeutralSelected: effectiveDark ? "#343b43" : "#d9e0e7"
    readonly property color control: effectiveDark ? "#1d2228" : "#ffffff"
    readonly property color controlQuiet: effectiveDark ? "#20252b" : "#f3f5f7"
    readonly property color actionSurface: effectiveDark ? "#282e35" : "#e9edf1"
    readonly property color actionPressedSurface: effectiveDark ? "#27384a" : "#e0eaf4"
    readonly property color controlPressed: effectiveDark ? "#2b323a" : "#e9edf2"
    readonly property color controlQuietPressed: effectiveDark ? "#2d343c" : "#e7ebef"
    readonly property color controlPressedStrong: effectiveDark ? "#303740" : "#dfe5eb"
    readonly property color controlDisabled: effectiveDark ? "#242a30" : "#f1f3f5"
    readonly property color photoCanvas: effectiveDark ? "#090b0d" : "#555a5f"
    readonly property color comparisonCanvas: effectiveDark ? "#08090a" : "#4d5257"
    readonly property color plot: effectiveDark ? "#0b0e11" : "#e8ebee"
    readonly property color transparent: "#00000000"

    // Modern controls and elevation. Shadows are deliberately reserved for
    // primary actions and floating surfaces instead of being applied to every
    // panel in the application.
    readonly property color buttonSurface: effectiveDark ? "#252d36" : "#ffffff"
    readonly property color buttonHoverSurface: effectiveDark ? "#303b47" : "#f3f6f8"
    readonly property color buttonPressedSurface: effectiveDark ? "#3b4856" : "#e9edf2"
    readonly property color buttonGhostHover: effectiveDark ? "#252f39" : "#f1f3f5"
    readonly property color buttonGhostPressed: effectiveDark ? "#303c48" : "#e7ebef"
    readonly property color buttonDisabledSurface: effectiveDark ? "#181e25" : "#f1f3f5"
    readonly property color buttonDisabledGhostSurface: effectiveDark ? "#151a20" : "#f5f7f8"
    readonly property color buttonBorder: effectiveDark ? "#53616e" : "#d1d8e0"
    readonly property color focusRing: effectiveDark ? "#76a6d4" : "#5a91cb"
    readonly property color shadowSoft: effectiveDark ? "#8a000000" : "#1a18202a"
    readonly property color shadowStrong: effectiveDark ? "#a6000000" : "#30151b24"

    // Structure and focus
    readonly property color border: effectiveDark ? "#323b45" : "#d9dee5"
    readonly property color borderStrong: effectiveDark ? "#485460" : "#c8d0d8"
    readonly property color borderEmphasis: effectiveDark ? "#5b6978" : "#aab4bf"
    readonly property color borderDisabled: effectiveDark ? "#303943" : "#e3e7eb"
    readonly property color imageBorder: effectiveDark ? "#252b32" : "#c9d0d7"
    readonly property color track: effectiveDark ? "#3b4652" : "#d0d6dd"
    readonly property color separatorStrong: effectiveDark ? "#404852" : "#bbc4cd"
    readonly property color previewHudBorder: effectiveDark ? "#45505a" : "#909ba5"

    // Text and icons
    readonly property color textPrimary: effectiveDark ? "#f1f4f6" : "#20252a"
    readonly property color textSecondary: effectiveDark ? "#cbd3d9" : "#4f5964"
    readonly property color textMuted: effectiveDark ? "#9ea9b4" : "#626e79"
    readonly property color textSubtle: effectiveDark ? "#7f8b97" : "#65717d"
    readonly property color textQuiet: effectiveDark ? "#707d89" : "#68737e"
    readonly property color textPending: effectiveDark ? "#6d7781" : "#87919b"
    readonly property color textFaint: effectiveDark ? "#5f6b77" : "#929ba4"
    readonly property color navigationTextDisabled: effectiveDark ? "#6b7783" : "#a1a8af"
    readonly property color textDisabled: effectiveDark ? "#74818d" : "#a4acb5"
    readonly property color textDisabledQuiet: effectiveDark ? "#687480" : "#aab1b9"
    readonly property color textPlaceholder: effectiveDark ? "#68727c" : "#7f8992"
    readonly property color rawPlaceholderText: effectiveDark ? "#727d88" : "#67727c"

    // Selection uses a restrained accent, while filled primary actions use a
    // slightly deeper surface so navigation state and calls to action do not
    // compete at the same visual weight.
    readonly property color accent: effectiveDark ? "#70a3d6" : "#3574b9"
    readonly property color accentPressed: effectiveDark ? "#5d91c6" : "#285f9c"
    readonly property color accentHover: effectiveDark ? "#82b0df" : "#407fc2"
    readonly property color accentForeground: "#ffffff"
    readonly property color selectionForeground: effectiveDark ? "#15191d" : "#ffffff"
    readonly property color primaryAction: effectiveDark ? "#3977b3" : "#2f6ead"
    readonly property color primaryActionHover: effectiveDark ? "#4382c0" : "#397aba"
    readonly property color primaryActionPressed: effectiveDark ? "#2e659b" : "#255b91"
    readonly property color primaryActionForeground: "#ffffff"
    readonly property color accentHandleBorder: effectiveDark ? "#16263a" : "#204a73"
    readonly property color accentSurface: effectiveDark ? "#223d58" : "#eaf2fa"
    readonly property color accentSurfacePressed: effectiveDark ? "#294a68" : "#dce9f6"
    readonly property color accentSurfaceQuiet: effectiveDark ? "#1b2a39" : "#f4f8fc"
    readonly property color accentBorder: effectiveDark ? "#5e95c4" : "#91b4d8"
    readonly property color accentTextMuted: effectiveDark ? "#b5d1ea" : "#345f89"
    readonly property color accentTextQuiet: effectiveDark ? "#98bad7" : "#597997"
    readonly property color accentSelectionSurface: effectiveDark ? "#28445f" : "#dfebf7"
    readonly property color accentSelectionText: effectiveDark ? "#b9d6ee" : "#285f9c"
    readonly property color sharedNodeBorder: effectiveDark ? "#4d7194" : "#9bb6d1"
    readonly property color switchOnSurface: effectiveDark ? "#2b4c6c" : "#c8dcef"
    readonly property color switchOnBorder: effectiveDark ? "#5684b0" : "#739cc4"
    readonly property color switchOffSurface: effectiveDark ? "#282e34" : "#dfe4e8"
    readonly property color switchOffBorder: effectiveDark ? "#4a535c" : "#9ca6af"

    // Lightroom-style local color labels. Their identities stay stable across
    // themes while the surrounding focus treatment comes from the controls.
    readonly property color labelRed: effectiveDark ? "#ef787d" : "#d9535d"
    readonly property color labelYellow: effectiveDark ? "#e7c55a" : "#c89a26"
    readonly property color labelGreen: effectiveDark ? "#73c48b" : "#3c9a5d"
    readonly property color labelBlue: effectiveDark ? "#72a9e5" : "#397fca"
    readonly property color labelPurple: effectiveDark ? "#b18ae3" : "#8158bd"

    function colorLabel(label) {
        switch (String(label).toLowerCase()) {
        case "red": return labelRed
        case "yellow": return labelYellow
        case "green": return labelGreen
        case "blue": return labelBlue
        case "purple": return labelPurple
        default: return transparent
        }
    }

    // Status colors
    readonly property color successSurface: effectiveDark ? "#214030" : "#e1f0e6"
    readonly property color successBorder: effectiveDark ? "#609677" : "#5b8e6c"
    readonly property color successText: effectiveDark ? "#a7d2b6" : "#2f6943"
    readonly property color successTextMuted: effectiveDark ? "#9fc7a7" : "#447b55"
    readonly property color savedSurface: effectiveDark ? "#19241f" : "#e8f4ec"
    readonly property color savedBorder: effectiveDark ? "#294436" : "#7fa68c"
    readonly property color savedText: effectiveDark ? "#91bda0" : "#376c49"
    readonly property color readyText: effectiveDark ? "#78a889" : "#3f7853"

    readonly property color dangerSurface: effectiveDark ? "#492a28" : "#f8e5e3"
    readonly property color dangerHoverSurface: effectiveDark ? "#59312e" : "#f3d8d5"
    readonly property color dangerPressedSurface: effectiveDark ? "#673733" : "#ecc8c4"
    readonly property color dangerBorder: effectiveDark ? "#a4645d" : "#b65f56"
    readonly property color dangerText: effectiveDark ? "#e2aaa3" : "#8a3028"
    readonly property color errorText: effectiveDark ? "#d28e82" : "#a43b31"
    readonly property color errorBorder: effectiveDark ? "#8b5148" : "#b8685e"

    readonly property color warningSurface: effectiveDark ? "#3b3324" : "#f8ecd2"
    readonly property color warningBorder: effectiveDark ? "#8f7a54" : "#aa7b21"
    readonly property color warningText: effectiveDark ? "#a99268" : "#76520d"
    readonly property color warningNoticeText: effectiveDark ? "#8f7a54" : "#795817"
    readonly property color currentRevisionSurface: accentSurface
    readonly property color currentRevisionBorder: accentBorder

    // Overlays preserve their original alpha in both appearances.
    readonly property color thumbnailCaptionOverlay: effectiveDark
        ? "#e615181c" : "#e6ffffff"
    readonly property color busyOverlay: effectiveDark ? "#b00c0e10" : "#b0ffffff"
    readonly property color previewHudOverlay: effectiveDark
        ? "#c9181c21" : "#c9ffffff"
    readonly property color previewHudStrongOverlay: effectiveDark
        ? "#d9181c21" : "#d9ffffff"

    // Tone curve
    readonly property color curveGrid: effectiveDark ? "#293038" : "#cbd1d7"
    readonly property color curveIdentity: effectiveDark ? "#59636e" : "#7f8993"
    readonly property color curveRed: effectiveDark ? "#ef7772" : "#c44943"
    readonly property color curveGreen: effectiveDark ? "#6dc98a" : "#2f9253"
    readonly property color curveBlue: effectiveDark ? "#75a6ec" : "#3f72c5"

    // Histogram and clipping indicators
    readonly property color histogramFrameBorder: effectiveDark ? "#22282e" : "#c7cdd3"
    readonly property color histogramGrid: effectiveDark
        ? "#3d4e5760" : "#3d53606c"
    readonly property color histogramRedFill: effectiveDark
        ? "#40eb504c" : "#38d83f3b"
    readonly property color histogramGreenFill: effectiveDark
        ? "#3856cf76" : "#3850a966"
    readonly property color histogramBlueFill: effectiveDark
        ? "#454e87ee" : "#40516fc2"
    readonly property color histogramLumaStroke: effectiveDark
        ? "#ebeff3f6" : "#e622282e"
    readonly property color clippingIdleSurface: effectiveDark ? "#161a1e" : "#edf0f3"
    readonly property color shadowClipSurface: effectiveDark ? "#1b2935" : "#e0edf6"
    readonly property color shadowClipBorder: effectiveDark ? "#47657c" : "#6e94ad"
    readonly property color shadowClipText: effectiveDark ? "#88b9dc" : "#386985"
    readonly property color highlightClipSurface: effectiveDark ? "#332421" : "#f5e4e0"
    readonly property color highlightClipBorder: effectiveDark ? "#765048" : "#b77b70"
    readonly property color highlightClipText: effectiveDark ? "#dfa096" : "#934b41"
}
