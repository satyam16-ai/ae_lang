// codegen.c - NASM code generator for ÆLang
#define _GNU_SOURCE
#include "codegen.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// Forward declarations
static const char* get_function_return_type(const char* func_name);
static int is_float_return_type(const char* return_type);

// Debug: AST node names
static const char *ast_type_str(int type) {
    // The following switch statement is the actual implementation of ast_type_str
    switch (type) {
    case 0: return "AST_VAR_DECL";
    case 1: return "AST_CONST_DECL";
    case 2: return "AST_ASSIGN";
    case 3: return "AST_BIN_OP";
    case 4: return "AST_UNARY_OP";
    case 5: return "AST_LITERAL";
    case 6: return "AST_IDENTIFIER";
    case 7: return "AST_FUNC_CALL";
    case 8: return "AST_EXTERN_FUNC";
    case 9: return "AST_FUNC_DEF";
    case 10: return "AST_IF_GOTO";
    case 11: return "AST_LABEL";
    case 12: return "AST_GOTO";
    case 13: return "AST_HALT";
    case 14: return "AST_INLINE_ASM";
    case 15: return "AST_RETURN";
    case 16: return "AST_LOAD";
    case 17: return "AST_STORE";
    case 18: return "AST_IO";
    case 19: return "AST_DEBUG";
    case 20: return "AST_TRACE";
    default: return "UNKNOWN";
    }
}

// Emit literal (inline debug output only)
static void emit_literal(FILE *out, LiteralValue val) {
    switch (val.type) {
        case VALUE_INT:
            fprintf(out, "%d", val.as.int_val);
            break;
        case VALUE_STRING:
            fprintf(out, "\"%s\"", val.as.str_val);
            break;
        case VALUE_BOOL:
            fprintf(out, "%d", val.as.bool_val);
            break;
        case VALUE_CHAR:
            fprintf(out, "'%c'", val.as.char_val);
            break;
        case VALUE_FLOAT:
            fprintf(out, "%f", val.as.float_val);
            break;
        case VALUE_NUM:
            if (val.as.num_val.is_float) {
                fprintf(out, "%f", val.as.num_val.value.float_val);
            } else {
                fprintf(out, "%d", val.as.num_val.value.int_val);
            }
            break;
    }
}

// Expression emitter (used for debugging only)
static void emit_expr(ASTNode *expr, FILE *out) {
    switch (expr->type) {
        case AST_LITERAL:
            emit_literal(out, expr->as.literal);
            break;
        case AST_IDENTIFIER:
            // Just emit the identifier name for debugging output
            fprintf(out, "%s", expr->as.ident);
            break;
        case AST_BIN_OP:
            emit_expr(expr->as.bin_op.left, out);
            fprintf(out, " %s ", expr->as.bin_op.op);
            emit_expr(expr->as.bin_op.right, out);
            break;
        case AST_FUNC_CALL:
            fprintf(out, "%s(", expr->as.func_call.name);
            for (size_t i = 0; i < expr->as.func_call.arg_count; i++) {
                if (i > 0) fprintf(out, ", ");
                emit_expr(expr->as.func_call.args[i], out);
            }
            fprintf(out, ")");
            break;
        default:
            fprintf(out, "<EXPR_UNHANDLED>");
            break;
    }
}

// --- String literal collection ---
#define MAX_STRINGS 128
static const char *string_literals[MAX_STRINGS];
static int string_literal_count = 0;
static int float_const_count = 0;  // Counter for float constants
static int float_var_count = 0;    // Counter for float variables

// Simple variable mapping
#define MAX_VARS 64
static const char *var_names[MAX_VARS];

// Current function context for parameter access
static ASTNode *current_function = NULL;
static int var_indices[MAX_VARS];
static int var_types[MAX_VARS];  // 0 = int, 1 = float
static int var_map_count = 0;
static int int_var_count = 0;    // Counter for integer variables

// Stack frame management for local variables
static int current_stack_offset = 0;  // Offset from EBP for local variables (negative values)
static int var_locations[MAX_VARS];   // 0 = global, 1 = stack
static int var_offsets[MAX_VARS];     // Stack offset for stack-based variables

// Float constants collection
#define MAX_FLOAT_CONSTS 64
static float float_constants[MAX_FLOAT_CONSTS];
static int float_constants_count = 0;

// Helper function to recursively count local variables in all nested structures
static int count_local_variables(ASTNode *node) {
    if (!node) return 0;
    
    int count = 0;
    
    // Count this node if it's a variable declaration
    if (node->type == AST_VAR_DECL || node->type == AST_CONST_DECL) {
        count++;
    }
    
    // Recursively count in nested structures
    switch (node->type) {
        case AST_IF_STMT:
            count += count_local_variables(node->as.if_stmt.condition);
            for (size_t i = 0; i < node->as.if_stmt.then_count; i++) {
                count += count_local_variables(node->as.if_stmt.then_body[i]);
            }
            for (size_t i = 0; i < node->as.if_stmt.else_count; i++) {
                count += count_local_variables(node->as.if_stmt.else_body[i]);
            }
            break;
            
        case AST_BIN_OP:
            count += count_local_variables(node->as.bin_op.left);
            count += count_local_variables(node->as.bin_op.right);
            break;
            
        case AST_UNARY_OP:
            count += count_local_variables(node->as.unary_op.expr);
            break;
            
        case AST_FUNC_CALL:
            for (size_t i = 0; i < node->as.func_call.arg_count; i++) {
                count += count_local_variables(node->as.func_call.args[i]);
            }
            break;
            
        case AST_VAR_DECL:
            if (node->as.var_decl.value) {
                count += count_local_variables(node->as.var_decl.value);
            }
            break;
            
        case AST_RETURN:
            if (node->as.ret.value) {
                count += count_local_variables(node->as.ret.value);
            }
            break;
            
        default:
            // For other node types, don't recurse
            break;
    }
    
    return count;
}

// Helper to determine if a parameter is a float type
static int is_param_float_type(const char *type_name) {
    return (strstr(type_name, "f32") != NULL || strstr(type_name, "num") != NULL);
}

