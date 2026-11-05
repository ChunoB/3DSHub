package io.github.chunob.hub3ds;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.net.URLEncoder;
import java.nio.charset.StandardCharsets;
import java.text.Normalizer;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.HashSet;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * Letra sincronizada da música, igual ao karaoke.py do PC: lrclib.net primeiro e, se lá
 * não tiver, NetEase Cloud Music. Grátis, sem cadastro, uma vez por música.
 */
final class Letras {
    /** Uma linha da letra: quando começa (ms) e o texto. */
    static final class Linha {
        final int ms;
        final String texto;
        Linha(int ms, String texto) { this.ms = ms; this.texto = texto; }
    }

    private static final int U = Pattern.CASE_INSENSITIVE | Pattern.UNICODE_CASE | Pattern.UNICODE_CHARACTER_CLASS;
    private static final String LIXO = "official|oficial|video|vídeo|lyric|letra|audio|áudio|clipe|ao vivo|live|hd|4k|"
            + "remaster|visualizer|\\bmv\\b|\\bdvd\\b|kondzilla";

    static String semAcento(String s) {
        return Normalizer.normalize(s == null ? "" : s, Normalizer.Form.NFKD).replaceAll("\\p{M}", "");
    }

    /** Para comparar nomes: sem acento, espaço e pontuação; "&" = "e" = "and". */
    static String normalizar(String s) {
        String t = Pattern.compile("(^|\\W)(&|e|and|y)(?=\\W|$)", U).matcher(semAcento(s).toLowerCase(Locale.ROOT)).replaceAll(" ");
        StringBuilder b = new StringBuilder();
        t.codePoints().filter(Character::isLetterOrDigit).forEach(b::appendCodePoint);
        return b.toString();
    }

    /** O nome inteiro e o nome sem "(Ao Vivo)", "[Remix]" ou "- Remastered". */
    static Set<String> nomesDe(String nome) {
        String base = Pattern.compile("\\s*[\\(\\[][^\\)\\]]*[\\)\\]]|\\s+-\\s.*$", U).matcher(nome == null ? "" : nome).replaceAll("");
        Set<String> s = new HashSet<>();
        for (String x : new String[] { normalizar(nome), normalizar(base) }) if (!x.isEmpty()) s.add(x);
        return s;
    }

    /** "Anitta, MC Zaac feat. Fulano" -> [Anitta, MC Zaac, Fulano]. */
    static List<String> artistasDe(String a) {
        List<String> l = new ArrayList<>();
        for (String x : Pattern.compile(",|\\b(?:feat|ft|featuring)\\b\\.?", U).split(a == null ? "" : a))
            if (!x.trim().isEmpty()) l.add(x.trim());
        return l;
    }

    private static String sub(String regex, String troca, String s) {
        return Pattern.compile(regex, U).matcher(s).replaceAll(troca);
    }

    private static String espacos(String s) { return String.join(" ", s.trim().split("\\s+")).trim(); }

    /** Deixa só "música" e "artista" (títulos do YouTube e do Spotify vêm com sobras). */
    static String[] limparTitulo(String titulo, String artista) {
        String t = sub("\\s+[–—]\\s+", " - ", titulo == null ? "" : titulo);
        t = sub("[\\(\\[][^\\)\\]]*(" + LIXO + ")[^\\)\\]]*[\\)\\]]", "", t);
        t = sub("\\s+M/?V\\.?$", "", t);
        t = sub("\\s+-\\s+[^-]*\\b(remaster\\w*|ao vivo|live|version|versão|edit)\\b[^-]*$", "", t);
        String a = sub("\\s*(-\\s*topic|vevo|official|oficial)$", "", artista == null ? "" : artista.trim());
        int i = t.indexOf(" - ");
        if (i >= 0) {
            String esq = t.substring(0, i).trim(), dir = t.substring(i + 3).trim();
            String na = normalizar(a), ne = normalizar(esq);
            if (na.isEmpty() || (!ne.isEmpty() && (ne.contains(na) || na.contains(ne)))) { a = esq; t = dir; }
        }
        t = sub("\\s*[\\(\\[]\\s*(ft|feat|featuring)\\b[^\\)\\]]*[\\)\\]]", "", t);
        t = sub("\\s+(ft|feat|featuring)\\b\\.?\\s.*$", "", t);
        return new String[] { espacos(t), espacos(a) };
    }

