#include <QCoreApplication>
#include <QGuiApplication>
#include <QMetaObject>
#include <QObject>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTest>
#include <QVariantList>

#include <cstdlib>
#include <iostream>
#include <memory>

class FakeSmartCategoryController final : public QObject {
    Q_OBJECT

  public:
    Q_INVOKABLE QVariantList
    feedbackCategories(const QString& photo_id, const QString& representation_id) const {
        if (photo_id != QStringLiteral("photo-a")
            || representation_id != QStringLiteral("representation-a")) {
            return {};
        }
        return {
            QVariantMap{
                {QStringLiteral("id"), QStringLiteral("portrait")},
                {QStringLiteral("name"), QStringLiteral("Portrait")},
                {QStringLiteral("uncertain"), true},
                {QStringLiteral("matched"), true},
            },
            QVariantMap{
                {QStringLiteral("id"), QStringLiteral("travel")},
                {QStringLiteral("name"), QStringLiteral("Travel")},
                {QStringLiteral("uncertain"), true},
                {QStringLiteral("matched"), false},
            },
        };
    }

    Q_INVOKABLE bool completeReview(
        const QString& photo_id,
        const QString& representation_id,
        const QVariantList& decisions
    ) {
        last_photo_id = photo_id;
        last_representation_id = representation_id;
        last_decisions = decisions;
        ++completion_count;
        return !fail_completion;
    }

    QString last_photo_id;
    QString last_representation_id;
    QVariantList last_decisions;
    int completion_count = 0;
    bool fail_completion = false;
};

class FakeImageUnderstandingController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool advancedReviewBusy READ advancedReviewBusy NOTIFY advancedReviewChanged)
    Q_PROPERTY(
        QString advancedReviewPhotoId READ advancedReviewPhotoId NOTIFY advancedReviewChanged
    )
    Q_PROPERTY(
        QString advancedReviewRepresentationId READ advancedReviewRepresentationId NOTIFY
            advancedReviewChanged
    )
    Q_PROPERTY(
        QString advancedReviewDisposition READ advancedReviewDisposition NOTIFY
            advancedReviewChanged
    )
    Q_PROPERTY(
        QString advancedReviewCategoryId READ advancedReviewCategoryId NOTIFY advancedReviewChanged
    )
    Q_PROPERTY(
        QString advancedReviewCategoryName READ advancedReviewCategoryName NOTIFY
            advancedReviewChanged
    )
    Q_PROPERTY(QString advancedReviewError READ advancedReviewError NOTIFY advancedReviewChanged)
    Q_PROPERTY(bool photoProposalAvailable READ photoProposalAvailable NOTIFY photoProposalChanged)
    Q_PROPERTY(QString photoProposalPhotoId READ photoProposalPhotoId NOTIFY photoProposalChanged)
    Q_PROPERTY(
        QString photoProposalRepresentationId READ photoProposalRepresentationId NOTIFY
            photoProposalChanged
    )
    Q_PROPERTY(QString photoDescription READ photoDescription NOTIFY photoProposalChanged)
    Q_PROPERTY(QStringList photoKeywords READ photoKeywords NOTIFY photoProposalChanged)
    Q_PROPERTY(
        QString photoProposalDisposition READ photoProposalDisposition NOTIFY photoProposalChanged
    )

  public:
    [[nodiscard]] bool advancedReviewBusy() const noexcept {
        return false;
    }
    [[nodiscard]] QString advancedReviewPhotoId() const {
        return advanced_photo_id_;
    }
    [[nodiscard]] QString advancedReviewRepresentationId() const {
        return advanced_representation_id_;
    }
    [[nodiscard]] QString advancedReviewDisposition() const {
        return advanced_disposition_;
    }
    [[nodiscard]] QString advancedReviewCategoryId() const {
        return advanced_category_id_;
    }
    [[nodiscard]] QString advancedReviewCategoryName() const {
        return advanced_category_name_;
    }
    [[nodiscard]] QString advancedReviewError() const {
        return {};
    }
    [[nodiscard]] bool photoProposalAvailable() const noexcept {
        return false;
    }
    [[nodiscard]] QString photoProposalPhotoId() const {
        return {};
    }
    [[nodiscard]] QString photoProposalRepresentationId() const {
        return {};
    }
    [[nodiscard]] QString photoDescription() const {
        return {};
    }
    [[nodiscard]] QStringList photoKeywords() const {
        return {};
    }
    [[nodiscard]] QString photoProposalDisposition() const {
        return {};
    }

    void offerPortraitSuggestion() {
        advanced_photo_id_ = QStringLiteral("photo-a");
        advanced_representation_id_ = QStringLiteral("representation-a");
        advanced_disposition_ = QStringLiteral("matched");
        advanced_category_id_ = QStringLiteral("portrait");
        advanced_category_name_ = QStringLiteral("Portrait");
        emit advancedReviewChanged();
    }

    Q_INVOKABLE void loadPhotoProposal(const QString&, const QString&) {}
    Q_INVOKABLE void requestAdvancedReview(const QString&, const QString&) {}
    Q_INVOKABLE void acceptAdvancedReview() {
        if (advanced_disposition_ != QStringLiteral("matched"))
            return;
        advanced_disposition_ = QStringLiteral("accepted");
        ++accepted_count;
        emit advancedReviewChanged();
    }
    Q_INVOKABLE void dismissAdvancedReview() {
        advanced_disposition_ = QStringLiteral("dismissed");
        emit advancedReviewChanged();
    }
    Q_INVOKABLE void acceptPhotoKeywords() {}

    int accepted_count = 0;

  signals:
    void advancedReviewChanged();
    void photoProposalChanged();

  private:
    QString advanced_photo_id_;
    QString advanced_representation_id_;
    QString advanced_disposition_;
    QString advanced_category_id_;
    QString advanced_category_name_;
};

