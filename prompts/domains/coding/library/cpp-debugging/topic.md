name: cpp-debugging
trigger: when a C++ failure needs more than reading the code — which layer to reach for first
tags: #cpp #debugging
weight: 70
reference: none

Three layers, cheapest first: what the compiler and the linter will tell you,
what a sanitizer build will tell you about memory and lifetime, and what a
debugger will tell you when both are quiet. Reach for them in that order; each
entry says what it answers and what it cannot.
