// Bounded synthetic source for the actual process-pipe and owner/UI tests.
#include <QCoreApplication>
#include <QImage>
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (app.arguments().size() != 2)
        return 2;
    QImage image(640, 426, QImage::Format_RGB32);
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            image.setPixel(
                x,
                y,
                qRgb(
                    20 + x * 130 / image.width(),
                    24 + y * 115 / image.height(),
                    38 + (x + y) * 90 / (image.width() + image.height())
                )
            );
    return image.save(app.arguments().at(1), "JPEG", 95) ? 0 : 1;
}