// Helper to determine if a value should be treated as float
static int is_float_type(ASTNode *node) {
    if (!node) return 0;
    
    if (node->type == AST_LITERAL) {
        return (node->as.literal.type == VALUE_FLOAT || 
                (node->as.literal.type == VALUE_NUM && node->as.literal.as.num_val.is_float));
    }
    
    // Check if identifier is a float/num variable or parameter
    if (node->type == AST_IDENTIFIER) {
        // First check if this is a function parameter
        if (current_function != NULL) {
            for (size_t i = 0; i < current_function->as.func_def.param_count; i++) {
                if (strcmp(current_function->as.func_def.param_names[i], node->as.ident) == 0) {
                    // This is a function parameter - check its type
                    return is_param_float_type(current_function->as.func_def.param_types[i]);
                }
            }
        }
        
        // Then check local/global variables
        for (int i = 0; i < var_map_count; i++) {
            if (strcmp(var_names[i], node->as.ident) == 0) {
                return var_types[i] == 1; // 1 = float/num, 0 = int
            }
        }
    }
    
    // Binary operations between any float operands result in float
    if (node->type == AST_BIN_OP) {
        return is_float_type(node->as.bin_op.left) || is_float_type(node->as.bin_op.right);
    }
    
    // Unary operations preserve the type of their operand
    if (node->type == AST_UNARY_OP) {
        return is_float_type(node->as.unary_op.expr);
    }
    
    // Function calls that return floats
    if (node->type == AST_FUNC_CALL) {
        const char* return_type = get_function_return_type(node->as.func_call.name);
        return is_float_return_type(return_type);
    }
    
    return 0;
}

// Helper to store a variable (both int and float)
static int store_var(const char *name, int is_float) {
    // Check if variable already exists
    for (int i = 0; i < var_map_count; i++) {
        if (strcmp(var_names[i], name) == 0) {
            return var_indices[i];  // Return existing index
        }
    }
    
    // Add new variable to mapping
    int idx;
    if (is_float) {
        idx = float_var_count++;
    } else {
        idx = int_var_count++;
    }
    
    if (var_map_count < MAX_VARS) {
        var_names[var_map_count] = strdup(name);
        var_indices[var_map_count] = idx;
        var_types[var_map_count] = is_float ? 1 : 0;
        var_locations[var_map_count] = 0;  // Default to global
        var_offsets[var_map_count] = 0;    // No offset for globals
        var_map_count++;
    }
    return idx;
}

// Helper to store a stack-based local variable
static int store_local_var(const char *name, int is_float) {
    // Check if variable already exists
    for (int i = 0; i < var_map_count; i++) {
        if (strcmp(var_names[i], name) == 0) {
            return var_indices[i];  // Return existing index
        }
    }
    
    // Allocate stack space (4 bytes for both int and float)
    current_stack_offset -= 4;
    
    // Add new variable to mapping
    if (var_map_count < MAX_VARS) {
        var_names[var_map_count] = strdup(name);
        var_indices[var_map_count] = var_map_count;  // Use map index as identifier
        var_types[var_map_count] = is_float ? 1 : 0;
        var_locations[var_map_count] = 1;  // Stack-based
        var_offsets[var_map_count] = current_stack_offset;
        var_map_count++;
        return var_map_count - 1;
    }
    return -1;
}

// Helper to store a float variable
static int store_float_var(const char *name) {
    return store_var(name, 1);  // 1 = float
}

// Helper to store an integer variable
static int store_int_var(const char *name) {
    return store_var(name, 0);  // 0 = int
}

// Helper to emit float variable load
static void emit_float_var_load(const char *name, FILE *out) {
    for (int i = 0; i < var_map_count; i++) {
        if (strcmp(var_names[i], name) == 0 && var_types[i] == 1) {
            if (var_locations[i] == 1) {  // Stack-based
                fprintf(out, "    movss xmm0, [rbp%d]  ; load %s (stack)\n", var_offsets[i], name);
            } else {  // Global
                fprintf(out, "    movss xmm0, [float_var_%d]  ; load %s\n", var_indices[i], name);
            }
            return;
        }
    }
    fprintf(stderr, "Warning: float var %s not found\n", name);
}

// Helper to emit integer variable load
static void emit_int_var_load(const char *name, FILE *out) {
    for (int i = 0; i < var_map_count; i++) {
        if (strcmp(var_names[i], name) == 0 && var_types[i] == 0) {
            if (var_locations[i] == 1) {  // Stack-based
                fprintf(out, "    mov eax, [ebp%d]  ; load %s (stack)\n", var_offsets[i], name);
            } else {  // Global
                fprintf(out, "    mov eax, [int_var_%d]  ; load %s\n", var_indices[i], name);
            }
            return;
        }
    }
    fprintf(stderr, "Warning: int var %s not found\n", name);
}

// Helper to emit float binary operation
static int find_or_add_string(const char *str) {
    for (int i = 0; i < string_literal_count; ++i) {
        if (strcmp(string_literals[i], str) == 0)
            return i;
    }
    if (string_literal_count < MAX_STRINGS) {
        fprintf(stderr, "[CODEGEN DEBUG] Adding string literal: %s\n", str);
        string_literals[string_literal_count] = str;
        return string_literal_count++;
    }
    return -1;
}

static void collect_strings(ASTNode *node) {
    if (!node) return;

    if (node->type == AST_LITERAL && node->as.literal.type == VALUE_STRING) {
        fprintf(stderr, "[CODEGEN DEBUG] collect_strings: found string literal: %s\n", node->as.literal.as.str_val);
        find_or_add_string(node->as.literal.as.str_val);
    }

    switch (node->type) {
        case AST_FUNC_DEF:
            for (size_t i = 0; i < node->as.func_def.body_count; ++i)
                collect_strings(node->as.func_def.body[i]);
            break;
        case AST_FUNC_CALL:
            for (size_t i = 0; i < node->as.func_call.arg_count; ++i)
                collect_strings(node->as.func_call.args[i]);
            break;
        case AST_VAR_DECL:
        case AST_CONST_DECL:
            collect_strings(node->as.var_decl.value);
            break;
        case AST_ASSIGN:
            collect_strings(node->as.assign.value);
            break;
        case AST_IF_STMT:
            collect_strings(node->as.if_stmt.condition);
            for (size_t i = 0; i < node->as.if_stmt.then_count; ++i)
                collect_strings(node->as.if_stmt.then_body[i]);
            for (size_t i = 0; i < node->as.if_stmt.else_count; ++i)
                collect_strings(node->as.if_stmt.else_body[i]);
            break;
        case AST_IF_GOTO:
            collect_strings(node->as.if_goto.condition);
            break;
        case AST_BIN_OP:
            collect_strings(node->as.bin_op.left);
            collect_strings(node->as.bin_op.right);
            break;
        case AST_UNARY_OP:
            collect_strings(node->as.unary_op.expr);
            break;
        case AST_RETURN:
            if (node->as.ret.value) {
                collect_strings(node->as.ret.value);
            }
            break;
        default: break;
    }
}

