#include "driver.h"

int main() {
    toy::Driver driver;

    // Run the main "interpreter loop" now.
    driver.mainLoop();

    // Then compile everything it accumulated to an object file.
    return driver.emitObjectFile("output.o");
}
