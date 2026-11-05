package io.github.chunob.hub3ds;

/**
 * IMA ADPCM igual ao audioop.lin2adpcm do Python, que é o que o 3DS sabe abrir
 * (ver 3ds/source/som.c). 4 bits por amostra; em cada byte a primeira amostra
 * fica nos 4 bits de cima.
 */
final class Adpcm {
    private static final int[] INDICE = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };
    private static final int[] PASSO = {
        7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66,
        73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408,
        449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
        2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630,
        9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
    };

    int previsao, indice;   // estado: vai no cabeçalho de cada pacote

    /** Compacta n amostras (n par) de pcm em saida (n/2 bytes). */
    void codificar(short[] pcm, int n, byte[] saida, int desloc) {
        int valpred = previsao, index = indice;
        for (int i = 0; i < n; i++) {
            int step = PASSO[index];
            int delta = pcm[i] - valpred;
            int sign = delta < 0 ? 8 : 0;
            if (sign != 0) delta = -delta;
            int code = 0, vpdiff = step >> 3;
            if (delta >= step) { code = 4; delta -= step; vpdiff += step; }
            step >>= 1;
            if (delta >= step) { code |= 2; delta -= step; vpdiff += step; }
            step >>= 1;
            if (delta >= step) { code |= 1; vpdiff += step; }
            valpred += sign != 0 ? -vpdiff : vpdiff;
            if (valpred > 32767) valpred = 32767;
            else if (valpred < -32768) valpred = -32768;
            code |= sign;
            index += INDICE[code];
            if (index < 0) index = 0;
            else if (index > 88) index = 88;
            if ((i & 1) == 0) saida[desloc + i / 2] = (byte) (code << 4);
            else saida[desloc + i / 2] |= (byte) code;
        }
        previsao = valpred;
        indice = index;
    }
}
