#include "justified_review_layout_model.hpp"
#include "review_filter_model.hpp"
#include "review_model.hpp"

#include <QGuiApplication>
#include <QImage>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickImageProvider>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTest>
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <memory>

class FilmstripPreviewProvider final : public QQuickImageProvider {
  public:
    FilmstripPreviewProvider() : QQuickImageProvider(Image) {}
    QImage requestImage(const QString&, QSize* size, const QSize&) override {
        QImage image(800, 600, QImage::Format_RGB32);
        image.fill(Qt::darkBlue);
        *size = image.size();
        return image;
    }
};

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
    engine.addImageProvider(QStringLiteral("filmstrip-selection"), new FilmstripPreviewProvider);
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
        property string selectedVisualSource: "image://filmstrip-selection/preview"
        property bool selectedVisualAutoTransform: false
        property bool selectedSourceAvailable: true
        property bool canMutateDecision: false
        property int selectedPhotoCount: 1
        property string selectedDecisionFlag: "unflagged"
        property int selectedDecisionRating: 0
        property bool selectedLiked: false
        property string selectedColorLabel: "none"
        property var justifiedReviewLayout: navigationModel
        property QtObject controller: QtObject {
            property bool hasMore: false
            property bool scanning: false
            property bool refreshing: false
            property bool busy: false
            property bool loadingMore: false
            property bool comparisonBusy: false
            property bool decisionBusy: false
            property int loadCount: 0
            signal comparisonStateChanged()
            signal decisionStateChanged()
            function loadMore() { ++loadCount; loadingMore = true }
        }
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
    auto next_photo = [&] {
        return navigation
            .navigationTarget(
                review->property("selectedPhotoId").toString(),
                QStringLiteral("raw"),
                1,
                0
            )
            .value(QStringLiteral("photoId"));
    };
    auto* viewport = root->findChild<QQuickItem*>(QStringLiteral("reviewPreviewViewport"));
    check(
        QTest::qWaitFor([&] { return viewport->property("imageReady").toBool(); }),
        "main preview did not load"
    );
    const auto after_preview = next_photo();
    QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, QPoint(250, 120));
    QTest::keyClick(&window, Qt::Key_Right);
    check(
        review->property("selectedPhotoId") == after_preview,
        "clicking the main preview disabled arrow-key culling"
    );
    check(selection_visible(), "main preview navigation lost the filmstrip selection");
    auto* zoom = root->findChild<QQuickItem*>(QStringLiteral("reviewPreviewActualSize"));
    const auto after_zoom = next_photo();
    QTest::mouseClick(
        &window,
        Qt::LeftButton,
        Qt::NoModifier,
        zoom->mapToScene(QPointF(zoom->width() / 2, zoom->height() / 2)).toPoint()
    );
    check(!viewport->property("fitView").toBool(), "100% zoom did not activate");
    QTest::keyClick(&window, Qt::Key_Right);
    check(
        review->property("selectedPhotoId") == after_zoom,
        "using a zoom button disabled arrow-key culling"
    );
    check(
        selection_visible() && viewport->property("fitView").toBool(),
        "next photo must reveal its thumbnail and reset the previous zoom"
    );
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
    auto* controller = review->property("controller").value<QObject*>();
    check(controller != nullptr, "filmstrip paging controller is unavailable");
    if (controller) {
        QVector<ReviewItem> page;
        for (int index = 0; index < 2; ++index) {
            ReviewItem photo;
            photo.photo_id = QStringLiteral("page-%1").arg(index);
            photo.representation_id = QStringLiteral("raw");
            photo.title = photo.photo_id;
            page.push_back(photo);
        }
        source.replace(page, 3);
        review->setProperty("selectedPhotoId", QStringLiteral("page-1"));
        check(selection_visible(), "page-boundary selection was not visible");
        controller->setProperty("hasMore", true);
        QMetaObject::invokeMethod(root.get(), "forceGalleryFocus");
        QTest::keyClick(&window, Qt::Key_Right);
        check(
            controller->property("loadCount").toInt() == 1
                && review->property("selectedPhotoId") == QStringLiteral("page-1"),
            "right arrow did not request the next page while retaining the current photo"
        );
        auto* page_status = root->findChild<QQuickItem*>(QStringLiteral("filmstripPageStatus"));
        check(page_status && page_status->isVisible(), "filmstrip page load has no visible status");
        ReviewItem next_photo;
        next_photo.photo_id = QStringLiteral("page-2");
        next_photo.representation_id = QStringLiteral("raw");
        next_photo.title = next_photo.photo_id;
        source.append({next_photo});
        controller->setProperty("hasMore", false);
        controller->setProperty("loadingMore", false);
        const bool advanced = QTest::qWaitFor([&] {
            return review->property("selectedPhotoId") == QStringLiteral("page-2");
        });
        if (!advanced) {
            const auto target = navigation.navigationTarget(
                QStringLiteral("page-1"), QStringLiteral("raw"), 1, 0);
            std::cerr << "page boundary state: source=" << source.rowCount()
                      << " filtered=" << filtered.rowCount()
                      << " layout=" << navigation.rowCount()
                      << " pending="
                      << root->property("pendingForwardPhotoId").toString().toStdString()
                      << " selected="
                      << review->property("selectedPhotoId").toString().toStdString()
                      << " next="
                      << target.value(QStringLiteral("photoId")).toString().toStdString()
                      << '\n';
        }
        check(advanced, "pending right arrow did not advance after the next page arrived");
        check(selection_visible(), "next-page photo was not revealed in the filmstrip");
        check(
            controller->property("loadCount").toInt() == 1,
            "a single page-boundary action requested duplicate pages"
        );
        controller->setProperty("hasMore", true);
        QMetaObject::invokeMethod(root.get(), "forceGalleryFocus");
        QTest::keyClick(&window, Qt::Key_Right);
        check(
            controller->property("loadCount").toInt() == 2,
            "the next page-boundary action did not request another page"
        );
        review->setProperty("selectedPhotoId", QStringLiteral("page-0"));
        ReviewItem later_photo;
        later_photo.photo_id = QStringLiteral("page-3");
        later_photo.representation_id = QStringLiteral("raw");
        later_photo.title = later_photo.photo_id;
        source.append({later_photo});
        controller->setProperty("hasMore", false);
        controller->setProperty("loadingMore", false);
        QCoreApplication::processEvents();
        check(
            review->property("selectedPhotoId") == QStringLiteral("page-0"),
            "a stale pending arrow overrode the user's newer selection"
        );
    }
    return ok && !warnings ? EXIT_SUCCESS : EXIT_FAILURE;
}
