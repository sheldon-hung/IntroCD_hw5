# hw5 report

|||
|-:|:-|
|Name|洪慎廷|
|ID|110550162|

## How much time did you spend on this project

> e.g. 2 hours.

about 12 hours

## Project overview

> Please describe the structure of your code and the ideas behind your implementation in an organized way.
> The point is to show us how you deal with the problems. It is not necessary to write a lot of words or paste all of your code here.

In this project, I modified the CodeGenerator.hpp, CodeGenerator.cpp, and the AST nodes. The code strucutre and ideas will be described in the following.

For the CodeGenerator.hpp, I declared some datastructure for saving some information that is needed when visiting across the AST nodes. For example, I use std::map to store the local varriables' location when declaration, and load it when varriable reference. I used a integer to store label number, when generating a label, load the label number
then add 1 to it, in order to make sure there won't have any same labels in the code. Also, when a function call, it is needed to store the number of the parameters, so it is also stored in a variable for generating the code of function declaration and invocation.

For the CodeGenerator.cpp, mostly generate the correspond code according to each node and store or load some information that is needed to be passed on. I implemented the integer type and boolean type code generation, the string type, real type and array type weren't implemented. I mostly use the stack machine model to generate the codes for simplicity, it can be add on to the output file when visiting the AST nodes.

For the AST nodes, the nodes that has control flow, "if", "for", "while", there are some labels and jump statement need to be generated between its' child nodes, but the function "visitChildNodes" will visit all its' child nodes at once. Thus, I add some function that can let me specify which child node to visit.

## What is the hardest you think in this project

> Not required, but bonus point may be given.

I think the hardest part of the project is designing the variable node's gode genertion, because there are many different case to consider. For example, the variable is global or local, is it a constant, is it a function parameter. The cases above will all affect the result of code generation. 

## Feedback to T.A.s

> Not required, but bonus point may be given.
