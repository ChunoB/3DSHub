package io.github.chunob.hub3ds;

import android.content.Context;
import android.content.SharedPreferences;

import org.json.JSONArray;
import org.json.JSONObject;

import java.util.HashMap;
import java.util.Map;

/**
 * Onde cada botão do 3DS toca na tela, para cada app (cada jogo tem o seu mapa).
 * Posições em fração da tela (0 a 1), para valer com a tela deitada ou em pé;
 * os analógicos guardam também o raio (fração da largura).
 */
final class MapaControle {
    // nome, bit do botão no 3DS (libctru); os analógicos não têm bit
    static final String[] NOMES = { "A", "B", "X", "Y", "L", "R", "ZL", "ZR", "Cima", "Baixo", "Esquerda",
            "Direita", "START", "SELECT", "Analógico", "C-stick" };
    static final int[] BITS = { 1, 1 << 1, 1 << 10, 1 << 11, 1 << 9, 1 << 8, 1 << 14, 1 << 15, 1 << 6, 1 << 7,
            1 << 5, 1 << 4, 1 << 3, 1 << 2, 0, 0 };
    static final int ANALOGICO = 14, CSTICK = 15;
    static final float RAIO_PADRAO = 0.08f;

    private static SharedPreferences prefs(Context c) {
        return c.getSharedPreferences("controle", Context.MODE_PRIVATE);
    }

    /** índice do botão -> {x, y, raio} */
    static Map<Integer, float[]> ler(Context c, String app) {
        Map<Integer, float[]> m = new HashMap<>();
        try {
            JSONObject o = new JSONObject(prefs(c).getString(app, "{}"));
            for (int i = 0; i < NOMES.length; i++) {
                JSONArray a = o.optJSONArray(NOMES[i]);
                if (a != null) m.put(i, new float[] { (float) a.getDouble(0), (float) a.getDouble(1), (float) a.optDouble(2, RAIO_PADRAO) });
            }
        } catch (Exception ignorado) {}
        return m;
    }

    static void gravar(Context c, String app, Map<Integer, float[]> m) {
        try {
            JSONObject o = new JSONObject();
            for (Map.Entry<Integer, float[]> e : m.entrySet())
                o.put(NOMES[e.getKey()], new JSONArray().put(e.getValue()[0]).put(e.getValue()[1]).put(e.getValue()[2]));
            prefs(c).edit().putString(app, o.toString()).apply();
        } catch (Exception ignorado) {}
    }

    static String nomeDoApp(Context c, String pacote) {
        if (pacote == null || pacote.isEmpty()) return "";
        try {
            return c.getPackageManager().getApplicationLabel(c.getPackageManager().getApplicationInfo(pacote, 0)).toString();
        } catch (Exception e) {
            return pacote;
        }
    }

    private MapaControle() {}
}