    private static final Pattern MARCA = Pattern.compile("\\[(\\d+):(\\d+(?:\\.\\d+)?)\\]");
    private static final Pattern CREDITO = Pattern.compile("^[\\u4e00-\\u9fff ]+[:：]");

    /** Texto LRC ("[01:02.50]linha") -> linhas; pula os créditos em chinês do NetEase. */
    static List<Linha> lerLrc(String lrc) {
        List<Linha> linhas = new ArrayList<>();
        if (lrc == null) return linhas;
        for (String l : lrc.split("\\r?\\n")) {
            String resto = l.replaceAll("\\[[^\\]]*\\]", "").trim();
            if (CREDITO.matcher(resto).find()) continue;
            Matcher m = MARCA.matcher(l);
            while (m.find()) {
                int ms = (int) ((Integer.parseInt(m.group(1)) * 60 + Double.parseDouble(m.group(2))) * 1000);
                linhas.add(new Linha(ms, SegundaTela.limpo(resto, 160)));
            }
        }
        linhas.sort((x, y) -> x.ms != y.ms ? Integer.compare(x.ms, y.ms) : x.texto.compareTo(y.texto));
        int comTexto = 0;
        for (Linha x : linhas) if (!x.texto.isEmpty()) comTexto++;
        return comTexto >= 5 ? linhas : new ArrayList<>();
    }

    private static String baixar(String url, String... cabecalhos) throws Exception {
        HttpURLConnection c = (HttpURLConnection) new URL(url).openConnection();
        c.setConnectTimeout(8000);
        c.setReadTimeout(8000);
        for (int i = 0; i + 1 < cabecalhos.length; i += 2) c.setRequestProperty(cabecalhos[i], cabecalhos[i + 1]);
        try (InputStream in = c.getInputStream()) {
            ByteArrayOutputStream o = new ByteArrayOutputStream();
            byte[] b = new byte[16384];
            int n;
            while ((n = in.read(b)) > 0) o.write(b, 0, n);
            return o.toString("UTF-8");
        } finally {
            c.disconnect();
        }
    }

    private static String q(String s) throws Exception { return URLEncoder.encode(s, "UTF-8"); }

    private static List<Linha> lrclib(String t, String a, double duracao) throws Exception {
        List<String> arts = artistasDe(a);
        a = arts.isEmpty() ? "" : arts.get(0);
        String[] buscas = {
            "track_name=" + q(t) + "&artist_name=" + q(a),
            "q=" + q((a + " " + t).trim()),
            "q=" + q(t),
        };
        for (String params : buscas) {
            JSONArray r = new JSONArray(baixar("https://lrclib.net/api/search?" + params,
                    "User-Agent", "3DSHub (github.com/ChunoB/3DSHub)"));
            List<JSONObject> comLetra = new ArrayList<>();
            for (int i = 0; i < r.length(); i++) {
                JSONObject x = r.getJSONObject(i);
                if (!x.isNull("syncedLyrics") && !x.optString("syncedLyrics").isEmpty()) comLetra.add(x);
            }
            if (duracao > 0) comLetra.sort((x, y) -> Double.compare(
                    Math.abs(x.optDouble("duration", 0) - duracao), Math.abs(y.optDouble("duration", 0) - duracao)));
            for (int i = 0; i < Math.min(3, comLetra.size()); i++) {
                List<Linha> l = lerLrc(comLetra.get(i).optString("syncedLyrics"));
                if (!l.isEmpty()) return l;
            }
        }
        return new ArrayList<>();
    }

