#include "edit_source_admission.hpp"

#include <QFileInfo>

bool editSourceIsAvailable(const QString& source_path) {
    return !source_path.trimmed().isEmpty() && QFileInfo(source_path).isFile();
}
