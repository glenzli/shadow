#include <QCoreApplication>

#include <cstdlib>

namespace review_library_keyword_test {

void run_keyword_lifecycle_contracts();
void run_failure_lifetime_contracts();

} // namespace review_library_keyword_test

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    review_library_keyword_test::run_keyword_lifecycle_contracts();
    review_library_keyword_test::run_failure_lifetime_contracts();
    return EXIT_SUCCESS;
}
