#pragma once
#include "backend/edit_types.hpp"
#include <QImage>

struct AutoStartTone final {
    BackendBasicEditParameters basic;
    BackendFineEditParameters fine;
    bool preserveLight = false;
    bool useful = false;
};
// Bounded display-referred policy; it never estimates RAW sensor headroom.
AutoStartTone measureAutoStartTone(const QImage& image, const QString& scene = {});
AutoStartTone
measureAutoStartSkin(const QImage& image, const QByteArray& coverage, int width, int height);
bool autoStartPreservesIlluminant(const QString& scene);

// Display endpoint evidence only; this is not a RAW sensor clipping estimate.
double autoStartDisplayClippedFraction(const QImage& image);
