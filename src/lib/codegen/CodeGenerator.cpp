#include "codegen/CodeGenerator.hpp"
#include "visitor/AstNodeInclude.hpp"

#include <map>
#include <algorithm>
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <stdarg.h>

CodeGenerator::CodeGenerator(const std::string &source_file_name,
                             const std::string &save_path,
                             const SymbolManager *const p_symbol_manager)
    : m_symbol_manager_ptr(p_symbol_manager),
      m_source_file_path(source_file_name)
{
    // FIXME: assume that the source file is always xxxx.p
    const auto &real_path =
        save_path.empty() ? std::string{"."} : save_path;
    auto slash_pos = source_file_name.rfind("/");
    auto dot_pos = source_file_name.rfind(".");

    if (slash_pos != std::string::npos)
    {
        ++slash_pos;
    }
    else
    {
        slash_pos = 0;
    }
    auto output_file_path{
        real_path + "/" +
        source_file_name.substr(slash_pos, dot_pos - slash_pos) + ".S"};
    m_output_file.reset(fopen(output_file_path.c_str(), "w"));
    assert(m_output_file.get() && "Failed to open output file");
}

static void dumpInstructions(FILE *p_out_file, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vfprintf(p_out_file, format, args);
    va_end(args);
}

void CodeGenerator::visit(ProgramNode &p_program)
{
    // Generate RISC-V instructions for program header
    // clang-format off
    constexpr const char *const riscv_assembly_file_prologue =
        "    .file \"%s\"\n"
        "    .option nopic\n";
    // clang-format on
    dumpInstructions(m_output_file.get(), riscv_assembly_file_prologue,
                     m_source_file_path.c_str());

    // Reconstruct the hash table for looking up the symbol entry
    // Hint: Use symbol_manager->lookup(symbol_name) to get the symbol entry.
    m_symbol_manager_ptr->reconstructHashTableFromSymbolTable(
        p_program.getSymbolTable());

    auto visit_ast_node = [&](auto &ast_node)
    { ast_node->accept(*this); };
    for_each(p_program.getDeclNodes().begin(), p_program.getDeclNodes().end(), visit_ast_node);
    for_each(p_program.getFuncNodes().begin(), p_program.getFuncNodes().end(), visit_ast_node);

    fp_offset = -8;
    global_decl = false;
    local_variable_offset.clear();

    const char *const emit_main_function_section =
        ".section    .text\n"
        "    .align 2\n"
        "    .globl main\n"
        "    .type main, @function\n"
        "main:\n";

    dumpInstructions(m_output_file.get(), emit_main_function_section);

    // the main function prologue
    const char *const main_function_prologue =
        "    addi sp, sp, -128\n"
        "    sw ra, 124(sp)\n"
        "    sw s0, 120(sp)\n"
        "    addi s0, sp, 128\n";

    dumpInstructions(m_output_file.get(), main_function_prologue);

    const_cast<CompoundStatementNode &>(p_program.getBody()).accept(*this);

    // the main function epilogue
    const char *const main_function_epilogue =
        "    lw ra, 124(sp)\n"
        "    lw s0, 120(sp)\n"
        "    addi sp, sp, 128\n"
        "    jr ra\n"
        "    .size main, .-main\n";

    dumpInstructions(m_output_file.get(), main_function_epilogue);

    // Remove the entries in the hash table
    m_symbol_manager_ptr->removeSymbolsFromHashTable(p_program.getSymbolTable());
}

void CodeGenerator::visit(DeclNode &p_decl)
{
    p_decl.visitChildNodes(*this);
}

