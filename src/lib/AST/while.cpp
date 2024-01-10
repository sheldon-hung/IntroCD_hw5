#include "AST/while.hpp"

void WhileNode::visitChildNodes(AstNodeVisitor &p_visitor)
{
    m_condition->accept(p_visitor);
    m_body->accept(p_visitor);
}

void WhileNode::visitConditionNode(AstNodeVisitor &p_visitor)
{
    m_condition->accept(p_visitor);
}

void WhileNode::visitBodyNode(AstNodeVisitor &p_visitor)
{
    m_body->accept(p_visitor);
}