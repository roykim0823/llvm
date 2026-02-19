#include "lexer.h"
#include "parser.h"

int main() {
    toy::Lexer lexer;
    toy::Parser parser(lexer);

    // Run the main "interpreter loop" now.
    parser.mainLoop();

    return 0;
}