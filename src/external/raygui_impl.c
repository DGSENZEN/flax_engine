// Single compilation unit for the raygui single-header library.
// Exactly ONE .c file in the whole project may define RAYGUI_IMPLEMENTATION.
// Everywhere else, just #include "raygui.h" for the declarations.
#define RAYGUI_IMPLEMENTATION
#include "raygui.h"