void CodeGenerator::visit(VariableNode &p_variable)
{
    if ((int)m_symbol_manager_ptr->getCurrentLevel() == 0 && global_decl) // global variable declaration
    {
        if (p_variable.getConstantPtr()) // is a global constant variable declaration
        {
            const char *const emit_rodata_section =
                ".section    .rodata\n"
                "    .align 2\n";

            dumpInstructions(m_output_file.get(), emit_rodata_section);

            constexpr const char *const emit_symbol_to_global_symbol_table =
                "    .globl %s\n"
                "    .type %s, @object\n"
                "%s:\n"
                "    .word %s\n";

            dumpInstructions(m_output_file.get(), emit_symbol_to_global_symbol_table,
                             p_variable.getNameCString(), p_variable.getNameCString(), p_variable.getNameCString(),
                             p_variable.getConstantPtr()->getConstantValueCString());
        }
        else // isn't a global constant variable declaration
        {
            dumpInstructions(m_output_file.get(), ".comm %s, 4, 4\n", p_variable.getNameCString());
        }
    }
    else if (func_para_num <= 0) // local variable declaration
    {
        fp_offset -= 4;
        local_variable_offset[m_symbol_manager_ptr->lookup(p_variable.getName())] = fp_offset;

        if (p_variable.getConstantPtr())
        {
            constexpr const char *const store_value_to_local_variable =
                "    li t0, %s\n"
                "    sw t0, %d(s0)\n";

            dumpInstructions(m_output_file.get(), store_value_to_local_variable,
                             p_variable.getConstantPtr()->getConstantValueCString(), fp_offset);
        }
    }
    else // function parameter declaration
    {
        fp_offset -= 4;
        local_variable_offset[m_symbol_manager_ptr->lookup(p_variable.getName())] = fp_offset;

        if (para_reg_idx < 8) // a0 ~ a7
        {
            constexpr const char *const store_register_to_stack =
                "    sw a%d, %d(s0)\n";

            dumpInstructions(m_output_file.get(), store_register_to_stack, para_reg_idx, fp_offset);
        }
        else // s8 ~ s11
        {
            constexpr const char *const store_register_to_stack =
                "    sw s%d, %d(s0)\n";

            dumpInstructions(m_output_file.get(), store_register_to_stack, para_reg_idx, fp_offset);
        }

        para_reg_idx++;
        if (para_reg_idx == func_para_num)
        {
            func_para_num = 0;
            para_reg_idx = 0;
        }
    }
}

void CodeGenerator::visit(ConstantValueNode &p_constant_value)
{
    std::string const_value = p_constant_value.getConstantValueCString();
    PType::PrimitiveTypeEnum const_value_type = p_constant_value.getTypePtr()->getPrimitiveType();

    if (const_value_type == PType::PrimitiveTypeEnum::kBoolType)
    {
        if (const_value == "true")
            const_value = "1";
        else // const_value == "false"
            const_value = "0";
    }

    constexpr const char *const load_constant_value =
        "    li t0, %s\n"
        "    addi sp, sp, -4\n"
        "    sw t0, 0(sp)\n";

    dumpInstructions(m_output_file.get(), load_constant_value, const_value.c_str());
}

void CodeGenerator::visit(FunctionNode &p_function)
{
    // Reconstruct the hash table for looking up the symbol entry
    m_symbol_manager_ptr->reconstructHashTableFromSymbolTable(p_function.getSymbolTable());

    constexpr const char *const emit_function_section =
        ".section    .text\n"
        "    .align 2\n"
        "    .globl %s\n"
        "    .type %s, @function\n"
        "%s:\n";

    dumpInstructions(m_output_file.get(), emit_function_section,
                     p_function.getNameCString(), p_function.getNameCString(), p_function.getNameCString());

    fp_offset = -8;
    global_decl = false;
    local_variable_offset.clear();

    const char *const function_prologue =
        "    addi sp, sp, -128\n"
        "    sw ra, 124(sp)\n"
        "    sw s0, 120(sp)\n"
        "    addi s0, sp, 128\n";

    dumpInstructions(m_output_file.get(), function_prologue);

    func_para_num = (int)p_function.getParametersNum(p_function.getParameters());
    para_reg_idx = 0;

    p_function.visitChildNodes(*this);

    func_para_num = para_reg_idx = 0;

    constexpr const char *const main_function_epilogue =
        "    lw ra, 124(sp)\n"
        "    lw s0, 120(sp)\n"
        "    addi sp, sp, 128\n"
        "    jr ra\n"
        "    .size %s, .-%s\n";

    dumpInstructions(m_output_file.get(), main_function_epilogue,
                     p_function.getNameCString(), p_function.getNameCString());

    // Remove the entries in the hash table
    m_symbol_manager_ptr->removeSymbolsFromHashTable(p_function.getSymbolTable());
}

void CodeGenerator::visit(CompoundStatementNode &p_compound_statement)
{
    // Reconstruct the hash table for looking up the symbol entry
    m_symbol_manager_ptr->reconstructHashTableFromSymbolTable(
        p_compound_statement.getSymbolTable());

    p_compound_statement.visitChildNodes(*this);

    // Remove the entries in the hash table
    m_symbol_manager_ptr->removeSymbolsFromHashTable(
        p_compound_statement.getSymbolTable());
}

