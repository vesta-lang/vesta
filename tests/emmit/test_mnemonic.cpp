#include "emmit/mnemonic.h"
#include <cstdio>
int main() {
    std::printf("%u instrucciones\n", (unsigned)emmit::cuantos_mnemonicos());
    std::printf("mov=%s jmp.je=%s and=%s xor3=%s\n",
                emmit::texto_de(emmit::Mnemonico::i_mov),
                emmit::texto_de(emmit::Mnemonico::i_jmp_je),
                emmit::texto_de(emmit::Mnemonico::i_and),
                emmit::texto_de(emmit::Mnemonico::i_xor3));
    return 0;
}
/* Comprueba que la lista unica de instrucciones produce un enum usable y que el
 * texto de cada mnemonico se recupera por indice.
 *
 * Vale la pena tenerlo: la lista se genero extrayendo las claves de la tabla del
 * emisor, y el primer intento se dejo SIETE fuera -- `mov` entre ellas -- porque
 * el patron no cogia las entradas escritas en varias lineas.  Un conteo que
 * cuadre no demuestra que esten las que hacen falta, asi que se comprueban por
 * nombre las que se sabe que existen. */
