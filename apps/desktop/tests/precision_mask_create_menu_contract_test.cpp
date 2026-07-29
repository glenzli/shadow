#include <QCoreApplication>
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

    bool create_result_ = true;
    int create_count_ = 0;
    int last_kind_ = -1;
    int last_destination_ = -1;

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

void drainBindings() {
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents();
}

} // namespace

int main(int argc, char* argv[]) {
    QGuiApplication application(argc, argv);
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
    if (!require(
            destination_selector != nullptr && brush_action != nullptr
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
        )) {
        return EXIT_FAILURE;
    }

    editor.setAvailability(false, true, true, true, 3);
    drainBindings();
    if (!require(
            !menu->property("currentNodeAvailable").toBool(),
            "a node with a mask cannot accept a replacement creation"
        )
        || !require(
            invokeWithInt(menu.get(), "selectDestination", 0)
                && menu->property("destination").toInt() == 1,
            "an existing mask redirects creation to a new node"
        )) {
        return EXIT_FAILURE;
    }

    menu->setProperty("destination", 0);
    if (!require(
            invokeWithInt(menu.get(), "createMask", 1) && editor.create_count_ == 3,
            "forcing the unavailable current destination cannot replace a mask"
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
