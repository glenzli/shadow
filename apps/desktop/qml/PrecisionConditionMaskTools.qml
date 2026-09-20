pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: conditions
    required property var editor
    property bool editable: true
    readonly property var expression: editor.selectedConditionMask
    readonly property var rows: flatten(expression.root, ["root"], 0)
    spacing: 12

    function flatten(node, path, depth) {
        if (!node) return [];
        let inverse = false;
        let item = node;
        let valuePath = path;
        while (item.operator === "not") {
            inverse = !inverse;
            item = item.child;
            valuePath = valuePath.concat(["child"]);
        }
        let output = [{ node: item, path: path, valuePath: valuePath, depth: depth, inverse: inverse }];
        if (item.children) {
            for (let i = 0; i < item.children.length; ++i)
                output = output.concat(flatten(item.children[i], valuePath.concat(["children", i]), depth + 1));
        }
        return output;
    }
    function change(path, transform) {
        let copy = JSON.parse(JSON.stringify(expression));
        let parent = copy;
        for (let i = 0; i < path.length - 1; ++i) parent = parent[path[i]];
        const key = path[path.length - 1];
        parent[key] = transform(parent[key]);
        editor.setSelectedConditionMask(JSON.stringify(copy));
    }
    function treeDepth(node) {
        if (node.operator === "not") return 1 + treeDepth(node.child);
        return node.children ? 1 + Math.max(...node.children.map(treeDepth)) : 1;
    }
    function allows(path, transform) {
        let copy = JSON.parse(JSON.stringify(expression));
        let parent = copy;
        for (let i = 0; i < path.length - 1; ++i) parent = parent[path[i]];
        const key = path[path.length - 1];
        parent[key] = transform(parent[key]);
        return treeDepth(copy.root) <= 4;
    }
    function invert(node) {
        return node.operator === "not" ? node.child : { operator: "not", child: node };
    }
    function withLeaf(node, kind) {
        if (node.children && node.children.length < 4) node.children.push(leaf(kind));
        else node = { operator: "all", children: [node, leaf(kind)] };
        return node;
    }
    function leaf(kind) {
        return { operator: "leaf", condition: kind === "oklch_hue_range"
            ? { kind: kind, center_hue_degrees: 30, half_width_degrees: 30, softness: 0.4, minimum_chroma: 0, minimum_chroma_feather: 0 }
            : { kind: kind, lower: 0, upper: 1, softness: 0.08 } };
    }
    function add(path, kind) {
        change(path, function(node) {
            return withLeaf(node, kind);
        });
    }
    function remove(path) {
        if (path.length < 3) return;
        const groupPath = path.slice(0, -2);
        const index = path[path.length - 1];
        change(groupPath, function(node) {
            node.children.splice(index, 1);
            return node.children.length === 1 ? node.children[0] : node;
        });
    }
    function controls(c) {
        if (c.kind === "oklch_hue_range") return [
            { key: "center_hue_degrees", label: qsTr("Hue"), max: 359, scale: 1, suffix: "°" },
            { key: "half_width_degrees", label: qsTr("Range"), min: 1, max: 180, scale: 1, suffix: "°" },
            { key: "minimum_chroma", label: qsTr("Minimum chroma") },
            { key: "minimum_chroma_feather", label: qsTr("Chroma transition"), enabled: c.minimum_chroma > 0 },
            { key: "softness", label: qsTr("Softness") }
        ];
        return [{ key: "lower", label: qsTr("Lower") }, { key: "upper", label: qsTr("Upper") },
                { key: "softness", label: qsTr("Softness") }];
    }
    Label {
        Layout.fillWidth: true
        text: qsTr("Conditions use this node's input, before its adjustments.")
        color: Theme.textMuted
        font.pixelSize: Theme.fontMeta
        wrapMode: Text.WordWrap
    }
    Repeater {
        model: conditions.rows.length
        delegate: ColumnLayout {
            id: row
            required property int index
            readonly property var modelData: conditions.rows[index]
            readonly property bool group: modelData.node.operator !== "leaf"
            Layout.fillWidth: true
            Layout.leftMargin: modelData.depth * 8
            spacing: 6
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                ShadowComboBox {
                    Layout.fillWidth: true
                    enabled: conditions.editable
                    model: row.group ? [qsTr("All conditions"), qsTr("Any condition")]
                                     : [qsTr("Lightness"), qsTr("Color"), qsTr("Chroma")]
                    currentIndex: row.group ? (row.modelData.node.operator === "any" ? 1 : 0)
                        : ["oklab_lightness_range", "oklch_hue_range", "oklch_chroma_range"].indexOf(row.modelData.node.condition.kind)
                    onActivated: index => conditions.change(row.modelData.valuePath, function(node) {
                        if (row.group) node.operator = index === 0 ? "all" : "any";
                        else node = conditions.leaf(["oklab_lightness_range", "oklch_hue_range", "oklch_chroma_range"][index]);
                        return node;
                    })
                }
                ShadowButton {
                    compact: true
                    variant: ShadowButton.Ghost
                    text: qsTr("Exclude")
                    selected: row.modelData.inverse
                    enabled: conditions.editable && conditions.allows(row.modelData.path, conditions.invert)
                    toolTipText: qsTr("Use the inverse of this condition or group")
                    onClicked: conditions.change(row.modelData.path, conditions.invert)
                }
                ShadowIconButton {
                    source: "qrc:/icons/trash.svg"
                    accessibleName: qsTr("Remove")
                    toolTipText: accessibleName
                    visible: row.modelData.path.length > 1
                    enabled: conditions.editable
                    onClicked: conditions.remove(row.modelData.path)
                }
            }
            Repeater {
                model: row.group ? 0 : (row.modelData.node.condition.kind === "oklch_hue_range" ? 5 : 3)
                delegate: ShadowSlider {
                    required property int index
                    readonly property var modelData: conditions.controls(row.modelData.node.condition)[index]
                    Layout.fillWidth: true
                    label: modelData.label
                    from: modelData.min === undefined ? 0 : modelData.min
                    to: modelData.max === undefined ? 1 : modelData.max
                    stepSize: modelData.scale === 1 ? 1 : 0.01
                    decimals: 0
                    displayMultiplier: modelData.scale === undefined ? 100 : modelData.scale
                    suffix: modelData.suffix === undefined ? "%" : modelData.suffix
                    value: row.modelData.node.condition[modelData.key]
                    enabled: conditions.editable && modelData.enabled !== false
                    onGestureStarted: conditions.editor.beginParameterEdit("local_mask/conditions")
                    onGestureFinished: conditions.editor.endParameterEdit("local_mask/conditions")
                    onEdited: value => {
                        const key = modelData.key;
                        conditions.change(row.modelData.valuePath, function(node) {
                            node.condition[key] = value;
                            if (key === "lower") node.condition.upper = Math.max(value, node.condition.upper);
                            if (key === "upper") node.condition.lower = Math.min(value, node.condition.lower);
                            if (key === "minimum_chroma" && value === 0) node.condition.minimum_chroma_feather = 0;
                            return node;
                        });
                    }
                }
            }
            ShadowButton {
                compact: true
                variant: ShadowButton.Ghost
                text: row.group ? qsTr("Add condition") : qsTr("Limit further…")
                enabled: conditions.editable && conditions.rows.filter(r => r.node.operator === "leaf").length < 8
                    && conditions.allows(row.modelData.valuePath, node => conditions.withLeaf(node, "oklab_lightness_range"))
                onClicked: addMenu.popup()
                Menu {
                    id: addMenu
                    MenuItem { text: qsTr("Lightness"); onTriggered: conditions.add(row.modelData.valuePath, "oklab_lightness_range") }
                    MenuItem { text: qsTr("Color"); onTriggered: conditions.add(row.modelData.valuePath, "oklch_hue_range") }
                    MenuItem { text: qsTr("Chroma"); onTriggered: conditions.add(row.modelData.valuePath, "oklch_chroma_range") }
                }
            }
        }
    }
}
