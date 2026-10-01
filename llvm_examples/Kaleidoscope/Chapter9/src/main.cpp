#include "driver.h"

int main() {
    toy::Driver driver;

    // Run the main "interpreter loop" now.
    driver.mainLoop();

    // Print out all of the generated code, debug info included.
    return driver.dumpModule();
}
