#include "CFL/Certificates/Frontend.h"
#include <iostream>

int main() {
    using namespace cflcert;
    // Input alphabet: 0=<, 1=a, 2=>. Stack alphabet: 0=bottom.
    twopda::Machine machine;
    machine.states = 3;
    machine.alphabet = 3;
    machine.stackAlphabet = 1;
    machine.initial = 0;
    machine.bottom = 0;
    machine.leftMarker = 0;
    machine.rightMarker = 2;
    machine.finals = {2};
    machine.transitions = {
        {0, 0, 0, 1, {0}, +1}, // move from left marker onto input
        {1, 1, 0, 2, {0}, +1}  // consume 'a', arrive at right marker
    };

    const bool accepted = frontend::decide2NPDA(machine, {1});
    std::cout << "input accepted = " << std::boolalpha << accepted << '\n';
    if (accepted) {
        pda::PositiveCertificate certificate;
        const bool built = frontend::buildPositive2NPDACertificate(
            machine, {1}, certificate);
        std::cout << "positive certificate built and verified = "
                  << (built && frontend::verifyPositive2NPDACertificate(
                                   machine, {1}, certificate)) << '\n';
    }
}
