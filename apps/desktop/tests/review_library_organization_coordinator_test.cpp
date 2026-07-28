#include <QCoreApplication>

#include <cstdlib>

namespace review_library_organization_test {

void run_mutation_projection_contracts();
void run_failure_lifetime_contracts();

} // namespace review_library_organization_test

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    review_library_organization_test::run_mutation_projection_contracts();
    review_library_organization_test::run_failure_lifetime_contracts();
    return EXIT_SUCCESS;
}
