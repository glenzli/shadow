#include <QCoreApplication>
#include <QFile>
#include <QGuiApplication>
#include <QMetaObject>
#include <QObject>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QString>
#include <QVariant>
#include <QVariantMap>

#include <cstdlib>
#include <iostream>
#include <memory>

class FakeMaskEditor final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool active READ active CONSTANT)
    Q_PROPERTY(QString photoId READ photoId NOTIFY sourceIdentityChanged)
    Q_PROPERTY(QString representationId READ representationId NOTIFY sourceIdentityChanged)
    Q_PROPERTY(QString selectedGradeNodeId READ selectedGradeNodeId NOTIFY selectedGradeNodeChanged)
    Q_PROPERTY(bool stateBusy READ stateBusy NOTIFY availabilityChanged)
    Q_PROPERTY(bool hasSelectedGradeNode READ hasSelectedGradeNode NOTIFY availabilityChanged)
    Q_PROPERTY(bool gradeNodeEnabled READ gradeNodeEnabled NOTIFY availabilityChanged)
    Q_PROPERTY(bool canAddGradeNode READ canAddGradeNode NOTIFY availabilityChanged)
    Q_PROPERTY(QVariantMap selectedLocalMask READ selectedLocalMask NOTIFY availabilityChanged)
    Q_PROPERTY(QVariantList localMaskComponents READ localMaskComponents NOTIFY availabilityChanged)

  public:
    using QObject::QObject;

    [[nodiscard]] bool active() const noexcept {
        return true;
    }
    [[nodiscard]] QString photoId() const {
        return photo_id_;
    }
    [[nodiscard]] QString representationId() const {
        return representation_id_;
    }
    [[nodiscard]] QString selectedGradeNodeId() const {
        return selected_node_id_;
    }
    [[nodiscard]] bool stateBusy() const noexcept {
        return state_busy_;
    }
    [[nodiscard]] bool hasSelectedGradeNode() const noexcept {
        return has_selected_node_;
    }
    [[nodiscard]] bool gradeNodeEnabled() const noexcept {
        return node_enabled_;
    }
    [[nodiscard]] bool canAddGradeNode() const noexcept {
        return can_add_node_;
    }
    [[nodiscard]] QVariantMap selectedLocalMask() const {
        return {{QStringLiteral("kind"), mask_kind_}};
    }
    [[nodiscard]] QVariantList localMaskComponents() const {
        QVariantList components;
        for (int index = 0; index < mask_component_count_; ++index) {
            components.push_back(QVariantMap{{QStringLiteral("index"), index}});
        }
        return components;
    }

    void setAvailability(
        const bool state_busy,
        const bool has_selected_node,
        const bool node_enabled,
        const bool can_add_node,
        const int mask_kind
    ) {
        state_busy_ = state_busy;
        has_selected_node_ = has_selected_node;
        node_enabled_ = node_enabled;
        can_add_node_ = can_add_node;
        mask_kind_ = mask_kind;
        mask_component_count_ = mask_kind == 0 ? 0 : 1;
        emit availabilityChanged();
    }

    void setMaskComponentCount(const int count) {
        mask_component_count_ = count;
        mask_kind_ = count == 0 ? 0 : 3;
        emit availabilityChanged();
    }

    void setSelectedNodeId(const QString& value) {
        selected_node_id_ = value;
        emit selectedGradeNodeChanged();
    }

    Q_INVOKABLE bool createLocalMask(const int kind, const int destination) {
        ++create_count_;
        last_kind_ = kind;
        last_destination_ = destination;
        return create_result_;
    }

    Q_INVOKABLE bool addLocalMaskComponent(const int kind, const int operation) {
        ++add_component_count_;
        last_kind_ = kind;
        last_operation_ = operation;
        return create_result_;
    }

    Q_INVOKABLE bool
    beginAiMaskPromptForOperation(const int operation, const bool prefer_current_node) {
        ++subject_prompt_count_;
        last_operation_ = operation;
        last_prefer_current_node_ = prefer_current_node;
        return ai_prompt_result_;
    }

    Q_INVOKABLE bool
    beginAiFaceMaskPromptForOperation(const int operation, const bool prefer_current_node) {
        ++face_prompt_count_;
        last_operation_ = operation;
        last_prefer_current_node_ = prefer_current_node;
        return ai_prompt_result_;
    }

    Q_INVOKABLE bool beginAiSemanticMaskForOperation(
        const QString& query,
        const int operation,
        const bool prefer_current_node
    ) {
        ++semantic_prompt_count_;
        last_semantic_query_ = query;
        last_operation_ = operation;
        last_prefer_current_node_ = prefer_current_node;
        return ai_prompt_result_;
    }

    bool create_result_ = true;
    int create_count_ = 0;
    int add_component_count_ = 0;
    int last_kind_ = -1;
    int last_destination_ = -1;
    int last_operation_ = -1;
    bool last_prefer_current_node_ = false;
    bool ai_prompt_result_ = true;
    int subject_prompt_count_ = 0;
    int face_prompt_count_ = 0;
    int semantic_prompt_count_ = 0;
    QString last_semantic_query_;

  signals:
    void availabilityChanged();
    void activeChanged();
    void sourceIdentityChanged();
    void selectedGradeNodeChanged();

  private:
    bool state_busy_ = false;
    bool has_selected_node_ = true;
    bool node_enabled_ = true;
    bool can_add_node_ = true;
    int mask_kind_ = 0;
    int mask_component_count_ = 0;
    QString photo_id_ = QStringLiteral("photo");
    QString representation_id_ = QStringLiteral("representation");
    QString selected_node_id_ = QStringLiteral("node");
};

