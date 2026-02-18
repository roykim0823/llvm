#include "lexer.h"
#include "parser.h"
#include "compiler_env.h"

int main() {
    toy::Lexer lexer;
    toy::IRGenContext ctx;
    toy::Parser parser(lexer, ctx);

    // Run the main "interpreter loop" now.
    parser.mainLoop();

    return 0;
}