void CodeGenerator::visit(PrintNode &p_print)
{
    var_ref_mode = 'r';
    p_print.visitChildNodes(*this);

    const char *const print_statement =
        "    lw a0, 0(sp)\n"
        "    addi sp, sp, 4\n"
        "    jal ra, printInt\n";

    dumpInstructions(m_output_file.get(), print_statement);
}

void CodeGenerator::visit(BinaryOperatorNode &p_bin_op)
{
    p_bin_op.visitChildNodes(*this);

    const char *const pop_stack_values =
        "    lw t0, 0(sp)\n"
        "    addi sp, sp, 4\n"
        "    lw t1, 0(sp)\n"
        "    addi sp, sp, 4\n";

    dumpInstructions(m_output_file.get(), pop_stack_values);

    constexpr const char *const arithmetic_boolean_operation =
        "    %s t0, t1, t0\n";

    Operator op_type = p_bin_op.getOp();

    switch (op_type)
    {
    case Operator::kMultiplyOp:
        dumpInstructions(m_output_file.get(), arithmetic_boolean_operation, "mul");
        break;
    case Operator::kDivideOp:
        dumpInstructions(m_output_file.get(), arithmetic_boolean_operation, "div");
        break;
    case Operator::kModOp:
        dumpInstructions(m_output_file.get(), arithmetic_boolean_operation, "rem");
        break;
    case Operator::kPlusOp:
        dumpInstructions(m_output_file.get(), arithmetic_boolean_operation, "add");
        break;
    case Operator::kMinusOp:
        dumpInstructions(m_output_file.get(), arithmetic_boolean_operation, "sub");
        break;
    case Operator::kLessOp:
        dumpInstructions(m_output_file.get(), "    slt t0, t1, t0\n");
        break;
    case Operator::kLessOrEqualOp:
        dumpInstructions(m_output_file.get(), "    slt t0, t0, t1\n    xori t0, t0, 1\n");
        break;
    case Operator::kGreaterOp:
        dumpInstructions(m_output_file.get(), "    slt t0, t0, t1\n");
        break;
    case Operator::kGreaterOrEqualOp:
        dumpInstructions(m_output_file.get(), "    slt t0, t1, t0\n    xori t0, t0, 1\n");
        break;
    case Operator::kEqualOp:
        dumpInstructions(m_output_file.get(),
                         "    slt t2, t1, t0\n    slt t3, t0, t1\n    or t0, t2, t3\n    xori t0, t0, 1\n");
        break;
    case Operator::kNotEqualOp:
        dumpInstructions(m_output_file.get(), "    slt t2, t1, t0\n    slt t3, t0, t1\n    or t0, t2, t3\n");
        break;
    case Operator::kAndOp:
        dumpInstructions(m_output_file.get(), arithmetic_boolean_operation, "and");
        break;
    case Operator::kOrOp:
        dumpInstructions(m_output_file.get(), arithmetic_boolean_operation, "or");
        break;
    default:;
    }

    const char *const store_result_to_stack =
        "    addi sp, sp, -4\n"
        "    sw t0, 0(sp)\n";

    dumpInstructions(m_output_file.get(), store_result_to_stack);
}

void CodeGenerator::visit(UnaryOperatorNode &p_un_op)
{
    p_un_op.visitChildNodes(*this);

    const char *const pop_stack_values =
        "    lw t0, 0(sp)\n"
        "    addi sp, sp, 4\n";

    dumpInstructions(m_output_file.get(), pop_stack_values);

    Operator op_type = p_un_op.getOp();

    switch (op_type)
    {
    case Operator::kNegOp:
        dumpInstructions(m_output_file.get(), "    sub t0, zero, t0\n");
        break;
    case Operator::kNotOp:
        dumpInstructions(m_output_file.get(), "    xori t0, t0, 1\n");
        break;
    default:;
    }

    const char *const store_result_to_stack =
        "    addi sp, sp, -4\n"
        "    sw t0, 0(sp)\n";

    dumpInstructions(m_output_file.get(), store_result_to_stack);
}