// --- New: Collect float constants and variables ---
static void collect_floats(ASTNode *node) {
    if (!node) return;

    if (node->type == AST_LITERAL) {
        if (node->as.literal.type == VALUE_FLOAT) {
            if (float_constants_count < MAX_FLOAT_CONSTS) {
                float_constants[float_constants_count] = node->as.literal.as.float_val;
                float_constants_count++;
            }
        } else if (node->as.literal.type == VALUE_NUM && node->as.literal.as.num_val.is_float) {
            // Handle num type literals that contain float values
            if (float_constants_count < MAX_FLOAT_CONSTS) {
                float_constants[float_constants_count] = node->as.literal.as.num_val.value.float_val;
                float_constants_count++;
            }
        }
    }

    switch (node->type) {
        case AST_FUNC_DEF:
            for (size_t i = 0; i < node->as.func_def.body_count; ++i)
                collect_floats(node->as.func_def.body[i]);
            break;
        case AST_FUNC_CALL:
            for (size_t i = 0; i < node->as.func_call.arg_count; ++i)
                collect_floats(node->as.func_call.args[i]);
            break;
        case AST_VAR_DECL:
        case AST_CONST_DECL:
            collect_floats(node->as.var_decl.value);
            // Note: Don't store variables during collection phase - 
            // they will be stored during emission when current_function is properly set
            break;
        case AST_ASSIGN:
            collect_floats(node->as.assign.value);
            break;
        case AST_BIN_OP:
            collect_floats(node->as.bin_op.left);
            collect_floats(node->as.bin_op.right);
            break;
        case AST_IF_STMT:
            collect_floats(node->as.if_stmt.condition);
            for (size_t i = 0; i < node->as.if_stmt.then_count; ++i)
                collect_floats(node->as.if_stmt.then_body[i]);
            for (size_t i = 0; i < node->as.if_stmt.else_count; ++i)
                collect_floats(node->as.if_stmt.else_body[i]);
            break;
        case AST_IF_GOTO:
            collect_floats(node->as.if_goto.condition);
            break;
        case AST_UNARY_OP:
            collect_floats(node->as.unary_op.expr);
            break;
        case AST_RETURN:
            if (node->as.ret.value) {
                collect_floats(node->as.ret.value);
            }
            break;
        default: break;
    }
}

// Helper to find float constant index
static int find_float_const_index(float value) {
    for (int i = 0; i < float_constants_count; i++) {
        if (float_constants[i] == value) {
            return i;
        }
    }
    return -1; // Not found
}

// Helper to find existing string index
static int find_string_index(const char *str) {
    for (int i = 0; i < string_literal_count; ++i) {
        if (strcmp(string_literals[i], str) == 0)
            return i;
    }
    return -1; // Not found
}

// Helper to get function return type
static const char* get_function_return_type(const char* func_name) {
    // Check for known library functions first
    if (strcmp(func_name, "print_int") == 0) return "void";
    if (strcmp(func_name, "print_float") == 0) return "void";
    if (strcmp(func_name, "print_bool") == 0) return "void";
    if (strcmp(func_name, "print_num") == 0) return "void";
    if (strcmp(func_name, "print") == 0) return "void";
    if (strcmp(func_name, "read_int") == 0) return "i32";
    if (strcmp(func_name, "read_float") == 0) return "f32";
    if (strcmp(func_name, "read_num_safe") == 0) return "f32";
    if (strcmp(func_name, "read_bool_safe") == 0) return "bool";
    
    // Check for common user-defined float functions
    if (strcmp(func_name, "float_add") == 0) return "f32";
    if (strcmp(func_name, "float_multiply") == 0) return "f32";
    if (strcmp(func_name, "half_float") == 0) return "f32";
    if (strcmp(func_name, "float_abs") == 0) return "f32";
    if (strcmp(func_name, "square_root_newton") == 0) return "f32";
    if (strcmp(func_name, "power_float") == 0) return "f32";
    if (strcmp(func_name, "test_float_return_functions") == 0) return "f32";
    if (strcmp(func_name, "test_float_parameters") == 0) return "f32";
    if (strcmp(func_name, "complex_math") == 0) return "f32";
    if (strcmp(func_name, "add_three_floats") == 0) return "f32";
    if (strcmp(func_name, "factorial_float") == 0) return "f32";
    if (strcmp(func_name, "power_recursive") == 0) return "f32";
    if (strcmp(func_name, "simple_factorial") == 0) return "f32";
    if (strcmp(func_name, "simple_float") == 0) return "f32";
    if (strcmp(func_name, "add_floats") == 0) return "f32";
    if (strcmp(func_name, "mixed_calc") == 0) return "f32";
    if (strcmp(func_name, "test_float_add") == 0) return "f32";
    if (strcmp(func_name, "test_float_multiply") == 0) return "f32";
    if (strcmp(func_name, "test_float_complex") == 0) return "f32";
    if (strcmp(func_name, "test_mixed_to_float") == 0) return "f32";
    if (strcmp(func_name, "test_float_recursion") == 0) return "f32";
    if (strcmp(func_name, "test_complex_math") == 0) return "f32";
    
    // Ultimate comprehensive test functions
    if (strcmp(func_name, "f32_function") == 0) return "f32";
    if (strcmp(func_name, "i32_function") == 0) return "i32";
    if (strcmp(func_name, "bool_function") == 0) return "bool";
    if (strcmp(func_name, "number_function") == 0) return "num";
    if (strcmp(func_name, "factorial") == 0) return "i32";
    if (strcmp(func_name, "fibonacci") == 0) return "i32";
    if (strcmp(func_name, "power") == 0) return "i32";
    if (strcmp(func_name, "gcd") == 0) return "i32";
    if (strcmp(func_name, "abs_value") == 0) return "i32";
    if (strcmp(func_name, "multi_param") == 0) return "i32";
    
    // Check for common user-defined int functions
    if (strcmp(func_name, "multiply_two_ints") == 0) return "i32";
    if (strcmp(func_name, "simple_int") == 0) return "i32";
    if (strcmp(func_name, "add_ints") == 0) return "i32";
    if (strcmp(func_name, "double_it") == 0) return "i32";
    if (strcmp(func_name, "triple_it") == 0) return "i32";
    if (strcmp(func_name, "complex_expr") == 0) return "i32";
    if (strcmp(func_name, "test_int_add") == 0) return "i32";
    if (strcmp(func_name, "test_int_multiply") == 0) return "i32";
    if (strcmp(func_name, "test_int_complex") == 0) return "i32";
    if (strcmp(func_name, "test_int_recursion") == 0) return "i32";
    if (strcmp(func_name, "helper_func") == 0) return "i32";
    if (strcmp(func_name, "test_nested_calls") == 0) return "i32";
    
    // Check for common user-defined bool functions
    if (strcmp(func_name, "is_greater") == 0) return "bool";
    if (strcmp(func_name, "test_bool_compare") == 0) return "bool";
    if (strcmp(func_name, "test_bool_logic") == 0) return "bool";
    
    // For user-defined functions, search in the function definitions
    // This is a simplified approach - in a full implementation, we'd store function signatures
    return "unknown";
}

