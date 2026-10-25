#include "ai_library.hpp"

#include "../ai_preferences.hpp"
#include "../image_understanding_controller.hpp"
#include "../people_analysis_controller.hpp"
#include "../review_controller.hpp"
#include "../review_model.hpp"
#include "../semantic_search_controller.hpp"
#include "../smart_category_controller.hpp"

#include <QAbstractItemModel>
#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QQmlApplicationEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTimer>
#include <QVariant>

#ifdef SHADOW_DESKTOP_UI_SMOKE
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>
#endif

#include <stdexcept>

namespace {
#ifdef SHADOW_DESKTOP_UI_SMOKE
template <typename Controller> Controller& boundController(QObject& root, const char* name) {
    auto* controller = qobject_cast<Controller*>(root.property(name).value<QObject*>());
    if (!controller)
        throw std::runtime_error("packaged AI controller is not bound to the window");
    return *controller;
}

QQuickItem* visibleItem(QObject& root, const QString& name) {
    const auto items = root.findChildren<QQuickItem*>(name);
    for (auto* item : items) {
        if (item->isVisible() && item->isEnabled() && item->width() > 1 && item->height() > 1)
            return item;
    }
    return nullptr;
}

void click(QQuickItem& item) {
    auto* window = item.window();
    if (!window)
        throw std::runtime_error("AI control has no real window");
    const auto point = item.mapToScene(QPointF(item.width() / 2, item.height() / 2)).toPoint();
    if (item.metaObject()->indexOfSignal("clicked()") >= 0) {
        QSignalSpy clicked(&item, SIGNAL(clicked()));
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, point);
        if (clicked.isEmpty())
            throw std::runtime_error("real pointer input did not click the visible AI control");
    } else {
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, point);
    }
}

class AiLibrarySmoke final : public QObject {
  public:
    AiLibrarySmoke(QQmlApplicationEngine& engine, ReviewController& review) :
        QObject(&engine), root_(*engine.rootObjects().front()), review_(review),
        preferences_(boundController<AiPreferences>(root_, "aiPreferences")),
        people_(boundController<PeopleAnalysisController>(root_, "peopleAnalysisController")),
        semantic_(boundController<SemanticSearchController>(root_, "semanticSearchController")),
        categories_(boundController<SmartCategoryController>(root_, "smartCategoryController")),
        understanding_(
            boundController<ImageUnderstandingController>(root_, "imageUnderstandingController")
        ) {
        preferences_.setImageUnderstandingBackgroundEnabled(false);
        preferences_.setImageUnderstandingAutoApplyKeywords(false);
        categories_.pause();
        elapsed_.start();
        timer_.setInterval(50);
        connect(&timer_, &QTimer::timeout, this, [this] {
            try {
                advance();
            } catch (const std::exception& error) {
                timer_.stop();
                qCritical() << "AI Library smoke failed" << static_cast<int>(stage_)
                            << error.what();
                qCritical() << "AI Library People state" << people_.statusText()
                            << people_.analyzedPhotos() << people_.detectedFaces()
                            << people_.embeddedFaces();
                QCoreApplication::exit(1);
            }
        });
        timer_.start();
    }

  private:
    enum class Stage {
        Import,
        PeopleStart,
        People,
        SearchStart,
        Search,
        Categories,
        Description,
        Classification,
        Complete
    };

    static void require(bool condition, const char* diagnostic) {
        if (!condition)
            throw std::runtime_error(diagnostic);
    }

    static void checkError(const QString& diagnostic) {
        if (!diagnostic.isEmpty())
            throw std::runtime_error(diagnostic.toStdString());
    }