void CodeGenerator::visit(FunctionInvocationNode &p_func_invocation)
{
    p_func_invocation.visitChildNodes(*this);

    func_para_num = (int)p_func_invocation.getArguments().size();

    constexpr const char *const load_arguments_to_regisers =
        "    lw %c%d, 0(sp)\n"
        "    addi sp, sp, 4\n";

    for (para_reg_idx = func_para_num - 1; para_reg_idx >= 0; para_reg_idx--)
    {
        if (para_reg_idx < 8)
        {
            dumpInstructions(m_output_file.get(), load_arguments_to_regisers, 'a', para_reg_idx);
        }
        else
        {
            dumpInstructions(m_output_file.get(), load_arguments_to_regisers, 's', para_reg_idx);
        }
    }

    func_para_num = 0;
    para_reg_idx = 0;

    constexpr const char *const call_function =
        "    jal ra, %s\n";

    dumpInstructions(m_output_file.get(), call_function, p_func_invocation.getNameCString());

    const char *const store_return_value_to_stack =
        "    mv t0, a0\n"
        "    addi sp, sp, -4\n"
        "    sw t0, 0(sp)\n";

    dumpInstructions(m_output_file.get(), store_return_value_to_stack);
}

void CodeGenerator::visit(VariableReferenceNode &p_variable_ref)
{
    const SymbolEntry *var_info = m_symbol_manager_ptr->lookup(p_variable_ref.getName());

    if (var_ref_mode == 'l')
    {
        if (var_info->getLevel() == 0) // global variable address
        {
            constexpr const char *const load_global_variable =
                "    addi sp, sp, -4\n"
                "    la t0, %s\n"
                "    sw t0, 0(sp)\n";

            dumpInstructions(m_output_file.get(), load_global_variable, p_variable_ref.getNameCString());
        }
        else // local variable address
        {
            int var_loc = local_variable_offset[var_info];

            constexpr const char *const load_local_variable =
                "    addi t0, s0, %d\n"
                "    addi sp, sp, -4\n"
                "    sw t0, 0(sp)\n";

            dumpInstructions(m_output_file.get(), load_local_variable, var_loc);
        }
    }
    else // var_ref_mode = 'r'
    {
        if (var_info->getLevel() == 0) // global variable value
        {
            constexpr const char *const load_global_variable =
                "    la t0, %s\n"
                "    lw t1, 0(t0)\n"
                "    mv t0, t1\n"
                "    addi sp, sp, -4\n"
                "    sw t0, 0(sp)\n";

            dumpInstructions(m_output_file.get(), load_global_variable, p_variable_ref.getNameCString());
        }
        else // local variable value
        {
            int var_loc = local_variable_offset[var_info];

            constexpr const char *const load_local_variable =
                "    lw t0, %d(s0)\n"
                "    addi sp, sp, -4\n"
                "    sw t0, 0(sp)\n";

            dumpInstructions(m_output_file.get(), load_local_variable, var_loc);
        }
    }

    var_ref_mode = 'r';
}

void CodeGenerator::visit(AssignmentNode &p_assignment)
{
    var_ref_mode = 'l';
    p_assignment.visitChildNodes(*this);

    const char *const assignment_statement =
        "    lw t0, 0(sp)\n"
        "    addi sp, sp, 4\n"
        "    lw t1, 0(sp)\n"
        "    addi sp, sp, 4\n"
        "    sw t0, 0(t1)\n";

    dumpInstructions(m_output_file.get(), assignment_statement);
}

void CodeGenerator::visit(ReadNode &p_read)
{
    var_ref_mode = 'l';
    p_read.visitChildNodes(*this);

    const char *const read_statement =
        "    jal ra, readInt\n"
        "    lw t0, 0(sp)\n"
        "    addi sp, sp, 4\n"
        "    sw a0, 0(t0)\n";

    dumpInstructions(m_output_file.get(), read_statement);
}

void CodeGenerator::visit(IfNode &p_if)
{
    p_if.visitExpressionNode(*this);

    const char *const branch_statement =
        "    lw t0, 0(sp)\n"
        "    addi sp, sp, 4\n"
        "    beq t0, zero, L%d\n";

    int first_label = label_num;
    label_num++;
    dumpInstructions(m_output_file.get(), branch_statement, first_label); // L1

    p_if.visitIfBodyNode(*this);

    constexpr const char *const jump_statement =
        "    j L%d\n";

    if (p_if.hasElse())
    {
        int second_label = label_num;
        label_num++;
        dumpInstructions(m_output_file.get(), jump_statement, second_label);

        dumpInstructions(m_output_file.get(), "L%d:\n", first_label);

        p_if.visitElseBodyNode(*this);

        dumpInstructions(m_output_file.get(), "L%d:\n", second_label); // L2
    }
    else
    {
        dumpInstructions(m_output_file.get(), "L%d:\n", first_label);

        p_if.visitElseBodyNode(*this);
    }
}

