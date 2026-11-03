#pragma once

#include <QCoreApplication>
#include <QString>

// Product-generated stored names share one localized projection. User names
// remain unchanged; the persisted label is never rewritten for presentation.
namespace GradeNodePresentation {
[[nodiscard]] inline QString display_grade_node_label(const QString& stored_label) {
    constexpr auto context = "EditController";
    const QString numbered_copy_marker = QStringLiteral(" Copy ");
    const qsizetype numbered_copy_index = stored_label.lastIndexOf(numbered_copy_marker);
    if (numbered_copy_index > 0) {
        const QString suffix = stored_label.mid(numbered_copy_index + numbered_copy_marker.size());
        bool valid_number = false;
        const int number = suffix.toInt(&valid_number);
        if (valid_number && number >= 2 && QString::number(number) == suffix) {
            const QString base = stored_label.left(numbered_copy_index);
            return QCoreApplication::translate(
                       context,
                       QT_TRANSLATE_NOOP("EditController", "%1 Copy %2")
            )
                .arg(display_grade_node_label(base))
                .arg(number);
        }
    }
    const QString copy_suffix = QStringLiteral(" Copy");
    if (stored_label.endsWith(copy_suffix) && stored_label.size() > copy_suffix.size()) {
        const QString base = stored_label.left(stored_label.size() - copy_suffix.size());
        return QCoreApplication::translate(context, QT_TRANSLATE_NOOP("EditController", "%1 Copy"))
            .arg(display_grade_node_label(base));
    }
    if (stored_label.compare(QStringLiteral("Adjustment Node"), Qt::CaseInsensitive) == 0
        || stored_label.compare(QStringLiteral("Adjustments"), Qt::CaseInsensitive) == 0) {
        return QCoreApplication::translate(
            context,
            QT_TRANSLATE_NOOP("EditController", "Adjustment Node")
        );
    }
    const QString current_numbered_prefix = QStringLiteral("Adjustment Node ");
    const QString legacy_numbered_prefix = QStringLiteral("Adjustments ");
    const QString numbered_prefix =
        stored_label.startsWith(current_numbered_prefix, Qt::CaseInsensitive)
            ? current_numbered_prefix
            : legacy_numbered_prefix;
    if (stored_label.startsWith(numbered_prefix, Qt::CaseInsensitive)) {
        const QString suffix = stored_label.mid(numbered_prefix.size());
        bool valid_number = false;
        const int number = suffix.toInt(&valid_number);
        if (valid_number && number >= 2 && QString::number(number) == suffix) {
            return QCoreApplication::translate(
                       context,
                       QT_TRANSLATE_NOOP("EditController", "Adjustment Node %1")
            )
                .arg(number);
        }
    }
    return stored_label;
}

} // namespace GradeNodePresentation
