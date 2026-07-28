#include "scenario_setup.hpp"

#include "../edit_controller.hpp"
#include "../review_controller.hpp"
#include "../thumbnail_provider.hpp"

#include <QAbstractItemModel>
#include <QApplication>
#include <QDebug>
#include <QMetaObject>
#include <QModelIndex>
#include <QQmlApplicationEngine>
#include <QSize>
#include <QTimer>
#include <QUrl>
#include <QVariant>

#include <memory>

namespace DesktopSmoke {
namespace {

[[nodiscard]] QString imageProviderRequestId(const QString& source) {
    const QUrl url(source);
    QString id = url.path();
    if (id.startsWith(QLatin1Char('/'))) {
        id.remove(0, 1);
    }
    const QString query = url.query(QUrl::FullyEncoded);
    if (!query.isEmpty()) {
        id += QLatin1Char('?');
        id += query;
    }
    return id;
}

} // namespace

void installFirstPhotoOpener(
    QApplication& application,
    QQmlApplicationEngine& engine,
    ReviewController& controller,
    EditController& editor
) {
    const auto open_first_available = [&controller, &editor, &engine]() {
        auto* const model = controller.reviewModel();
        if (editor.active() || model->rowCount() == 0) {
            return;
        }
        const QModelIndex first = model->index(0, 0);
        editor.openPhoto(
            model->data(first, ReviewModel::PhotoIdRole).toString(),
            model->data(first, ReviewModel::RepresentationIdRole).toString(),
            model->data(first, ReviewModel::SourcePathRole).toString(),
            model->data(first, ReviewModel::TitleRole).toString()
        );
        if (!engine.rootObjects().isEmpty()) {
            engine.rootObjects().front()->setProperty("workspaceIndex", 1);
        }
    };
    QObject::connect(
        &controller,
        &ReviewController::itemCountChanged,
        &application,
        open_first_available
    );
    // The Catalog count and the first page are deliberately separate
    // asynchronous queries.  A persisted Library may publish the page
    // before (or without a changed) count notification, so a headless
    // open-first flow must listen to the model that owns the rows rather
    // than treating the aggregate count as its readiness signal.
    QObject::connect(
        controller.reviewModel(),
        &QAbstractItemModel::modelReset,
        &application,
        open_first_available
    );
    QObject::connect(
        controller.reviewModel(),
        &QAbstractItemModel::rowsInserted,
        &application,
        [open_first_available](const QModelIndex&, int, int) {
            open_first_available();
        }
    );
    QTimer::singleShot(0, &application, open_first_available);
    QTimer::singleShot(50, &application, open_first_available);
}

void installFirstComparisonRequest(
    QApplication& application,
    ReviewController& controller,
    ThumbnailProvider* const thumbnail_provider
) {
    auto comparison_requested = std::make_shared<bool>(false);
    const auto request_first_comparison = [
        &application,
        &controller,
        thumbnail_provider,
        comparison_requested
    ]() {
        auto* const model = controller.reviewModel();
        if (*comparison_requested || model->rowCount() < 2
            || controller.scanning() || controller.refreshing()
            || controller.comparisonBusy()
            || controller.sessionEvidenceCount() > 0) {
            return;
        }
        const QModelIndex left = model->index(0, 0);
        const QModelIndex right = model->index(1, 0);
        const QString left_handle =
            model->data(left, ReviewModel::VisualHandleRole).toString();
        const QString right_handle =
            model->data(right, ReviewModel::VisualHandleRole).toString();
        if (left_handle.isEmpty() || right_handle.isEmpty()) {
            return;
        }
        *comparison_requested = true;
        const QVariantMap presentation = controller.prepareComparison(
            left_handle,
            right_handle
        );
        const QString presentation_id =
            presentation.value(QStringLiteral("presentationId")).toString();
        const QString left_ticket =
            presentation.value(QStringLiteral("leftRequestTicket")).toString();
        const QString right_ticket =
            presentation.value(QStringLiteral("rightRequestTicket")).toString();
        if (presentation_id.isEmpty() || left_ticket.isEmpty()
            || right_ticket.isEmpty()) {
            qCritical() << "Comparison smoke could not prepare two visual tickets";
            application.exit(EXIT_FAILURE);
            return;
        }
        const QSize requested_size(1'280, 960);
        QSize left_size;
        QSize right_size;
        const QImage left_image = thumbnail_provider->requestImage(
            imageProviderRequestId(
                presentation.value(QStringLiteral("leftSource")).toString()
            ),
            &left_size,
            requested_size
        );
        const QImage right_image = thumbnail_provider->requestImage(
            imageProviderRequestId(
                presentation.value(QStringLiteral("rightSource")).toString()
            ),
            &right_size,
            requested_size
        );
        if (left_image.isNull() || right_image.isNull()) {
            qCritical() << "Comparison smoke could not decode both visual tickets"
                        << "left null" << left_image.isNull() << "right null"
                        << right_image.isNull();
            application.exit(EXIT_FAILURE);
            return;
        }
        if (!controller.confirmComparisonReady(
                presentation_id,
                left_ticket,
                right_ticket
            )) {
            qCritical() << "Comparison smoke could not verify both decoded frames";
            application.exit(EXIT_FAILURE);
            return;
        }
        controller.recordComparison(presentation_id, 0);
    };
    QObject::connect(
        &controller,
        &ReviewController::itemCountChanged,
        &application,
        request_first_comparison
    );
    QObject::connect(
        controller.reviewModel(),
        &QAbstractItemModel::modelReset,
        &application,
        request_first_comparison
    );
    QObject::connect(
        controller.reviewModel(),
        &QAbstractItemModel::rowsInserted,
        &application,
        [&application, request_first_comparison](const QModelIndex&, int, int) {
            QTimer::singleShot(0, &application, request_first_comparison);
        }
    );
    QObject::connect(
        &controller,
        &ReviewController::refreshingChanged,
        &application,
        request_first_comparison
    );
    QTimer::singleShot(0, &application, request_first_comparison);
    QTimer::singleShot(50, &application, request_first_comparison);
}

void installFirstDecisionRequest(
    QApplication& application,
    ReviewController& controller
) {
    auto decision_requested = std::make_shared<bool>(false);
    const auto request_first_decision = [&controller, decision_requested]() {
        auto* const model = controller.reviewModel();
        if (*decision_requested || model->rowCount() == 0
            || controller.scanning() || controller.refreshing()
            || controller.decisionBusy()) {
            return;
        }
        *decision_requested = true;
        const QModelIndex first = model->index(0, 0);
        controller.setPhotoFlag(
            model->data(first, ReviewModel::PhotoIdRole).toString(),
            QStringLiteral("picked")
        );
    };
    QObject::connect(
        &controller,
        &ReviewController::itemCountChanged,
        &application,
        request_first_decision
    );
    // Just like open-first editing, Review's count and first page resolve
    // independently.  A persisted Catalog can publish its rows without a
    // new aggregate-count notification, so drive this headless action from
    // the row-owning model as well.
    QObject::connect(
        controller.reviewModel(),
        &QAbstractItemModel::modelReset,
        &application,
        request_first_decision
    );
    QObject::connect(
        controller.reviewModel(),
        &QAbstractItemModel::rowsInserted,
        &application,
        [request_first_decision](const QModelIndex&, int, int) {
            request_first_decision();
        }
    );
    QTimer::singleShot(0, &application, request_first_decision);
    QTimer::singleShot(50, &application, request_first_decision);
}

} // namespace DesktopSmoke
