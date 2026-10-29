// Som do PC tocando no 3DS (ver som.c).
#pragma once

#include <stdbool.h>

bool somIniciar(void);      // liga o som e pede ao PC; false = sem NDSP (falta o dspfirm.cdc)
void somEncerrar(void);     // desliga (antes do HOME e ao sair)
void somAtualizar(void);    // chame sempre (a cada ~16 ms): pede ao PC e guarda o que chegou
bool somLigado(void);
bool somChegando(void);     // chegou som do PC no último segundo
