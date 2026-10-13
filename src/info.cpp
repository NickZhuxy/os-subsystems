#include <iostream>
#include <sys/utsname.h>
int main() {
    utsname system{};
    if (uname(&system) != 0) return 1;
    std::cout << "os-subsystems 0.1.0\n" << system.sysname << ' '
              << system.release << ' ' << system.machine << '\n';
}