class FakeReviewWorkspace final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QObject* smartCategoryController READ smartCategoryController CONSTANT)
    Q_PROPERTY(QObject* imageUnderstandingController READ imageUnderstandingController CONSTANT)
    Q_PROPERTY(double width READ width CONSTANT)
    Q_PROPERTY(double height READ height CONSTANT)

  public:
    FakeReviewWorkspace(QObject* smart_categories, QObject* image_understanding) :
        smart_categories_(smart_categories), image_understanding_(image_understanding) {}

    [[nodiscard]] QObject* smartCategoryController() const noexcept {
        return smart_categories_;
    }
    [[nodiscard]] QObject* imageUnderstandingController() const noexcept {
        return image_understanding_;
    }
    [[nodiscard]] double width() const noexcept {
        return 1'200.0;
    }
    [[nodiscard]] double height() const noexcept {
        return 900.0;
    }

  private:
    QObject* smart_categories_;
    QObject* image_understanding_;
};

namespace {

[[nodiscard]] bool require(const bool condition, const char* const message) {
    if (!condition)
        std::cerr << "Smart-category review popup contract failed: " << message << '\n';
    return condition;
}

void drainBindings() {
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents();
}

void click(QQuickWindow& window, QQuickItem& item) {
    const QPointF center = item.mapToScene(QPointF(item.width() / 2.0, item.height() / 2.0));
    QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, center.toPoint());
    drainBindings();
}

[[nodiscard]] bool
itemFitsWindow(const QQuickItem& item, const QQuickWindow& window, const qreal margin) {
    const QPointF top_left = item.mapToScene(QPointF(0.0, 0.0));
    const QPointF bottom_right = item.mapToScene(QPointF(item.width(), item.height()));
    return top_left.x() >= margin && top_left.y() >= margin
           && bottom_right.x() <= static_cast<qreal>(window.width()) - margin
           && bottom_right.y() <= static_cast<qreal>(window.height()) - margin;
}

} // namespace

