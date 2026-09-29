#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QSignalSpy>
#include <QVariantMap>

#include <cstdlib>
#include <iostream>
#include <memory>

class DecisionFixture final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool decisionBusy MEMBER busy NOTIFY decisionStateChanged)
    Q_PROPERTY(QString selectedPhotoId MEMBER photo NOTIFY selectedPhotoIdChanged)
    Q_PROPERTY(
        QString selectedRepresentationId MEMBER representation NOTIFY
            selectedRepresentationIdChanged
    )

  public:
    bool busy = false;
    bool already_matches = false;
    QString photo = "a";
    QString representation = "a-raw";
    QVariantMap next{{"photoId", "b"}, {"representationId", "b-raw"}};
    Q_INVOKABLE QVariantMap navigationTarget(const QString&, const QString&, int, int) {
        return next;
    }
    Q_INVOKABLE void setPhotoRating(const QString& id, int) {
        start(id);
    }
    Q_INVOKABLE void setPhotoFlag(const QString& id, const QString&) {
        start(id);
    }
    void start(const QString& id) {
        if (already_matches) {
            emit decisionCommitted(id);
        } else {
            busy = true;
            emit decisionStateChanged();
        }
    }
    void finish(bool success) {
        busy = false;
        emit decisionStateChanged();
        if (success)
            emit decisionCommitted("a");
        QCoreApplication::processEvents();
    }
  signals:
    void decisionCommitted(const QString& photoId);
    void decisionStateChanged();
    void filtersChanged();
    void libraryOrderChanged();
    void libraryAlbumChanged();
    void selectedPhotoIdChanged();
    void selectedRepresentationIdChanged();
};

namespace {
void require(bool okay, const char* message) {
    if (!okay) {
        std::cerr << "Review decision flow: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}
void invoke(QObject* flow, const char* method, const QVariant& value) {
    require(
        QMetaObject::invokeMethod(flow, method, Q_ARG(QVariant, value)),
        "call production flow"
    );
}
} // namespace

int main(int argc, char** argv) {
    QGuiApplication application(argc, argv);
    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.loadFromModule("Shadow.ReviewDecisionContract", "ReviewDecisionFlow");
    DecisionFixture fixture;
    const QVariant fixture_object = QVariant::fromValue(&fixture);
    std::unique_ptr<QObject> flow(component.createWithInitialProperties(
        {{"controller", fixture_object},
         {"selection", fixture_object},
         {"navigationModel", fixture_object}}
    ));
    if (!flow) {
        std::cerr << component.errorString().toStdString();
        return EXIT_FAILURE;
    }
    QSignalSpy advances(flow.get(), SIGNAL(advanceRequested(QVariant)));
    require(advances.isValid(), "real packaged flow exposes its navigation intent");
    invoke(flow.get(), "setRating", 4);
    fixture.finish(true);
    require(advances.size() == 0, "auto advance defaults off");
    flow->setProperty("autoAdvance", true);
    invoke(flow.get(), "setRating", 4);
    QCoreApplication::processEvents();
    require(advances.size() == 0, "async write must settle before selection moves");
    fixture.finish(true);
    require(advances.size() == 1, "accepted forward decision advances exactly once");
    emit fixture.decisionCommitted("a");
    require(advances.size() == 1, "duplicate or undo notifications cannot replay navigation");

    invoke(flow.get(), "setFlag", "picked");
    fixture.finish(false);
    emit fixture.decisionCommitted("a");
    require(advances.size() == 1, "failure retires pending navigation");

    invoke(flow.get(), "setRating", 2);
    fixture.photo = "c";
    emit fixture.selectedPhotoIdChanged();
    fixture.photo = "a";
    emit fixture.selectedPhotoIdChanged();
    fixture.finish(true);
    require(advances.size() == 1, "manual navigation away and back cancels pending intent");

    invoke(flow.get(), "setRating", 2);
    emit fixture.filtersChanged();
    fixture.finish(true);
    require(advances.size() == 1, "scope changes cancel pending intent");
    invoke(flow.get(), "setRating", 2);
    emit fixture.libraryOrderChanged();
    fixture.finish(true);
    require(advances.size() == 1, "sorting changes cancel pending intent");

    fixture.next.clear();
    invoke(flow.get(), "setRating", 2);
    fixture.finish(true);
    require(advances.size() == 1, "last photo retains selection without wrapping");
    fixture.next = {{"photoId", "b"}, {"representationId", "b-raw"}};
    fixture.already_matches = true;
    invoke(flow.get(), "setRating", 2);
    require(advances.size() == 2, "authoritative unchanged rating also advances");
    QCoreApplication::processEvents();
    std::cout << "Packaged Review decision flow boundaries passed\n";
    return EXIT_SUCCESS;
}

#include "review_decision_flow_contract_test.moc"
