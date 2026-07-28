#include <QCoreApplication>

#include <cstdlib>

namespace review_library_facet_test {

void run_facet_projection_contracts();
void run_coalescing_failure_lifetime_contracts();

} // namespace review_library_facet_test

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    review_library_facet_test::run_facet_projection_contracts();
    review_library_facet_test::run_coalescing_failure_lifetime_contracts();
    return EXIT_SUCCESS;
}