void CodeGenerator::visit(WhileNode &p_while)
{
    int first_label = label_num;
    label_num++;
    dumpInstructions(m_output_file.get(), "L%d:\n", first_label);

    p_while.visitConditionNode(*this);

    const char *const branch_statement =
        "    lw t0, 0(sp)\n"
        "    addi sp, sp, 4\n"
        "    beq t0, zero, L%d\n";

    int second_label = label_num;
    label_num++;

    dumpInstructions(m_output_file.get(), branch_statement, second_label);

    p_while.visitBodyNode(*this);

    dumpInstructions(m_output_file.get(), "    j L%d\n", first_label);

    dumpInstructions(m_output_file.get(), "L%d:\n", second_label);
}

void CodeGenerator::visit(ForNode &p_for)
{
    // Reconstruct the hash table for looking up the symbol entry
    m_symbol_manager_ptr->reconstructHashTableFromSymbolTable(
        p_for.getSymbolTable());

    p_for.visitLoopVarInitNodes(*this);

    int first_label = label_num;
    label_num++;
    dumpInstructions(m_output_file.get(), "L%d:\n", first_label);

    const SymbolEntry *loop_var_info = m_symbol_manager_ptr->lookup(p_for.getLoopVarName());
    int loop_var_loc = local_variable_offset[loop_var_info];

    const char *const store_loop_var_value_to_stack =
        "    lw t0, %d(s0)\n"
        "    addi sp, sp, -4\n"
        "    sw t0, 0(sp)\n";

    dumpInstructions(m_output_file.get(), store_loop_var_value_to_stack, loop_var_loc);

    p_for.visitEndConditionNode(*this);

    const char *const load_operands =
        "    lw t0, 0(sp)\n"
        "    addi sp, sp, 4\n"
        "    lw t1, 0(sp)\n"
        "    addi sp, sp, 4\n";

    dumpInstructions(m_output_file.get(), load_operands);

    constexpr const char *const branch_statement =
        "    bge t1, t0, L%d\n";

    int second_label = label_num;
    label_num++;

    dumpInstructions(m_output_file.get(), branch_statement, second_label);

    p_for.visitBodyNode(*this);

    const char *const add_1_to_loop_var =
        "    addi t0, s0, %d\n"
        "    addi sp, sp, -4\n"
        "    sw t0, 0(sp)\n"
        "    lw t0, %d(s0)\n"
        "    addi sp, sp, -4\n"
        "    sw t0, 0(sp)\n"
        "    li t0, 1\n"
        "    addi sp, sp, -4\n"
        "    sw t0, 0(sp)\n"
        "    lw t0, 0(sp)\n"
        "    addi sp, sp, 4\n"
        "    lw t1, 0(sp)\n"
        "    addi sp, sp, 4\n"
        "    add t0, t1, t0\n"
        "    addi sp, sp, -4\n"
        "    sw t0, 0(sp)\n"
        "    lw t0, 0(sp)\n"
        "    addi sp, sp, 4\n"
        "    lw t1, 0(sp)\n"
        "    addi sp, sp, 4\n"
        "    sw t0, 0(t1)\n";

    dumpInstructions(m_output_file.get(), add_1_to_loop_var, loop_var_loc, loop_var_loc);

    dumpInstructions(m_output_file.get(), "    j L%d\n", first_label);

    dumpInstructions(m_output_file.get(), "L%d:\n", second_label);

    // Remove the entries in the hash table
    m_symbol_manager_ptr->removeSymbolsFromHashTable(p_for.getSymbolTable());
}

void CodeGenerator::visit(ReturnNode &p_return)
{
    p_return.visitChildNodes(*this);

    const char *const load_return_value =
        "    lw t0, 0(sp)\n"
        "    addi sp, sp, 4\n"
        "    mv a0, t0\n";

    dumpInstructions(m_output_file.get(), load_return_value);
}
