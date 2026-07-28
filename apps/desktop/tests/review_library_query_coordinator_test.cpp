#include <QCoreApplication>

#include <cstdlib>

namespace review_library_query_test {

void run_projection_pagination_contracts();
void run_coalescing_failure_lifetime_contracts();

} // namespace review_library_query_test

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    review_library_query_test::run_projection_pagination_contracts();
    review_library_query_test::run_coalescing_failure_lifetime_contracts();
    return EXIT_SUCCESS;
}
