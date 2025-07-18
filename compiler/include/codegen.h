// codegen.h - Header for NASM Code Generator
#ifndef CODEGEN_H
#define CODEGEN_H

#include "parser.h"
#include <stdio.h>

// Generates NASM assembly code from the ÆLang AST
void generate_code(AST *ast, FILE *out);

#endif // CODEGEN_H
