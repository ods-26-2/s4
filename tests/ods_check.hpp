#pragma once

#include <cstdio>
#include <cstdlib>

// Verificacao de teste que NAO depende de NDEBUG.
//
// Por que existir: o build oficial (CMAKE_BUILD_TYPE=Release, tambem usado no
// Dockerfile) define NDEBUG, e nesse modo assert() e compilado fora — um teste
// escrito so com assert() passa sem verificar nada. ODS_CHECK vale em qualquer
// modo de build e imprime arquivo, linha e expressao quando falha.
#define ODS_CHECK(condition)                                                           \
    do {                                                                               \
        if (!(condition)) {                                                            \
            std::fprintf(stderr, "CHECK falhou: %s\n  em %s:%d\n", #condition,         \
                         __FILE__, __LINE__);                                          \
            std::abort();                                                              \
        }                                                                              \
    } while (false)