    private static List<Linha> netease(String t, String a, double duracao) throws Exception {
        String nt = normalizar(t);
        Set<String> nomesT = nomesDe(t);
        List<String> pedidos = new ArrayList<>();
        for (String x : artistasDe(a)) pedidos.add(normalizar(x));
        List<String> arts = artistasDe(a);
        String principal = arts.isEmpty() ? "" : arts.get(0);
        String busca = (t + " " + principal).trim();
        Set<Long> vistos = new HashSet<>();
        for (String qq : new LinkedHashSet<>(Arrays.asList(busca, semAcento(busca)))) {
            JSONObject r = new JSONObject(baixar("https://music.163.com/api/search/get?s=" + q(qq) + "&type=1&limit=20",
                    "User-Agent", "Mozilla/5.0", "Referer", "https://music.163.com/"));
            JSONArray songs = r.optJSONObject("result") == null ? null : r.optJSONObject("result").optJSONArray("songs");
            if (songs == null) continue;
            List<JSONObject> musicas = new ArrayList<>();
            for (int i = 0; i < songs.length(); i++) {
                JSONObject s = songs.getJSONObject(i);
                if (vistos.contains(s.optLong("id"))) continue;
                Set<String> nomes = nomesDe(s.optString("name"));
                nomes.retainAll(nomesT);
                if (nomes.isEmpty()) continue;
                boolean artistaOk = pedidos.isEmpty();
                JSONArray as = s.optJSONArray("artists");
                for (int k = 0; as != null && k < as.length() && !artistaOk; k++) {
                    String x = normalizar(as.getJSONObject(k).optString("name"));
                    for (String p : pedidos) if (!x.isEmpty() && !p.isEmpty() && (x.contains(p) || p.contains(x))) artistaOk = true;
                }
                if (artistaOk) musicas.add(s);
            }
            for (JSONObject s : musicas) vistos.add(s.optLong("id"));
            musicas.sort((x, y) -> {
                int a1 = normalizar(x.optString("name")).equals(nt) ? 0 : 1, a2 = normalizar(y.optString("name")).equals(nt) ? 0 : 1;
                if (a1 != a2) return a1 - a2;
                if (duracao <= 0) return 0;
                return Double.compare(Math.abs(x.optDouble("duration", 0) / 1000 - duracao),
                        Math.abs(y.optDouble("duration", 0) / 1000 - duracao));
            });
            for (int i = 0; i < Math.min(6, musicas.size()); i++) {
                JSONObject letra = new JSONObject(baixar("https://music.163.com/api/song/lyric?id=" + musicas.get(i).optLong("id") + "&lv=1",
                        "User-Agent", "Mozilla/5.0", "Referer", "https://music.163.com/"));
                JSONObject lrc = letra.optJSONObject("lrc");
                List<Linha> l = lerLrc(lrc == null ? null : lrc.optString("lyric"));
                if (!l.isEmpty()) return l;
            }
        }
        return new ArrayList<>();
    }

    /** A letra; lista vazia se nenhuma fonte tem; null se um site falhou (vale tentar de novo). */
    static List<Linha> buscar(String titulo, String artista, double duracao) {
        String[] ta = limparTitulo(titulo, artista);
        if (ta[0].isEmpty()) return new ArrayList<>();
        List<String[]> pares = new ArrayList<>();
        pares.add(ta);
        int i = ta[0].indexOf(" - ");
        if (i >= 0) pares.add(new String[] { ta[0].substring(i + 3).trim(), ta[0].substring(0, i).trim() });
        boolean erro = false;
        for (int fonte = 0; fonte < 2; fonte++) {
            for (String[] par : pares) {
                try {
                    List<Linha> l = fonte == 0 ? lrclib(par[0], par[1], duracao) : netease(par[0], par[1], duracao);
                    if (!l.isEmpty()) return l;
                } catch (Exception e) {
                    android.util.Log.w("3dshub", "Karaokê: " + (fonte == 0 ? "lrclib" : "NetEase") + " falhou", e);
                    erro = true;
                    break;
                }
            }
        }
        return erro ? null : new ArrayList<>();
    }

    private Letras() {}
}