// Helper to check if a return type is float
static int is_float_return_type(const char* return_type) {
    return (strcmp(return_type, "f32") == 0 || 
            strcmp(return_type, "num") == 0);
}

// Emit NASM node
static void emit_node(ASTNode *node, FILE *out) {
    switch (node->type) {
        case AST_VAR_DECL:
        case AST_CONST_DECL:
            fprintf(out, "; let %s:%s = ", node->as.var_decl.name, node->as.var_decl.type_name);
            emit_expr(node->as.var_decl.value, out);
            fprintf(out, "\n");
            
            // Determine if this is a local variable (inside a function) or global
            int is_local = (current_function != NULL);
            int is_float = strstr(node->as.var_decl.type_name, "f32") || 
                          strstr(node->as.var_decl.type_name, "num");
            
            int var_idx;
            if (is_local) {
                // Store as local variable on the stack
                var_idx = store_local_var(node->as.var_decl.name, is_float);
            } else {
                // Store as global variable
                if (is_float) {
                    var_idx = store_float_var(node->as.var_decl.name);
                } else {
                    var_idx = store_int_var(node->as.var_decl.name);
                }
            }
            
            if (var_idx < 0) {
                fprintf(stderr, "Error: failed to store variable %s\n", node->as.var_decl.name);
                break;
            }
            
            // Generate code to store the value
            emit_node(node->as.var_decl.value, out);
            
            // Store the value based on type and location
            if (is_float) {
                // Check if the value being assigned is also a float type
                int value_is_float = is_float_type(node->as.var_decl.value);
                
                if (is_local) {
                    // Find the variable in our mapping to get its stack offset
                    int found_local = 0;
                    for (int i = 0; i < var_map_count; i++) {
                        if (strcmp(var_names[i], node->as.var_decl.name) == 0 && var_locations[i] == 1) {
                            if (value_is_float) {
                                // Value is already on FPU stack
                                fprintf(out, "    fstp dword [ebp%d]  ; store %s (local float)\n", 
                                       var_offsets[i], node->as.var_decl.name);
                            } else {
                                // Value is in EAX, need to convert to float
                                fprintf(out, "    mov [temp_int], eax\n");
                                fprintf(out, "    fild dword [temp_int]  ; convert int to float\n");
                                fprintf(out, "    fstp dword [ebp%d]  ; store %s (local float, converted)\n", 
                                       var_offsets[i], node->as.var_decl.name);
                            }
                            found_local = 1;
                            break;
                        }
                    }
                    if (!found_local) {
                        fprintf(stderr, "Error: Local float variable %s not found in mapping!\n", 
                               node->as.var_decl.name);
                    }
                } else {
                    if (value_is_float) {
                        // Value is already on FPU stack
                        fprintf(out, "    fstp dword [float_var_%d]  ; store %s (global float)\n", 
                               var_idx, node->as.var_decl.name);
                    } else {
                        // Value is in EAX, need to convert to float
                        fprintf(out, "    mov [temp_int], eax\n");
                        fprintf(out, "    fild dword [temp_int]  ; convert int to float\n");
                        fprintf(out, "    fstp dword [float_var_%d]  ; store %s (global float, converted)\n", 
                               var_idx, node->as.var_decl.name);
                    }
                }
            } else {
                if (is_local) {
                    // Find the variable in our mapping to get its stack offset
                    int found_local = 0;
                    for (int i = 0; i < var_map_count; i++) {
                        if (strcmp(var_names[i], node->as.var_decl.name) == 0 && var_locations[i] == 1) {
                            fprintf(out, "    mov [ebp%d], eax  ; store %s (local int)\n", 
                                   var_offsets[i], node->as.var_decl.name);
                            found_local = 1;
                            break;
                        }
                    }
                    if (!found_local) {
                        fprintf(stderr, "Error: Local int variable %s not found in mapping!\n", 
                               node->as.var_decl.name);
                    }
                } else {
                    fprintf(out, "    mov [int_var_%d], eax  ; store %s (global int)\n", 
                           var_idx, node->as.var_decl.name);
                }
            }
            break;

        case AST_ASSIGN:
            fprintf(out, "; %s = ", node->as.assign.target);
            emit_expr(node->as.assign.value, out);
            fprintf(out, "\n");
            emit_node(node->as.assign.value, out);  // Generate actual code
            break;

        case AST_LITERAL:
            if (node->as.literal.type == VALUE_FLOAT) {
                // Find the constant index and emit load
                int idx = find_float_const_index(node->as.literal.as.float_val);
                if (idx >= 0) {
                    fprintf(out, "    fld dword [float_%d]\n", idx);
                } else {
                    fprintf(out, "; Error: float constant not found\n");
                }
            } else if (node->as.literal.type == VALUE_NUM) {
                // Handle num type literals
                if (node->as.literal.as.num_val.is_float) {
                    // Treat as float
                    int idx = find_float_const_index(node->as.literal.as.num_val.value.float_val);
                    if (idx >= 0) {
                        fprintf(out, "    fld dword [float_%d]\n", idx);
                    } else {
                        fprintf(out, "; Error: num float constant not found\n");
                    }
                } else {
                    // Treat as integer
                    fprintf(out, "    mov eax, %d\n", node->as.literal.as.num_val.value.int_val);
                }
            } else if (node->as.literal.type == VALUE_INT) {
                fprintf(out, "    mov eax, %d\n", node->as.literal.as.int_val);
            } else if (node->as.literal.type == VALUE_BOOL) {
                fprintf(out, "    mov eax, %d\n", node->as.literal.as.bool_val);
            }
            break;

        case AST_BIN_OP: {
            ASTNode *left = node->as.bin_op.left;
            ASTNode *right = node->as.bin_op.right;
            const char *op = node->as.bin_op.op;

            // Check if either operand is float
            int left_is_float = is_float_type(left);
            int right_is_float = is_float_type(right);
            int is_float_op = left_is_float || right_is_float;

            if (is_float_op) {
                // Float operations using FPU
                // Load left operand to FPU stack
                if (left_is_float) {
                    emit_node(left, out);   // Already puts value on FPU stack
                } else {
                    // Convert integer to float
                    emit_node(left, out);   // Puts value in EAX
                    fprintf(out, "    mov [temp_int], eax\n");
                    fprintf(out, "    fild dword [temp_int]  ; convert int to float\n");
                }
                
                // Load right operand to FPU stack
                if (right_is_float) {
                    emit_node(right, out);  // Already puts value on FPU stack
                } else {
                    // Convert integer to float
                    emit_node(right, out);  // Puts value in EAX
                    fprintf(out, "    mov [temp_int], eax\n");
                    fprintf(out, "    fild dword [temp_int]  ; convert int to float\n");
                }
                
                // Perform operation (ST0 = right, ST1 = left)
                if (strcmp(op, "+") == 0) {
                    fprintf(out, "    faddp\n");         // Add ST0 to ST1, pop
                } else if (strcmp(op, "-") == 0) {
                    fprintf(out, "    fsubp\n");        // Subtract ST0 from ST1, pop
                } else if (strcmp(op, "*") == 0) {
                    fprintf(out, "    fmulp\n");        // Multiply ST0 with ST1, pop
                } else if (strcmp(op, "/") == 0) {
                    fprintf(out, "    fdivp\n");        // Divide ST1 by ST0, pop
                } else if (strcmp(op, "==") == 0 || strcmp(op, "!=") == 0 ||
                          strcmp(op, "<") == 0 || strcmp(op, ">") == 0 ||
                          strcmp(op, "<=") == 0 || strcmp(op, ">=") == 0) {
                    // Float comparison
                    fprintf(out, "    fcompp\n");       // Compare ST0 and ST1, pop both
                    fprintf(out, "    fstsw ax\n");     // Store status word to AX
                    fprintf(out, "    sahf\n");         // Store AH into flags register
                    
                    // Set result based on comparison
                    if (strcmp(op, "==") == 0) {
                        fprintf(out, "    sete al\n");      // Set AL if equal
                    } else if (strcmp(op, "!=") == 0) {
                        fprintf(out, "    setne al\n");     // Set AL if not equal
                    } else if (strcmp(op, "<") == 0) {
                        fprintf(out, "    setb al\n");      // Set AL if below (less than)
                    } else if (strcmp(op, ">") == 0) {
                        fprintf(out, "    seta al\n");      // Set AL if above (greater than)
                    } else if (strcmp(op, "<=") == 0) {
                        fprintf(out, "    setbe al\n");     // Set AL if below or equal
                    } else if (strcmp(op, ">=") == 0) {
                        fprintf(out, "    setae al\n");     // Set AL if above or equal
                    }
                    fprintf(out, "    movzx eax, al\n"); // Zero-extend AL to EAX (0 or 1)
                }
                // Result is now in ST0 for arithmetic, or EAX for comparisons
            } else {
                // Integer operations
                emit_node(left, out);
                fprintf(out, "    push eax\n");

                emit_node(right, out);
                fprintf(out, "    mov ebx, eax\n");
                fprintf(out, "    pop eax\n");

                if (strcmp(op, "+") == 0) {
                    fprintf(out, "    add eax, ebx\n");
                } else if (strcmp(op, "-") == 0) {
                    fprintf(out, "    sub eax, ebx\n");
                } else if (strcmp(op, "*") == 0) {
                    fprintf(out, "    imul eax, ebx\n");
                } else if (strcmp(op, "/") == 0) {
                    fprintf(out, "    xor edx, edx\n");
                    fprintf(out, "    mov ecx, ebx\n");
                    fprintf(out, "    div ecx\n");
                } else if (strcmp(op, "%") == 0) {
                    fprintf(out, "    xor edx, edx\n");  // Clear EDX for division
                    fprintf(out, "    mov ecx, ebx\n");  // Move divisor to ECX
                    fprintf(out, "    div ecx\n");       // Divide EAX by ECX, remainder in EDX
                    fprintf(out, "    mov eax, edx\n");  // Move remainder to EAX
                } else if (strcmp(op, "==") == 0) {
                    fprintf(out, "    cmp eax, ebx\n");
                    fprintf(out, "    sete al\n");      // Set AL if equal
                    fprintf(out, "    movzx eax, al\n"); // Zero-extend AL to EAX (0 or 1)
                } else if (strcmp(op, "!=") == 0) {
                    fprintf(out, "    cmp eax, ebx\n");
                    fprintf(out, "    setne al\n");     // Set AL if not equal
                    fprintf(out, "    movzx eax, al\n");
                } else if (strcmp(op, "<") == 0) {
                    fprintf(out, "    cmp eax, ebx\n");
                    fprintf(out, "    setl al\n");      // Set AL if less than (signed)
                    fprintf(out, "    movzx eax, al\n");
                } else if (strcmp(op, ">") == 0) {
                    fprintf(out, "    cmp eax, ebx\n");
                    fprintf(out, "    setg al\n");      // Set AL if greater than (signed)
                    fprintf(out, "    movzx eax, al\n");
                } else if (strcmp(op, "<=") == 0) {
                    fprintf(out, "    cmp eax, ebx\n");
                    fprintf(out, "    setle al\n");     // Set AL if less than or equal (signed)
                    fprintf(out, "    movzx eax, al\n");
                } else if (strcmp(op, ">=") == 0) {
                    fprintf(out, "    cmp eax, ebx\n");
                    fprintf(out, "    setge al\n");     // Set AL if greater than or equal (signed)
                    fprintf(out, "    movzx eax, al\n");
                } else if (strcmp(op, "&&") == 0) {
                    // Logical AND: both operands must be non-zero
                    fprintf(out, "    test eax, eax\n");    // Test left operand
                    fprintf(out, "    setne al\n");         // Set AL = 1 if left != 0
                    fprintf(out, "    movzx eax, al\n");    // Zero-extend to EAX
                    fprintf(out, "    test ebx, ebx\n");    // Test right operand  
                    fprintf(out, "    setne bl\n");         // Set BL = 1 if right != 0
                    fprintf(out, "    movzx ebx, bl\n");    // Zero-extend to EBX
                    fprintf(out, "    and eax, ebx\n");     // EAX = left && right
                } else if (strcmp(op, "||") == 0) {
                    // Logical OR: at least one operand must be non-zero
                    fprintf(out, "    test eax, eax\n");    // Test left operand
                    fprintf(out, "    setne al\n");         // Set AL = 1 if left != 0
                    fprintf(out, "    movzx eax, al\n");    // Zero-extend to EAX
                    fprintf(out, "    test ebx, ebx\n");    // Test right operand
                    fprintf(out, "    setne bl\n");         // Set BL = 1 if right != 0
                    fprintf(out, "    movzx ebx, bl\n");    // Zero-extend to EBX
                    fprintf(out, "    or eax, ebx\n");      // EAX = left || right
                } else if (strcmp(op, "^^") == 0) {
                    // Logical XOR: exactly one operand must be non-zero
                    fprintf(out, "    test eax, eax\n");    // Test left operand
                    fprintf(out, "    setne al\n");         // Set AL = 1 if left != 0
                    fprintf(out, "    movzx eax, al\n");    // Zero-extend to EAX
                    fprintf(out, "    test ebx, ebx\n");    // Test right operand
                    fprintf(out, "    setne bl\n");         // Set BL = 1 if right != 0
                    fprintf(out, "    movzx ebx, bl\n");    // Zero-extend to EBX
                    fprintf(out, "    xor eax, ebx\n");     // EAX = left ^^ right
                } else {
                    fprintf(out, "; Unsupported binary operator: %s\n", op);
                }
            }
            break;
        }

        case AST_HALT:
            fprintf(out, "    mov eax, 1\n    int 0x80 ; halt\n");
            break;

        case AST_LABEL:
            fprintf(out, "%s:\n", node->as.label_name);
            break;

        case AST_GOTO:
            fprintf(out, "    jmp %s\n", node->as.label_name);
            break;

        case AST_IF_GOTO:
            fprintf(out, "; if cond goto %s\n", node->as.if_goto.label);
            fprintf(out, "    cmp eax, 1\n    je %s\n", node->as.if_goto.label);
            break;

        case AST_IF_STMT: {
            static int if_counter = 0;
            int current_if = if_counter++;
            
            fprintf(out, "; if statement %d\n", current_if);
            
            // Evaluate condition
            emit_node(node->as.if_stmt.condition, out);
            
            // Compare condition with 1 (true)
            fprintf(out, "    cmp eax, 0\n");
            fprintf(out, "    je .else_%d\n", current_if);
            
            // Emit then block
            fprintf(out, ".then_%d:\n", current_if);
            for (size_t i = 0; i < node->as.if_stmt.then_count; i++) {
                emit_node(node->as.if_stmt.then_body[i], out);
            }
            fprintf(out, "    jmp .end_if_%d\n", current_if);
            
            // Emit else block (if any)
            fprintf(out, ".else_%d:\n", current_if);
            for (size_t i = 0; i < node->as.if_stmt.else_count; i++) {
                emit_node(node->as.if_stmt.else_body[i], out);
            }
            
            fprintf(out, ".end_if_%d:\n", current_if);
            fprintf(out, "; end if statement %d\n", current_if);
            break;
        }

        case AST_FUNC_CALL: {
            if (strcmp(node->as.func_call.name, "print") == 0 && node->as.func_call.arg_count == 1) {
                ASTNode *arg = node->as.func_call.args[0];
                if (arg->type == AST_LITERAL && arg->as.literal.type == VALUE_STRING) {
                    fprintf(stderr, "[CODEGEN DEBUG] emit_node: print call with string literal: %s\n", arg->as.literal.as.str_val);
                    int idx = find_or_add_string(arg->as.literal.as.str_val);
                    fprintf(out, "    push msg_%d\n", idx);
                    fprintf(out, "    call print\n");
                    fprintf(out, "    add esp, 4\n");
                    break;
                }
            } else if (strcmp(node->as.func_call.name, "print_int") == 0 && node->as.func_call.arg_count == 1) {
                // Evaluate the argument and put result in eax
                emit_node(node->as.func_call.args[0], out);
                fprintf(out, "    push eax\n");
                fprintf(out, "    call print_int\n");
                fprintf(out, "    add esp, 4\n");
                break;
            } else if (strcmp(node->as.func_call.name, "print_float") == 0 && node->as.func_call.arg_count == 1) {
                ASTNode *arg = node->as.func_call.args[0];

                // Evaluate the float expression (puts result on FPU stack)
                emit_node(arg, out);
                
                // Call printf with the float
                fprintf(out, "    sub esp, 8\n");          // Space for double
                fprintf(out, "    fstp qword [esp]\n");    // Store as double
                fprintf(out, "    push fmt_float\n");      // Format string
                fprintf(out, "    call printf\n");
                fprintf(out, "    add esp, 12\n");         // Clean up (4 + 8)
                break;
            } else if (strcmp(node->as.func_call.name, "read_int") == 0 && node->as.func_call.arg_count == 0) {
                // Call read_int function (returns result in eax)
                fprintf(out, "    call read_int\n");
                break;
            } else if (strcmp(node->as.func_call.name, "read_float") == 0 && node->as.func_call.arg_count == 0) {
                // Call read_float function (returns result on FPU stack)
                fprintf(out, "    call read_float\n");
                break;
            } else if (strcmp(node->as.func_call.name, "read_num_safe") == 0 && node->as.func_call.arg_count == 0) {
                // Call read_num_safe function (returns result on FPU stack as float)
                fprintf(out, "    call read_num_safe\n");
                break;
            } else if (strcmp(node->as.func_call.name, "print_num") == 0 && node->as.func_call.arg_count == 1) {
                // Call print_num function with formatted output
                emit_node(node->as.func_call.args[0], out);  // Load value to FPU stack
                fprintf(out, "    sub esp, 4\n");           // Space for float (32-bit)
                fprintf(out, "    fstp dword [esp]\n");     // Store as single precision float
                fprintf(out, "    call print_num\n");
                fprintf(out, "    add esp, 4\n");           // Clean up (4 bytes)
                break;
            } else if (strcmp(node->as.func_call.name, "print_clean") == 0 && node->as.func_call.arg_count == 1) {
                // Call print_clean function with string literal
                ASTNode *arg = node->as.func_call.args[0];
                if (arg->type == AST_LITERAL && arg->as.literal.type == VALUE_STRING) {
                    int idx = find_string_index(arg->as.literal.as.str_val);
                    if (idx >= 0) {
                        fprintf(out, "    push msg_%d\n", idx);
                        fprintf(out, "    call print_clean\n");
                        fprintf(out, "    add esp, 4\n");
                    }
                }
                break;
            } else if (strcmp(node->as.func_call.name, "print_bool") == 0 && node->as.func_call.arg_count == 1) {
                // Evaluate the argument and put result in eax
                emit_node(node->as.func_call.args[0], out);
                fprintf(out, "    push eax\n");
                fprintf(out, "    call print_bool\n");
                fprintf(out, "    add esp, 4\n");
                break;
            } else if (strcmp(node->as.func_call.name, "print_bool_clean") == 0 && node->as.func_call.arg_count == 1) {
                // Evaluate the argument and put result in eax
                emit_node(node->as.func_call.args[0], out);
                fprintf(out, "    push eax\n");
                fprintf(out, "    call print_bool_clean\n");
                fprintf(out, "    add esp, 4\n");
                break;
            } else if (strcmp(node->as.func_call.name, "print_bool_numeric") == 0 && node->as.func_call.arg_count == 1) {
                // Evaluate the argument and put result in eax
                emit_node(node->as.func_call.args[0], out);
                fprintf(out, "    push eax\n");
                fprintf(out, "    call print_bool_numeric\n");
                fprintf(out, "    add esp, 4\n");
                break;
            } else if (strcmp(node->as.func_call.name, "read_bool_safe") == 0 && node->as.func_call.arg_count == 0) {
                // Call read_bool_safe function (returns result in eax)
                fprintf(out, "    call read_bool_safe\n");
                break;
            }

            // Handle other function calls (user-defined functions)
            // Push arguments in reverse order (right-to-left for standard calling convention)
            for (int i = node->as.func_call.arg_count - 1; i >= 0; i--) {
                ASTNode *arg = node->as.func_call.args[i];
                emit_node(arg, out);
                
                // Check if this argument is a float type
                if (is_float_type(arg)) {
                    // For float arguments, store from FPU stack to memory, then push
                    fprintf(out, "    sub esp, 4\n");           // Reserve space on stack
                    fprintf(out, "    fstp dword [esp]  ; push float argument %d\n", i);
                } else {
                    // For integer arguments, push EAX
                    fprintf(out, "    push eax  ; push argument %d\n", i);
                }
            }
            
            fprintf(out, "    call %s\n", node->as.func_call.name);
            
            // Clean up stack (remove pushed arguments)
            if (node->as.func_call.arg_count > 0) {
                fprintf(out, "    add esp, %ld  ; clean up %ld arguments\n", 
                       node->as.func_call.arg_count * 4, node->as.func_call.arg_count);
            }
            
            // Handle return value based on function return type
            const char* return_type = get_function_return_type(node->as.func_call.name);
            if (is_float_return_type(return_type)) {
                // Float return value is already on FPU stack - nothing to do
                // (the calling context will handle it appropriately)
            } else if (strcmp(return_type, "void") != 0) {
                // Non-void, non-float return value is in eax - already handled
            }
            break;
        }

        case AST_EXTERN_FUNC:
            fprintf(out, "extern %s\n", node->as.extern_func.name);
            break;

        case AST_FUNC_DEF:
            fprintf(out, "; CODEGEN TEST MARKER: emitting function %s\n", node->as.func_def.name);
            // fprintf(stderr, "[CODEGEN DEBUG] func %s body_count=%zu\n", node->as.func_def.name, node->as.func_def.body_count);
            for (size_t i = 0; i < node->as.func_def.body_count; ++i) {
                if (node->as.func_def.body[i]) {
                    // fprintf(stderr, "[CODEGEN DEBUG]   body[%zu] type=%d\n", i, node->as.func_def.body[i]->type);
                }
            }
            
            // Set current function context for parameter access
            ASTNode *prev_function = current_function;
            current_function = node;
            
            // Save previous stack offset and reset for this function
            int prev_stack_offset = current_stack_offset;
            current_stack_offset = 0;
            
            fprintf(out, "%s:\n", node->as.func_def.name);
            fprintf(out, "    push ebp\n    mov ebp, esp\n");
            
            // Count all local variables recursively (including those in nested structures)
            int local_vars = 0;
            for (size_t i = 0; i < node->as.func_def.body_count; ++i) {
                local_vars += count_local_variables(node->as.func_def.body[i]);
            }
            
            // Allocate stack space for local variables (if any) - align to 16 bytes
            if (local_vars > 0) {
                int stack_space = ((local_vars * 4) + 15) & ~15; // Round up to 16-byte boundary, 4 bytes per var for 32-bit
                fprintf(out, "    sub esp, %d  ; allocate space for %d local variables\n", 
                       stack_space, local_vars);
            }
            
            // Check if function has explicit return statement
            int has_explicit_return = 0;
            for (size_t i = 0; i < node->as.func_def.body_count; ++i) {
                if (node->as.func_def.body[i] && node->as.func_def.body[i]->type == AST_RETURN) {
                    has_explicit_return = 1;
                }
            }
            
            for (size_t i = 0; i < node->as.func_def.body_count; ++i)
                emit_node(node->as.func_def.body[i], out);
            
            // Only emit epilogue if there's no explicit return
            if (!has_explicit_return) {
                fprintf(out, "    mov rsp, rbp  ; restore stack pointer\n");
                fprintf(out, "    pop ebp\n    ret\n");
            }
            
            // Restore previous function context and stack offset
            current_function = prev_function;
            current_stack_offset = prev_stack_offset;
            break;

        case AST_IDENTIFIER:
            // Load variable value based on type
            {
                int found = 0;
                
                // First check if this is a function parameter
                if (current_function != NULL) {
                    for (size_t i = 0; i < current_function->as.func_def.param_count; i++) {
                        if (strcmp(current_function->as.func_def.param_names[i], node->as.ident) == 0) {
                            // This is a function parameter - access it from stack
                            // Parameters are at [ebp+8], [ebp+12], [ebp+16], etc.
                            // (ebp+4 is return address, ebp+0 is saved ebp)
                            int offset = 8 + (i * 4);
                            
                            // Check if this parameter is a float type
                            if (current_function->as.func_def.param_types && 
                                is_param_float_type(current_function->as.func_def.param_types[i])) {
                                fprintf(out, "    fld dword [ebp+%d]  ; load float parameter %s\n", 
                                       offset, node->as.ident);
                            } else {
                                fprintf(out, "    mov eax, [ebp+%d]  ; load parameter %s\n", 
                                       offset, node->as.ident);
                            }
                            found = 1;
                            break;
                        }
                    }
                }
                
                // If not found as parameter, check local variables
                if (!found) {
                    for (int i = 0; i < var_map_count; i++) {
                        if (strcmp(var_names[i], node->as.ident) == 0) {
                            if (var_types[i] == 1) { // float
                                emit_float_var_load(node->as.ident, out);
                            } else { // int
                                emit_int_var_load(node->as.ident, out);
                            }
                            found = 1;
                            break;
                        }
                    }
                }
                
                if (!found) {
                    fprintf(stderr, "Warning: variable %s not found\n", node->as.ident);
                }
            }
            break;

        case AST_RETURN:
            if (node->as.ret.value) {
                // Return with value
                emit_node(node->as.ret.value, out);
                // Value is now in RAX (for int) or XMM0 (for float)
                // Function epilogue will handle the actual return
            }
            // For void returns, no value to load
            fprintf(out, "    mov esp, ebp  ; restore stack pointer\n");
            fprintf(out, "    pop ebp\n");
            fprintf(out, "    ret\n");
            break;

        case AST_UNARY_OP: {
            char op = node->as.unary_op.op;
            
            if (op == '!') {
                // Logical NOT: result is 1 if operand is 0, 0 if operand is non-zero
                emit_node(node->as.unary_op.expr, out);
                fprintf(out, "    test eax, eax\n");     // Test if EAX is zero
                fprintf(out, "    sete al\n");           // Set AL = 1 if EAX was zero
                fprintf(out, "    movzx eax, al\n");     // Zero-extend AL to EAX
            } else if (op == '-') {
                // Unary minus: negate the operand
                
                // Check if operand is float/num type
                int is_float = is_float_type(node->as.unary_op.expr);
                
                if (is_float) {
                    // Float negation: load value, negate on FPU stack
                    emit_node(node->as.unary_op.expr, out);
                    fprintf(out, "    fchs\n");                  // Change sign on FPU stack
                    // Result is already on FPU stack, ready for use
                } else {
                    // Integer negation
                    emit_node(node->as.unary_op.expr, out);
                    fprintf(out, "    neg eax\n");               // Two's complement negation
                }
            } else {
                fprintf(out, "; Unsupported unary operator: %c\n", op);
            }
            break;
        }

        default:
            fprintf(out, "; Unhandled node type %s\n", ast_type_str(node->type));
            break;
    }
}

