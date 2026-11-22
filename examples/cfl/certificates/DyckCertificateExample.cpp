#include "CFL/Certificates/DyckCertificates.h"
#include <iostream>

int main() {
    using namespace cflcert;
    Instance g;
    g.n = 3;
    g.source = 0;
    g.target = 2;
    g.edges = {
        {0, 1, Label::Open1},
        {1, 2, Label::Close1}
    };

    PositiveCertificate cert;
    if (buildPositive(g, cert)) {
        std::cout << "reachable; certificate valid = "
                  << std::boolalpha << verifyPositive(g, cert) << '\n';
    } else {
        std::cout << "unreachable\n";
    }
}
