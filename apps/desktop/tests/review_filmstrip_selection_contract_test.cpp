#include "justified_review_layout_model.hpp"
#include "review_filter_model.hpp"
#include "review_model.hpp"

#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTest>
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <memory>

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    ReviewModel source;
    QVector<ReviewItem> photos;
    for (int index = 0; index < 2000; ++index) {
        ReviewItem photo;
        photo.photo_id = QStringLiteral("photo-%1").arg(index);
        photo.representation_id = QStringLiteral("raw");
        photo.title = photo.photo_id;
        photo.decision_rating = index % 2;
        photos.push_back(photo);
    }
    source.replace(photos, 1);
    ReviewFilterModel filtered;
    filtered.setSourceModel(&source);
    JustifiedReviewLayoutModel navigation;
    navigation.setSourceModel(&filtered);
    navigation.setAvailableWidth(600);
    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("photoModel"), &filtered);
    engine.rootContext()->setContextProperty(QStringLiteral("navigationModel"), &navigation);
    bool warnings = false;
    QObject::connect(&engine, &QQmlEngine::warnings, [&](const QList<QQmlError>& errors) {
        warnings = true;
        for (const auto& error : errors)
            std::cerr << error.toString().toStdString() << '\n';
    });
    QQmlComponent component(&engine);
    component.setData(
        R"QML(
import QtQuick
import Shadow.ReviewFilmstripContract
ReviewSinglePreview {
    width: 600; height: 420
    visible: false
    model: photoModel
    review: QtObject {
        property string selectedPhotoId: "photo-1500"
        property string selectedRepresentationId: "raw"
        property string selectedVisualSource: ""
        property bool selectedVisualAutoTransform: false
        property bool selectedSourceAvailable: true
        property bool canMutateDecision: false
        property int selectedPhotoCount: 1
        property string selectedDecisionFlag: "unflagged"
        property int selectedDecisionRating: 0
        property bool selectedLiked: false
        property string selectedColorLabel: "none"
        property var justifiedReviewLayout: navigationModel
        property var comparison: ({compareMode: false})
        property var culling: ({arenaActive: false, containsCandidate: function() { return false }})
        property int opened: 0
        function selectedVisualSnapshot() { return ({}) }
        function isPhotoSelected(photo, representation) {
            return photo === selectedPhotoId && representation === selectedRepresentationId
        }
        function selectPhoto(photo, modifiers) {
            selectedPhotoId = photo.photoId
            selectedRepresentationId = photo.representationId
        }
        function openSelectedPhoto() { ++opened }
    }
}
)QML",
        QUrl{}
    );
    std::unique_ptr<QObject> root(component.create());
    if (!root) {
        std::cerr << component.errorString().toStdString();
        return EXIT_FAILURE;
    }
    QQuickWindow window;
    window.resize(600, 420);
    auto* root_item = qobject_cast<QQuickItem*>(root.get());
    root_item->setParentItem(window.contentItem());
    window.show();
    auto* review = root->property("review").value<QObject*>();
    auto* strip = root->findChild<QQuickItem*>(QStringLiteral("reviewFilmstrip"));
    bool ok = review && strip;
    auto check = [&](bool condition, const char* message) {
        if (!condition)
            std::cerr << message << '\n';
        ok &= condition;
    };
    if (!ok)
        return EXIT_FAILURE;
    auto selection_visible = [&] {
        return QTest::qWaitFor(
            [&] {
                auto* card = strip->property("currentItem").value<QQuickItem*>();
                if (!card || card->property("photoId") != review->property("selectedPhotoId"))
                    return false;
                const qreal left = card->mapToItem(strip, QPointF{}).x();
                return left >= -1 && left + card->width() <= strip->width() + 1;
            },
            3000
        );
    };
    root_item->setVisible(true);
    check(selection_visible(), "entering filmstrip did not reveal an initially offscreen photo");
    review->setProperty("selectedPhotoId", QStringLiteral("photo-20"));
    check(selection_visible(), "backward selection did not reveal the selected photo");
    review->setProperty("selectedPhotoId", QStringLiteral("photo-1700"));
    check(selection_visible(), "forward selection outside the delegate cache was lost");
    check(
        filtered.indexOfPhoto(QStringLiteral("photo-1700"), QStringLiteral("jpeg")) == -1,
        "row lookup confused different representations of the same photo"
    );
    filtered.setMinimumRating(1);
    check(
        QTest::qWaitFor([&] { return strip->property("currentIndex").toInt() == -1; }),
        "a filtered-out photo retained an unrelated current row"
    );
    filtered.clearFilters();
    check(selection_visible(), "restoring a filter did not reveal the retained selection");
    std::reverse(photos.begin(), photos.end());
    source.replace(photos, 2);
    check(selection_visible(), "same-count model reset did not reconcile the current row");
    root_item->setVisible(false);
    review->setProperty("selectedPhotoId", QStringLiteral("photo-800"));
    QCoreApplication::processEvents();
    root_item->setVisible(true);
    check(selection_visible(), "selection changed in grid was not revealed on reentry");
    check(QTest::qWaitFor([&] { return navigation.rowCount() > 0; }), "navigation layout missing");
    const auto next =
        navigation.navigationTarget(QStringLiteral("photo-800"), QStringLiteral("raw"), 1, 0);
    QMetaObject::invokeMethod(root.get(), "forceGalleryFocus");
    QTest::keyClick(&window, Qt::Key_Right);
    check(
        review->property("selectedPhotoId") == next.value(QStringLiteral("photoId")),
        "keyboard navigation failed to select the adjacent photo"
    );
    check(selection_visible(), "keyboard navigation did not reveal the selected photo");
    auto* card = strip->property("currentItem").value<QQuickItem*>();
    if (card) {
        const auto point =
            card->mapToScene(QPointF(card->width() / 2, card->height() / 2)).toPoint();
        const auto scroll = strip->property("contentX").toDouble();
        QTest::mouseDClick(&window, Qt::LeftButton, Qt::NoModifier, point);
        QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier, point);
        QCoreApplication::processEvents();
        check(
            review->property("opened").toInt() == 1,
            "double click did not open the selected photo"
        );
        check(
            qAbs(strip->property("contentX").toDouble() - scroll) < 1,
            "pointer selection moved the double-click target"
        );
    }
    auto* content = strip->property("contentItem").value<QQuickItem*>();
    check(
        content && content->childItems().size() < 100,
        "selection synchronization instantiated the whole library"
    );
    return ok && !warnings ? EXIT_SUCCESS : EXIT_FAILURE;
}