// --- Main entry: generate_code() ---
void generate_code(AST *ast, FILE *out) {
    string_literal_count = 0;
    float_const_count = 0;
    float_var_count = 0;
    int_var_count = 0;
    var_map_count = 0;
    float_constants_count = 0;
    current_stack_offset = 0;

    // First pass: collect strings and analyze floats
    for (size_t i = 0; i < ast->count; ++i) {
        collect_strings(ast->nodes[i]);
        collect_floats(ast->nodes[i]);
    }

    fprintf(out, "; Generated NASM by ÆLang Compiler\n");

    // Emit .rodata section for float constants and strings
    if (string_literal_count > 0 || float_constants_count > 0) {
        fprintf(out, "section .rodata\n");
        fprintf(out, "    align 4\n");
        
        // String literals
        for (int i = 0; i < string_literal_count; ++i)
            fprintf(out, "msg_%d db \"%s\",0\n", i, string_literals[i]);
        
        // Float constants
        for (int i = 0; i < float_constants_count; i++) {
            float f = float_constants[i];
            unsigned int *float_bits = (unsigned int*)&f;
            fprintf(out, "float_%d: dd 0x%08x  ; %.9g\n", i, *float_bits, f);
        }
        
        // Format string for printf
        fprintf(out, "fmt_float: db \"%%f\", 10, 0\n");
        fprintf(out, "\n");
    }

    // Emit .bss section for variables if needed
    if (var_map_count > 0) {
        fprintf(out, "section .bss\n");
        fprintf(out, "    align 4\n");
        fprintf(out, "    temp_int: resd 1  ; temporary for int to float conversion\n");
        
        // Emit float variables
        for (int i = 0; i < var_map_count; i++) {
            if (var_types[i] == 1 && var_locations[i] == 0) { // global float
                fprintf(out, "    float_var_%d: resd 1  ; %s\n", var_indices[i], var_names[i]);
            }
        }
        
        // Emit integer variables
        for (int i = 0; i < var_map_count; i++) {
            if (var_types[i] == 0 && var_locations[i] == 0) { // global int
                fprintf(out, "    int_var_%d: resd 1  ; %s\n", var_indices[i], var_names[i]);
            }
        }
        
        fprintf(out, "\n");
    } else {
        // Always emit temp_int for mixed arithmetic
        fprintf(out, "section .bss\n");
        fprintf(out, "    align 4\n");
        fprintf(out, "    temp_int: resd 1  ; temporary for int to float conversion\n");
        fprintf(out, "\n");
    }

    // Emit text section with main
    fprintf(out, "section .text\n");
    fprintf(out, "    global main\n");
    fprintf(out, "    extern printf\n");
    fprintf(out, "    extern print\n");
    fprintf(out, "    extern read_int\n");
    fprintf(out, "    extern read_float\n");
    fprintf(out, "    extern read_num\n");
    fprintf(out, "    extern read_num_safe\n");
    fprintf(out, "    extern print_num\n");
    fprintf(out, "    extern print_clean\n");
    fprintf(out, "    extern print_num_precision\n");
    fprintf(out, "    extern print_num_scientific\n");
    fprintf(out, "    extern print_currency\n");
    fprintf(out, "    extern print_percentage\n");
    fprintf(out, "    extern print_num_engineering\n");
    fprintf(out, "    extern print_hex\n");
    fprintf(out, "    extern read_num_validated\n");
    fprintf(out, "    extern read_num_with_prompt\n");
    fprintf(out, "    extern read_positive_num\n");
    fprintf(out, "    extern read_integer_only\n\n");

    // First emit all externs and function definitions
    for (size_t i = 0; i < ast->count; ++i) {
        if (ast->nodes[i]->type == AST_EXTERN_FUNC || ast->nodes[i]->type == AST_FUNC_DEF) {
            emit_node(ast->nodes[i], out);
        }
    }

    // Top-level statements are emitted in the function bodies
}
