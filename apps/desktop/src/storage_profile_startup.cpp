#include "storage_profile_startup.hpp"
#include "storage_profile_archive.hpp"

#include <QDialog>
#include <QEventLoop>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QVBoxLayout>
#include <algorithm>

namespace {
QString text(const char* source) {
    return QCoreApplication::translate("StorageProfileStartup", source);
}
} // namespace

std::optional<StorageProfileInfo> chooseStorageProfileAtStartup(
    StorageProfileRegistry& registry,
    const QString& context,
    const QString& error
) {
    QDialog dialog;
    dialog.setWindowTitle(text(QT_TRANSLATE_NOOP("StorageProfileStartup", "Choose a Library")));
    dialog.resize(600, 420);
    auto* layout = new QVBoxLayout(&dialog);
    auto* explanation = new QLabel(error);
    explanation->setWordWrap(true);
    layout->addWidget(explanation);
    auto* list = new QListWidget;
    layout->addWidget(list, 1);
    auto* controls = new QHBoxLayout;
    auto* open = new QPushButton(text(QT_TRANSLATE_NOOP("StorageProfileStartup", "Open Selected")));
    auto* browse =
        new QPushButton(text(QT_TRANSLATE_NOOP("StorageProfileStartup", "Open Library…")));
    auto* create =
        new QPushButton(text(QT_TRANSLATE_NOOP("StorageProfileStartup", "New Library…")));
    auto* cancel = new QPushButton(text(QT_TRANSLATE_NOOP("StorageProfileStartup", "Cancel")));
    for (auto* button : {open, browse, create, cancel})
        controls->addWidget(button);
    layout->addLayout(controls);
    auto* restore =
        new QPushButton(text(QT_TRANSLATE_NOOP("StorageProfileStartup", "Restore Backup…")));
    layout->addWidget(restore);
    auto* reset = new QPushButton(
        text(QT_TRANSLATE_NOOP("StorageProfileStartup", "Reset Recent Library Records…"))
    );
    layout->addWidget(reset);
    QVector<StorageProfileInfo> profiles;
    std::optional<StorageProfileInfo> selected;
    const auto reload = [&] {
        QString registry_error;
        profiles = registry.recent(&registry_error);
        reset->setVisible(!registry_error.isEmpty());
        open->setEnabled(!profiles.isEmpty());
        list->clear();
        for (const auto& profile : profiles)
            list->addItem(profile.name() + '\n' + profile.root);
        int initial = 0;
        if (context == "debug") {
            for (qsizetype i = 0; i < profiles.size(); ++i) {
                if (profiles[i].kind == "debug") {
                    initial = static_cast<int>(i);
                    break;
                }
            }
        }
        list->setCurrentRow(initial);
    };
    const auto choose = [&](const QString& root, const QString& id) {
        QString selection_error;
        selected = registry.prepare(root, id, &selection_error);
        if (selected)
            dialog.accept();
        else
            explanation->setText(selection_error);
    };
    QObject::connect(open, &QPushButton::clicked, &dialog, [&] {
        const int index = list->currentRow();
        if (index >= 0 && index < profiles.size())
            choose(profiles[index].root, profiles[index].id);
    });
    QObject::connect(list, &QListWidget::itemDoubleClicked, open, [open] { open->click(); });
    QObject::connect(browse, &QPushButton::clicked, &dialog, [&] {
        const QString root = QFileDialog::getExistingDirectory(
            &dialog,
            text(QT_TRANSLATE_NOOP("StorageProfileStartup", "Open Library"))
        );
        if (!root.isEmpty())
            choose(root, {});
    });
    QObject::connect(create, &QPushButton::clicked, &dialog, [&] {
        QInputDialog prompt(&dialog);
        prompt.setWindowTitle(text(QT_TRANSLATE_NOOP("StorageProfileStartup", "New Library")));
        prompt.setLabelText(text(QT_TRANSLATE_NOOP("StorageProfileStartup", "Library Name")));
        prompt.setOkButtonText(text(QT_TRANSLATE_NOOP("StorageProfileStartup", "Create and Open")));
        prompt.setCancelButtonText(text(QT_TRANSLATE_NOOP("StorageProfileStartup", "Cancel")));
        if (prompt.exec() != QDialog::Accepted)
            return;
        const QString name = prompt.textValue();
        const QString parent = QFileDialog::getExistingDirectory(
            &dialog,
            text(QT_TRANSLATE_NOOP("StorageProfileStartup", "Choose a Location"))
        );
        if (parent.isEmpty())
            return;
        QString selection_error;
        selected = registry.create(name, parent, &selection_error);
        if (selected)
            dialog.accept();
        else
            explanation->setText(selection_error);
    });
    QObject::connect(restore, &QPushButton::clicked, &dialog, [&] {
        const auto backup = QFileDialog::getExistingDirectory(
            &dialog,
            text(QT_TRANSLATE_NOOP("StorageProfileStartup", "Choose a Library Backup"))
        );
        if (backup.isEmpty())
            return;
        const auto parent = QFileDialog::getExistingDirectory(
            &dialog,
            text(QT_TRANSLATE_NOOP("StorageProfileStartup", "Choose a Location"))
        );
        if (parent.isEmpty())
            return;
        if (StorageProfile::rootContains(registry.dailyRoot(), parent)
            || std::any_of(profiles.cbegin(), profiles.cend(), [&](const auto& info) {
                   return StorageProfile::rootContains(info.root, parent);
               })) {
            explanation->setText(text(QT_TRANSLATE_NOOP(
                "StorageProfileStartup",
                "Choose a destination outside existing libraries."
            )));
            return;
        }
        QInputDialog prompt(&dialog);
        prompt.setWindowTitle(
            text(QT_TRANSLATE_NOOP("StorageProfileStartup", "Restore into a New Library"))
        );
        prompt.setLabelText(text(QT_TRANSLATE_NOOP("StorageProfileStartup", "Library Name")));
        prompt.setTextValue(text(QT_TRANSLATE_NOOP("StorageProfileStartup", "Restored Library")));
        prompt.setOkButtonText(text(QT_TRANSLATE_NOOP("StorageProfileStartup", "Restore")));
        prompt.setCancelButtonText(text(QT_TRANSLATE_NOOP("StorageProfileStartup", "Cancel")));
        if (prompt.exec() != QDialog::Accepted)
            return;
        StorageProfileArchive worker;
        QEventLoop loop;
        QProgressDialog progress(
            text(QT_TRANSLATE_NOOP(
                "StorageProfileStartup",
                "Verifying the backup and restoring into a new library…"
            )),
            text(QT_TRANSLATE_NOOP("StorageProfileStartup", "Cancel")),
            0,
            0,
            &dialog
        );
        progress.setWindowModality(Qt::ApplicationModal);
        progress.setMinimumDuration(0);
        progress.setAutoClose(false);
        QString result, diagnostic;
        QObject::connect(
            &progress,
            &QProgressDialog::canceled,
            &worker,
            &StorageProfileArchive::cancel
        );
        QObject::connect(
            &worker,
            &StorageProfileArchive::progress,
            &progress,
            [&](quint64 done, quint64 total) {
                if (total > 0) {
                    progress.setRange(0, 100);
                    progress.setValue(static_cast<int>(std::min<quint64>(100, done * 100 / total)));
                }
            }
        );
        QObject::connect(
            &worker,
            &StorageProfileArchive::completed,
            &loop,
            [&](const QString& root) {
                result = root;
                loop.quit();
            }
        );
        QObject::connect(&worker, &StorageProfileArchive::failed, &loop, [&](const QString& error) {
            diagnostic = error;
            loop.quit();
        });
        worker.start({1, backup, parent, prompt.textValue(), {}});
        loop.exec();
        progress.close();
        if (!result.isEmpty())
            choose(result, {});
        else if (!progress.wasCanceled())
            explanation->setText(
                text(QT_TRANSLATE_NOOP(
                         "StorageProfileStartup",
                         "Could not restore the backup. Existing libraries were preserved. %1"
                     ))
                    .arg(diagnostic)
            );
    });
    QObject::connect(reset, &QPushButton::clicked, &dialog, [&] {
        QMessageBox confirmation(
            QMessageBox::Question,
            text(QT_TRANSLATE_NOOP("StorageProfileStartup", "Reset Recent Library Records")),
            text(QT_TRANSLATE_NOOP(
                "StorageProfileStartup",
                "This resets only recent locations and launch choices. Library data and names "
                "remain unchanged, and the previous records are preserved."
            )),
            QMessageBox::NoButton,
            &dialog
        );
        auto* reset_button = confirmation.addButton(
            text(QT_TRANSLATE_NOOP("StorageProfileStartup", "Reset Records")),
            QMessageBox::AcceptRole
        );
        confirmation.addButton(
            text(QT_TRANSLATE_NOOP("StorageProfileStartup", "Cancel")),
            QMessageBox::RejectRole
        );
        confirmation.exec();
        if (confirmation.clickedButton() != reset_button)
            return;
        QString reset_error;
        if (!registry.resetRecentRecords(&reset_error))
            explanation->setText(reset_error);
        reload();
    });
    QObject::connect(cancel, &QPushButton::clicked, &dialog, &QDialog::reject);
    reload();
    return dialog.exec() == QDialog::Accepted ? selected : std::nullopt;
}