    void advance() {
        require(elapsed_.elapsed() < 840'000, "real model acceptance exceeded its deadline");
        switch (stage_) {
        case Stage::Import: {
            auto* model = review_.model();
            if (review_.busy() || review_.scanning() || model->rowCount() == 0)
                return;
            require(model->rowCount() == 1, "AI acceptance requires exactly one portrait");
            if (model->data(model->index(0, 0), ReviewModel::VisualWidthRole).toInt() == 0) {
                require(elapsed_.elapsed() < 120'000, "import did not publish a usable AI visual");
                return;
            }
            photo_ = model->data(model->index(0, 0), ReviewModel::PhotoIdRole).toString();
            representation_ =
                model->data(model->index(0, 0), ReviewModel::RepresentationIdRole).toString();
            require(!photo_.isEmpty() && !representation_.isEmpty(), "missing imported identity");
            preferences_.grantPeopleAnalysisConsent();
            root_.setProperty("workspaceIndex", root_.property("peopleWorkspacePage"));
            stage_ = Stage::PeopleStart;
            return;
        }
        case Stage::PeopleStart:
            if (auto* button = visibleItem(root_, QStringLiteral("peopleStartButton"))) {
                if (!button->window() || !button->window()->isExposed())
                    return;
                click(*button);
                stage_ = Stage::People;
            }
            return;
        case Stage::People:
            if (people_.busy())
                return;
            checkError(people_.errorText());
            require(
                people_.analyzedPhotos() == 1 && people_.detectedFaces() > 0
                    && people_.embeddedFaces() > 0,
                "People UI did not publish real detection and embedding results"
            );
            qInfo() << "AI Library people passed" << people_.detectedFaces()
                    << people_.embeddedFaces();
            root_.setProperty("workspaceIndex", root_.property("reviewWorkspacePage"));
            stage_ = Stage::SearchStart;
            return;
        case Stage::SearchStart: {
            auto* input = visibleItem(root_, QStringLiteral("semanticSearchInput"));
            if (!input)
                input = visibleItem(root_, QStringLiteral("semanticSearchPopupInput"));
            if (!input) {
                if (auto* button =
                        visibleItem(root_, QStringLiteral("semanticSearchCompactButton")))
                    click(*button);
                return;
            }
            click(*input);
            input->forceActiveFocus();
            for (const QChar character : QStringLiteral("a portrait of a woman"))
                QTest::keyClick(
                    input->window(),
                    static_cast<Qt::Key>(character.toUpper().unicode())
                );
            QTest::keyClick(input->window(), Qt::Key_Return);
            stage_ = Stage::Search;
            return;
        }
        case Stage::Search:
            if (semantic_.busy())
                return;
            checkError(semantic_.errorText());
            require(
                semantic_.consideredPhotos() == 1 && semantic_.embeddedPhotos() == 1
                    && semantic_.hasResults(),
                "semantic search did not return the actual portrait embedding"
            );
            qInfo() << "AI Library semantic search passed" << semantic_.shownResultCount();
            semantic_.clearSessionResults();
            categories_.rebuild();
            stage_ = Stage::Categories;
            return;
        case Stage::Categories:
            if (categories_.busy())
                return;
            checkError(categories_.errorText());
            require(
                categories_.hasPublishedResults() && categories_.processedPhotos() == 1,
                "smart classification did not publish its real vector results"
            );
            qInfo() << "AI Library categories passed" << categories_.processedPhotos();
            preferences_.setImageUnderstandingScanScope(QStringLiteral("all"));
            preferences_.setImageUnderstandingExecutionAllowed(true);
            understanding_.refresh();
            stage_ = Stage::Description;
            return;
        case Stage::Description:
            if (understanding_.busy())
                return;
            checkError(understanding_.errorText());
            require(
                understanding_.processedPhotos() == 1,
                "image understanding did not process the imported portrait"
            );
            preferences_.setImageUnderstandingBackgroundEnabled(false);
            understanding_.loadPhotoProposal(photo_, representation_);
            require(
                understanding_.photoProposalAvailable()
                    && !understanding_.photoDescription().trimmed().isEmpty(),
                "real image description was not projected into the photo proposal"
            );
            qInfo() << "AI Library description passed" << understanding_.photoKeywords().size();
            understanding_.requestAdvancedReview(photo_, representation_);
            stage_ = Stage::Classification;
            return;
        case Stage::Classification:
            if (understanding_.advancedReviewBusy())
                return;
            checkError(understanding_.advancedReviewError());
            require(
                !understanding_.advancedReviewDisposition().isEmpty(),
                "closed-set review did not return a validated disposition"
            );
            qInfo() << "AI Library classification passed"
                    << understanding_.advancedReviewDisposition();
            stage_ = Stage::Complete;
            qInfo() << "AI Library smoke passed";
            timer_.stop();
            QCoreApplication::quit();
            return;
        case Stage::Complete:
            return;
        }
    }

    QObject& root_;
    ReviewController& review_;
    AiPreferences& preferences_;
    PeopleAnalysisController& people_;
    SemanticSearchController& semantic_;
    SmartCategoryController& categories_;
    ImageUnderstandingController& understanding_;
    QTimer timer_;
    QElapsedTimer elapsed_;
    Stage stage_ = Stage::Import;
    QString photo_;
    QString representation_;
};
#endif
} // namespace

bool DesktopSmoke::installAiLibrarySmoke(QQmlApplicationEngine& engine, ReviewController& review) {
    if (!qEnvironmentVariableIsSet("SHADOW_DESKTOP_AI_LIBRARY_SMOKE"))
        return false;
    if (qEnvironmentVariableIsEmpty("SHADOW_DESKTOP_DATA_ROOT")) {
        qCritical() << "AI Library smoke requires an explicit isolated data root";
        QTimer::singleShot(0, [] { QCoreApplication::exit(1); });
        return true;
    }
#ifdef SHADOW_DESKTOP_UI_SMOKE
    try {
        new AiLibrarySmoke(engine, review);
    } catch (const std::exception& error) {
        qCritical() << "AI Library smoke admission failed" << error.what();
        QTimer::singleShot(0, [] { QCoreApplication::exit(1); });
    }
#else
    Q_UNUSED(engine)
    Q_UNUSED(review)
    qCritical() << "AI Library smoke requires a build with UI acceptance support";
    QTimer::singleShot(0, [] { QCoreApplication::exit(1); });
#endif
    return true;
}