int main(int argc, char* argv[]) {
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QGuiApplication application(argc, argv);
    FakeSmartCategoryController smart_categories;
    FakeImageUnderstandingController image_understanding;
    FakeReviewWorkspace workspace(&smart_categories, &image_understanding);

    QQmlEngine engine;
    QQmlComponent component{&engine};
    component.setData(
        R"QML(
import QtQuick
import QtQuick.Controls
import Shadow.SmartCategoryFeedbackContract

ApplicationWindow {
    id: host
    width: 900
    height: 360
    visible: true
    property var workspaceObject

    Rectangle {
        id: anchor
        width: 120
        height: 80
        anchors.right: parent.right
        anchors.bottom: parent.bottom
    }

    ReviewSmartCategoryFeedbackPopup { id: reviewPopup }

    function presentReview() {
        reviewPopup.openFor(
            anchor, workspaceObject, "photo-a", "representation-a", "Photo A")
    }

    Component.onCompleted: presentReview()
}
)QML",
        QUrl(QStringLiteral("qrc:/tests/smart-category-review-host.qml"))
    );
    std::unique_ptr<QObject> host{component.createWithInitialProperties({
        {QStringLiteral("workspaceObject"), QVariant::fromValue(&workspace)},
    })};
    if (!host) {
        std::cerr << component.errorString().toStdString();
        return EXIT_FAILURE;
    }
    auto* const window = qobject_cast<QQuickWindow*>(host.get());
    auto* const popup = host->findChild<QObject*>(QStringLiteral("smartCategoryReviewPopup"));
    auto* const complete_button =
        host->findChild<QQuickItem*>(QStringLiteral("completeSmartCategoryReviewButton"));
    auto* const completion_error =
        host->findChild<QQuickItem*>(QStringLiteral("smartCategoryReviewCompletionError"));
    auto* const body_scroll =
        host->findChild<QQuickItem*>(QStringLiteral("smartCategoryReviewBodyScroll"));
    drainBindings();
    if (!require(
            window && popup && complete_button && completion_error && body_scroll,
            "the packaged popup exposes its completion action"
        )
        || !require(
            popup->property("visible").toBool() && complete_button->isEnabled(),
            "an unchanged uncertain classification can be completed"
        )
        || !require(
            popup->property("y").toDouble() >= 8.0
                && popup->property("y").toDouble() + popup->property("height").toDouble()
                       <= static_cast<double>(window->height()) - 8.0,
            "the popup is constrained to the real window overlay near the bottom edge"
        )
        || !require(
            popup->property("height").toDouble() < popup->property("implicitHeight").toDouble()
                && body_scroll->height() > 0.0 && itemFitsWindow(*complete_button, *window, 8.0),
            "overflow scrolls inside the popup while the completion action remains reachable"
        )) {
        return EXIT_FAILURE;
    }

    smart_categories.fail_completion = true;
    click(*window, *complete_button);
    if (!require(
            smart_categories.completion_count == 1 && smart_categories.last_decisions.size() == 2,
            "completion includes every uncertain choice even when no checkbox changed"
        )
        || !require(
            popup->property("visible").toBool() && completion_error->isVisible(),
            "a failed durable completion keeps the review open and reports the failure"
        )) {
        return EXIT_FAILURE;
    }
    smart_categories.fail_completion = false;
    click(*window, *complete_button);
    if (!require(
            smart_categories.completion_count == 2 && !popup->property("visible").toBool(),
            "the popup closes only after the whole review succeeds"
        )) {
        return EXIT_FAILURE;
    }

    image_understanding.offerPortraitSuggestion();
    if (!require(
            QMetaObject::invokeMethod(host.get(), "presentReview"),
            "the next uncertain photo review can be opened"
        )) {
        return EXIT_FAILURE;
    }
    drainBindings();
    auto* const suggestion_button =
        host->findChild<QQuickItem*>(QStringLiteral("acceptAdvancedSmartCategorySuggestionButton"));
    if (!require(
            suggestion_button && suggestion_button->isVisible(),
            "an advanced suggestion exposes its explicit acceptance action"
        )) {
        return EXIT_FAILURE;
    }
    click(*window, *suggestion_button);
    if (!require(
            image_understanding.accepted_count == 1 && popup->property("visible").toBool(),
            "accepting one overlapping category keeps remaining uncertainties open"
        )) {
        return EXIT_FAILURE;
    }
    click(*window, *complete_button);
    if (!require(
            smart_categories.completion_count == 3 && smart_categories.last_decisions.size() == 1
                && smart_categories.last_decisions.front()
                           .toMap()
                           .value(QStringLiteral("categoryId"))
                           .toString()
                       == QStringLiteral("travel"),
            "the final review only completes the uncertainty not already accepted"
        )) {
        return EXIT_FAILURE;
    }
    return require(
               !popup->property("visible").toBool(),
               "the fully resolved review leaves the queue presentation"
           )
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
}

#include "review_smart_category_feedback_popup_test.moc"
