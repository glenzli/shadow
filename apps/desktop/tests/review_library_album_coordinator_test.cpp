#include <QCoreApplication>

#include <cstdlib>

namespace review_library_album_test {

void run_album_lifecycle_contracts();
void run_refresh_failure_lifetime_contracts();

} // namespace review_library_album_test

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    review_library_album_test::run_album_lifecycle_contracts();
    review_library_album_test::run_refresh_failure_lifetime_contracts();
    return EXIT_SUCCESS;
}
