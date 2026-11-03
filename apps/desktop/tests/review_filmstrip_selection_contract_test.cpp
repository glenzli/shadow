#include "justified_review_layout_model.hpp"
#include "review_filter_model.hpp"
#include "review_model.hpp"
#include <QJSValue>

#include <QGuiApplication>
#include <QImage>
#include <QMutex>
#include <QMutexLocker>
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

QQuickItem* visualItem(QQuickItem* root, const QString& name) {
    if (root->objectName() == name)
        return root;
    for (auto* child : root->childItems()) {
        if (auto* found = visualItem(child, name))
            return found;
    }
    return nullptr;
}

class FilmstripPreviewProvider final : public QQuickImageProvider {
  public:
    FilmstripPreviewProvider() : QQuickImageProvider(Image) {}
    QImage requestImage(const QString& id, QSize* size, const QSize&) override {
        {
            const QMutexLocker lock(&requests_mutex_);
            requests_.push_back(id);
        }
        QImage image(800, 600, QImage::Format_RGB32);
        image.fill(Qt::darkBlue);
        *size = image.size();
        return image;
    }

    qsizetype requestCount() const {
        const QMutexLocker lock(&requests_mutex_);
        return requests_.size();
    }

    bool requested(const QString& id) const {
        const QMutexLocker lock(&requests_mutex_);
        return requests_.contains(id);
    }