namespace {

[[nodiscard]] bool require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "Precision mask-create menu contract failed: " << message << '\n';
    }
    return condition;
}

[[nodiscard]] bool invokeWithInt(QObject* target, const char* method, const int value) {
    return QMetaObject::invokeMethod(target, method, Q_ARG(QVariant, QVariant(value)));
}

[[nodiscard]] bool invokeWithBool(QObject* target, const char* method, const bool value) {
    return QMetaObject::invokeMethod(target, method, Q_ARG(QVariant, QVariant(value)));
}

[[nodiscard]] bool invokeWithString(QObject* target, const char* method, const QString& value) {
    return QMetaObject::invokeMethod(target, method, Q_ARG(QVariant, QVariant(value)));
}

void drainBindings() {
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents();
}

} // namespace

int main(int argc, char* argv[]) {
    QGuiApplication application(argc, argv);
    QFile local_mask_tools(
        QStringLiteral(SHADOW_DESKTOP_SOURCE_DIR "/qml/PrecisionLocalMaskTools.qml")
    );
    if (!require(
            local_mask_tools.open(QIODevice::ReadOnly),
            "local mask tools source is readable"
        )) {
        return EXIT_FAILURE;
    }
    const QByteArray local_mask_tools_source = local_mask_tools.readAll();
    if (!require(
            local_mask_tools_source.contains("removeSelectedLocalMaskComponent()")
                && !local_mask_tools_source.contains("setSelectedLocalMask(0)"),
            "the visible delete action delegates component removal instead of replacing the node "
            "mask"
        )) {
        return EXIT_FAILURE;
    }
    QQmlEngine engine;
    QQmlComponent component(
        &engine,
        QUrl::fromLocalFile(
            QStringLiteral(SHADOW_DESKTOP_SOURCE_DIR "/qml/PrecisionMaskCreateMenu.qml")
        )
    );
    FakeMaskEditor editor;
    std::unique_ptr<QObject> menu(component.createWithInitialProperties({
        {QStringLiteral("editor"), QVariant::fromValue(&editor)},
    }));
    if (!menu) {
        std::cerr << component.errorString().toStdString();
        return EXIT_FAILURE;
    }

    drainBindings();
    QQuickItem* destination_selector =
        menu->findChild<QQuickItem*>(QStringLiteral("maskDestinationSelector"));
    QQuickItem* brush_action = menu->findChild<QQuickItem*>(QStringLiteral("brushMaskAction"));
    QQuickItem* subject_action =
        menu->findChild<QQuickItem*>(QStringLiteral("aiSubjectMaskAction"));
    QQuickItem* people_details_action =
        menu->findChild<QQuickItem*>(QStringLiteral("aiPeopleDetailsMaskAction"));
    QQuickItem* semantic_action =
        menu->findChild<QQuickItem*>(QStringLiteral("aiSemanticMaskAction"));
    if (!require(
            destination_selector != nullptr && brush_action != nullptr && subject_action != nullptr
                && people_details_action != nullptr && semantic_action != nullptr
                && destination_selector->y() < brush_action->y(),
            "the destination choice is presented before mask type selection"
        )
        || !require(
            menu->property("currentNodeAvailable").toBool()
                && menu->property("newNodeAvailable").toBool(),
            "an enabled empty node exposes both atomic destinations"
        )
        || !require(
            invokeWithInt(menu.get(), "selectDestination", 0)
                && menu->property("destination").toInt() == 0,
            "the node-row entry keeps the current-node default"
        )
        || !require(
            invokeWithInt(menu.get(), "createMask", 3) && editor.create_count_ == 1
                && editor.last_kind_ == 3 && editor.last_destination_ == 0,
            "current-node creation delegates one complete brush transaction"
        )
        || !require(
            invokeWithInt(menu.get(), "createMask", 4) && editor.create_count_ == 2
                && editor.last_kind_ == 4 && editor.last_destination_ == 0,
            "luminance conditions use the same atomic current-node transaction"
        )
        || !require(
            invokeWithInt(menu.get(), "selectDestination", 1)
                && invokeWithInt(menu.get(), "createMask", 5) && editor.create_count_ == 3
                && editor.last_kind_ == 5 && editor.last_destination_ == 1,
            "color conditions use the same atomic new-node transaction"
        )
        || !require(
            invokeWithBool(menu.get(), "startAiMask", false)
                && invokeWithBool(menu.get(), "startAiMask", true)
                && editor.subject_prompt_count_ == 1 && editor.face_prompt_count_ == 1
                && editor.last_operation_ == 0 && !editor.last_prefer_current_node_,
            "both AI selectors delegate one controller-owned new-node transaction"
        )
        || !require(
            invokeWithString(menu.get(), "startSemanticMask", QStringLiteral("red train"))
                && editor.semantic_prompt_count_ == 1
                && editor.last_semantic_query_ == QStringLiteral("red train"),
            "semantic selection delegates one controller-owned query transaction"
        )) {
        return EXIT_FAILURE;
    }

    editor.setAvailability(false, true, true, true, 3);
    drainBindings();
    if (!require(
            menu->property("currentNodeAvailable").toBool(),
            "a node with a mask can accept another bounded component"
        )
        || !require(
            invokeWithInt(menu.get(), "selectDestination", 0)
                && menu->property("destination").toInt() == 0,
            "an existing mask keeps the selected-node destination"
        )) {
        return EXIT_FAILURE;
    }

    menu->setProperty("componentOperation", 2);
    if (!require(
            invokeWithInt(menu.get(), "createMask", 1) && editor.create_count_ == 3
                && editor.add_component_count_ == 1 && editor.last_kind_ == 1
                && editor.last_operation_ == 2,
            "current-node creation appends one subtracting component without replacing the mask"
        )
        || !require(
            invokeWithBool(menu.get(), "startAiMask", false) && editor.subject_prompt_count_ == 2
                && editor.last_operation_ == 2 && editor.last_prefer_current_node_,
            "AI selection appends with the same selected operation"
        )) {
        return EXIT_FAILURE;
    }

    editor.setMaskComponentCount(8);
    drainBindings();
    if (!require(
            !menu->property("currentNodeAvailable").toBool(),
            "an eight-component mask closes the bounded current-node destination"
        )
        || !require(
            invokeWithInt(menu.get(), "selectDestination", 0)
                && menu->property("destination").toInt() == 1,
            "a full component stack redirects creation to a new node"
        )) {
        return EXIT_FAILURE;
    }

    if (!require(
            invokeWithInt(menu.get(), "selectDestination", 1)
                && invokeWithInt(menu.get(), "createMask", 2) && editor.create_count_ == 4
                && editor.last_kind_ == 2 && editor.last_destination_ == 1,
            "the global entry delegates one complete new-node transaction"
        )) {
        return EXIT_FAILURE;
    }

    editor.setAvailability(false, true, true, false, 0);
    drainBindings();
    if (!require(
            invokeWithInt(menu.get(), "selectDestination", 1)
                && menu->property("destination").toInt() == 0,
            "a full node stack falls back to the selected empty node"
        )
        || !require(
            invokeWithBool(menu.get(), "startAiMask", false) && editor.subject_prompt_count_ == 3
                && editor.last_operation_ == 0 && editor.last_prefer_current_node_,
            "AI selection can attach a Base component to the selected empty node"
        )) {
        return EXIT_FAILURE;
    }

    editor.create_result_ = false;
    if (!require(
            invokeWithInt(menu.get(), "createMask", 1) && editor.create_count_ == 5,
            "a rejected backend transaction is attempted exactly once"
        )) {
        return EXIT_FAILURE;
    }

    menu->setProperty("openedPhotoId", editor.photoId());
    menu->setProperty("openedRepresentationId", editor.representationId());
    menu->setProperty("openedGradeNodeId", editor.selectedGradeNodeId());
    menu->setProperty("visible", true);
    editor.setSelectedNodeId(QStringLiteral("other-node"));
    drainBindings();
    if (!require(
            !menu->property("visible").toBool(),
            "a target-node change closes the stale creation transaction"
        )) {
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

#include "precision_mask_create_menu_contract_test.moc"