  private:
    mutable QMutex requests_mutex_;
    QStringList requests_;
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
        photo.has_visual = true;
        photo.visual_source_override =
            QStringLiteral("image://filmstrip-selection/%1").arg(photo.photo_id);
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
    auto* preview_provider = new FilmstripPreviewProvider;
    engine.addImageProvider(QStringLiteral("filmstrip-selection"), preview_provider);
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
    id: previewRoot
    width: 600; height: 420
    visible: false
    model: photoModel
    review: QtObject {
        property string selectedPhotoId: "photo-1500"
        property string selectedRepresentationId: "raw"
        property string selectedVisualSource: "image://filmstrip-selection/preview"
        property bool selectedVisualAutoTransform: false
        property bool selectedSourceAvailable: true
        id: reviewState
        property bool visible: true
        property real width: 600
        property real height: 420
        property bool canMutateDecision: true
        property bool canOpenSelectedPhoto: true
        property int selectedPhotoCount: 1
        property string selectedDecisionFlag: "unflagged"
        property int selectedDecisionRating: 0
        property bool selectedLiked: false
        property string selectedColorLabel: "none"
        property var manualLibraryAlbums: []
        property var smartCategoryController: ({hasPublishedResults: false})
        property var adjustmentTransferController: QtObject {
            property bool hasClipboard: true
            property bool admissionEnabled: true
            property bool busy: false
            property bool copying: false
            property string sourcePhotoId: "copied-source"
            property var appliedTargets: []
            function applyToPhotos(targets) { appliedTargets = targets }
        }
        property var copiedTarget: null
        function copyPhotoStyleRequested(target) { copiedTarget = target }
        function batchSelectionTargets() {
            return [{photoId: selectedPhotoId, sourcePath: "/isolated/test.dng", title: "Target"}]
        }
        function selectionContainsRemote() { return false }
        function snapshotForCard(card) {
            return {photoId: card.photoId, representationId: card.representationId,
                visualSource: card.visualSource}
        }
        function sharedNodeQuickList() { return [] }
        property var justifiedReviewLayout: navigationModel
        property QtObject controller: QtObject {
            property bool hasMore: false
            property bool scanning: false
            property bool refreshing: false
            property bool busy: false
            property bool loadingMore: false
            property bool comparisonBusy: false
            property bool decisionBusy: false
            property bool libraryAlbumsBusy: false
            property bool remoteLibraryBusy: false
            function refreshSharedGradeNodes() {}
            function setPhotoLiked(photoId, liked) { reviewState.selectedLiked = liked }
            property int loadCount: 0
            property var priorityIds: []
            property string prioritySelected: ""
            signal comparisonStateChanged()
            signal decisionStateChanged()
            function loadMore() { ++loadCount; loadingMore = true }
            function prioritizeVisibleRefinements(selected, ids) {
                prioritySelected = selected
                priorityIds = ids
            }
        }
        property var comparison: ({compareMode: false})
        property var culling: ({arenaActive: false, containsCandidate: function() { return false }})
        property int opened: 0
        function selectedVisualSnapshot() { return ({}) }
        function focusGallery() { previewRoot.forceGalleryFocus() }
        function setSelectedRating(value) { selectedDecisionRating = value }
        function setSelectedFlag(value) { selectedDecisionFlag = value }
        function hasMoreSharedNodes() { return false }
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
    QTest::qWait(150);
    check(preview_provider->requestCount() == 0, "hidden initial preview requested images");
    review->setProperty(
        "selectedVisualSource",
        QStringLiteral("image://filmstrip-selection/initial-current")
    );
    QTest::qWait(150);
    check(preview_provider->requestCount() == 0, "hidden initial selection requested an image");
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
    check(
        QTest::qWaitFor([&] {
            return preview_provider->requested(QStringLiteral("initial-current"))
                   && preview_provider->requested(QStringLiteral("photo-1500"));
        }),
        "showing the preview did not load the current hero and filmstrip images"
    );
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
    // Let requests admitted before hiding finish, then change both hidden surfaces' sources.
    QTest::qWait(150);
    const auto hidden_request_count = preview_provider->requestCount();
    review->setProperty("selectedPhotoId", QStringLiteral("photo-800"));
    review->setProperty(
        "selectedVisualSource",
        QStringLiteral("image://filmstrip-selection/hidden-current")
    );
    for (auto& photo : photos)
        photo.visual_source_override += QStringLiteral("-hidden");
    source.replace(photos, 3);
    QTest::qWait(150);
    check(
        preview_provider->requestCount() == hidden_request_count,
        "hidden selection or filmstrip refresh requested images"
    );
    root_item->setVisible(true);
    check(selection_visible(), "selection changed in grid was not revealed on reentry");
    check(
        QTest::qWaitFor([&] {
            return preview_provider->requested(QStringLiteral("hidden-current"))
                   && preview_provider->requested(QStringLiteral("photo-800-hidden"));
        }),
        "reentering the preview did not load the latest hero and filmstrip images"
    );
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
    // Pointer curation commands must leave all four navigation keys usable.
    for (const auto& action :
         {std::pair{"reviewLikeButton", Qt::Key_Right},
          std::pair{"reviewRatingButton5", Qt::Key_Down},
          std::pair{"reviewPickButton", Qt::Key_Up}}) {
        auto* button = visualItem(window.contentItem(), QString::fromLatin1(action.first));
        check(button && button->isVisible() && button->isEnabled(), "decision action unreachable");
        if (!button)
            continue;
        const int direction = action.second == Qt::Key_Up ? -1 : 1;
        const auto expected = navigation.navigationTarget(
            review->property("selectedPhotoId").toString(),
            QStringLiteral("raw"),
            direction,
            0
        );
        QTest::mouseClick(
            &window,
            Qt::LeftButton,
            Qt::NoModifier,
            button->mapToScene(button->boundingRect().center()).toPoint()
        );
        QTest::keyClick(&window, action.second);
        check(
            review->property("selectedPhotoId") == expected.value("photoId"),
            "pointer decision action stole subsequent arrow navigation"
        );
        check(selection_visible(), "decision navigation lost visible selection");
    }
    check(review->property("selectedLiked").toBool(), "Like click did not execute");
    check(review->property("selectedDecisionRating").toInt() == 5, "rating click did not execute");
    auto* position = root->findChild<QQuickItem*>(QStringLiteral("filmstripPosition"));
    auto* scroll_bar = root->findChild<QQuickItem*>(QStringLiteral("filmstripScrollBar"));
    check(
        position && position->isVisible() && scroll_bar && scroll_bar->isVisible(),
        "filmstrip position and persistent scroll affordance missing"
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
    if (card) {
        const auto point = card->mapToScene(card->boundingRect().center()).toPoint();
        QTest::mouseClick(&window, Qt::RightButton, Qt::NoModifier, point);
        auto* menu = root->findChild<QObject*>(QStringLiteral("filmstripPhotoContextMenu"));
        check(
            menu && QTest::qWaitFor([&] { return menu->property("opened").toBool(); }),
            "filmstrip right click did not open the photo menu"
        );
        check(
            menu && menu->property("photoId") == review->property("selectedPhotoId"),
            "filmstrip menu did not capture the clicked photo identity"
        );
        auto* paste =
            visualItem(window.contentItem(), QStringLiteral("pasteAdjustmentsToSelectionAction"));
        if (paste) {
            check(paste->isVisible() && paste->isEnabled(), "filmstrip paste action disabled");
            auto* copy = visualItem(window.contentItem(), QStringLiteral("copyPhotoStyleAction"));
            check(
                copy && QTest::qWaitFor([&] {
                    const auto paste_rect = paste->mapRectToScene(paste->boundingRect());
                    const auto copy_rect = copy->mapRectToScene(copy->boundingRect());
                    return paste_rect.top() >= copy_rect.bottom()
                           && copy_rect.top() >= menu->property("y").toReal()
                           && paste_rect.bottom() <= window.height();
                }),
                "photo menu actions did not settle inside the window"
            );
            check(
                menu->property("y").toReal() >= 0
                    && menu->property("y").toReal() + menu->property("height").toReal()
                           <= window.height(),
                "photo menu exceeded the window"
            );
            QTest::mouseClick(
                &window,
                Qt::LeftButton,
                Qt::NoModifier,
                paste->mapToScene(paste->boundingRect().center()).toPoint()
            );
            auto* transfer = review->property("adjustmentTransferController").value<QObject*>();
            check(
                QTest::qWaitFor([&] {
                    return !transfer->property("appliedTargets")
                                .value<QJSValue>()
                                .toVariant()
                                .toList()
                                .isEmpty();
                }),
                "filmstrip paste click was not delivered"
            );
            const auto targets =
                transfer->property("appliedTargets").value<QJSValue>().toVariant().toList();
            check(
                targets.size() == 1
                    && targets.first().toMap().value("photoId")
                           == review->property("selectedPhotoId"),
                "filmstrip paste did not target the captured selection"
            );
        } else {
            check(false, "filmstrip Paste style action missing");
        }
        if (menu)
            QMetaObject::invokeMethod(menu, "close");
    }
    auto* content = strip->property("contentItem").value<QQuickItem*>();
    check(
        content && content->childItems().size() < 100,
        "selection synchronization instantiated the whole library"
    );
    auto* controller = review->property("controller").value<QObject*>();
    check(controller != nullptr, "filmstrip paging controller is unavailable");
    if (controller) {
        QMetaObject::invokeMethod(root.get(), "publishVisibleRefinements");
        const auto priority_ids =
            controller->property("priorityIds").value<QJSValue>().toVariant().toList();
        check(
            controller->property("prioritySelected").toString()
                    == review->property("selectedRepresentationId").toString()
                && !priority_ids.isEmpty() && priority_ids.size() < 30,
            "filmstrip priority must include the active photo and only the visible strip"
        );
        QVector<ReviewItem> page;
        for (int index = 0; index < 2; ++index) {
            ReviewItem photo;
            photo.photo_id = QStringLiteral("page-%1").arg(index);
            photo.representation_id = QStringLiteral("raw");
            photo.title = photo.photo_id;
            page.push_back(photo);
        }
        source.replace(page, 4);
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
            const auto target =
                navigation.navigationTarget(QStringLiteral("page-1"), QStringLiteral("raw"), 1, 0);
            std::cerr << "page boundary state: source=" << source.rowCount()
                      << " filtered=" << filtered.rowCount() << " layout=" << navigation.rowCount()
                      << " pending="
                      << root->property("pendingForwardPhotoId").toString().toStdString()
                      << " selected="
                      << review->property("selectedPhotoId").toString().toStdString() << " next="
                      << target.value(QStringLiteral("photoId")).toString().toStdString() << '\n';
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
