package io.github.chunob.hub3ds;

import android.content.Context;
import android.media.MediaMetadata;
import android.media.session.MediaController;
import android.media.session.PlaybackState;
import android.os.SystemClock;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.File;
import java.io.FileOutputStream;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetSocketAddress;
import java.net.SocketAddress;
import java.net.SocketTimeoutException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Date;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;
import java.util.zip.CRC32;

/**
 * Karaokê (UDP 5016), o mesmo jogo do PC (pc/karaoke.py), com a música do celular:
 * a letra rola no 3DS, as notas da música saem do som que o celular está tocando
 * (a mesma captura do som do 3DS) e a voz cantada no 3DS ganha nota. Perfis, recordes
 * e as notas gravadas de cada música ficam nos arquivos do app.
 * O protocolo e as regras são os do PC; veja lá os detalhes.
 */
final class Karaoke extends Thread {
    static final int PORTA = 5016;
    private static final int TAXA = 22050, PEDACO = 900;
    private static final float SLOT = 0.1f, NOTA_MIN_S = 0.25f, LATENCIA_VOZ = 0.12f;
    private static final int ANTES = 12, DEPOIS = 36;
    private static final int SEM_NOTA = 255, CANTO = 252, NAO_GRAVADO = 253, INSTRUMENTAL = 100;
    private static final int PERFEITO = 1, OTIMO = 2, BOM = 3, ERROU = 4;
    private static final int[] PONTOS_JULG = { 0, 100, 70, 40, 0 };
    private static final int FACIL = 0, NORMAL = 1, DIFICIL = 2;
    private static final String[] NOMES_DIF = { "F", "N", "D" };
    // por dificuldade: crédito por distância (0, 1, 2 semitons), croma mínimo (-1 = não vale), limites
    private static final float[][] CREDITOS = { { 1f, 1f, 0.5f }, { 1f, 0.75f, 0f }, { 1f, 0f, 0f } };
    private static final float[] CROMA_MIN = { 0.6f, 0.85f, -1f };
    private static final float[][] LIMITES = { { 0.65f, 0.4f, 0.2f }, { 0.7f, 0.45f, 0.25f }, { 0.8f, 0.55f, 0.3f } };
    private static final float TURBO_S = 10;
    private static final int MAX_JOGADORES = 4;

    static float agora() { return SystemClock.elapsedRealtime() / 1000f; }

    // ================================================================ ouvido (notas da música)
    private static volatile float ativoAte;
    private static final float[] buf = new float[4096];
    private static int novos;
    private static volatile float[] croma;      // 12 notas (Dó a Si), normalizado; null = sem som
    private static volatile boolean forte;
    private static float pico = 1e-3f;
    private static final float[] janela = new float[4096];
    private static int[] notaDoBin;
    private static int bin0, bin1;

    /** A captura de som do celular deve rodar (o karaokê está aberto no 3DS). */
    static boolean querOuvir() { return agora() < ativoAte; }

    /** A captura manda aqui o som (mono, 22050 Hz) enquanto querOuvir(). */
    static synchronized void ouvir(float[] amostras, int n) {
        if (notaDoBin == null) {
            for (int i = 0; i < 4096; i++) janela[i] = (float) (0.5 - 0.5 * Math.cos(2 * Math.PI * i / 4095));
            bin0 = (int) Math.ceil(150.0 * 4096 / TAXA);
            bin1 = (int) Math.floor(1100.0 * 4096 / TAXA);
            notaDoBin = new int[bin1 + 1];
            for (int k = bin0; k <= bin1; k++) {
                double f = k * (double) TAXA / 4096;
                notaDoBin[k] = (int) (((Math.round(12 * Math.log(f / 440) / Math.log(2) + 69)) % 12 + 12) % 12);
            }
        }
        for (int i = 0; i < n; i++) {
            System.arraycopy(buf, 1, buf, 0, 4095);
            buf[4095] = amostras[i];
            if (++novos >= 2048) {
                novos = 0;
                analisar();
            }
        }
    }

    private static void analisar() {
        double[] re = new double[4096], im = new double[4096];
        for (int i = 0; i < 4096; i++) re[i] = buf[i] * janela[i];
        fft(re, im);
        float[] c = new float[12];
        for (int k = bin0; k <= bin1; k++) c[notaDoBin[k]] += (float) Math.hypot(re[k], im[k]);
        float maior = 0, soma = 0;
        for (float v : c) maior = Math.max(maior, v);
        pico = Math.max(maior, pico * 0.995f);
        if (maior > 1e-3f) {
            for (int i = 0; i < 12; i++) { c[i] /= maior; soma += c[i]; }
            croma = c;
            forte = maior > pico * 0.08f && soma / 12 < 0.6f;
        } else {
            croma = null;
            forte = false;
        }
    }

    private static void fft(double[] re, double[] im) {
        int n = re.length;
        for (int i = 1, j = 0; i < n; i++) {
            int bit = n >> 1;
            for (; (j & bit) != 0; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) { double t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
        }
        for (int len = 2; len <= n; len <<= 1) {
            double ang = -2 * Math.PI / len, wr = Math.cos(ang), wi = Math.sin(ang);
            for (int i = 0; i < n; i += len) {
                double cr = 1, ci = 0;
                for (int k = 0; k < len / 2; k++) {
                    int a = i + k, b = a + len / 2;
                    double xr = re[b] * cr - im[b] * ci, xi = re[b] * ci + im[b] * cr;
                    re[b] = re[a] - xr; im[b] = im[a] - xi;
                    re[a] += xr; im[a] += xi;
                    double t = cr * wr - ci * wi;
                    ci = cr * wi + ci * wr;
                    cr = t;
                }
            }
        }
    }

    private static int maisForte(float[] c) {
        int m = 0;
        for (int i = 1; i < 12; i++) if (c[i] > c[m]) m = i;
        return m;
    }

    // ================================================================ notas gravadas
    private final File pastaNotas;
    private final Map<Long, byte[]> faixas = new HashMap<>(), suaves = new HashMap<>();
    private final Map<Long, Float> coberturas = new HashMap<>();
    private final Set<Long> mudou = new HashSet<>();

    private static byte[] suavizar(byte[] f) {
        int n = f.length;
        int[] v = new int[n], out = new int[n];
        for (int i = 0; i < n; i++) v[i] = out[i] = f[i] & 0xFF;
        for (int i = 1; i < n - 1; i++)
            if (v[i - 1] == v[i + 1] && v[i - 1] < 12 && v[i] != v[i - 1] && v[i] != NAO_GRAVADO) out[i] = v[i - 1];
        for (int i = 0; i < n; ) {
            int j = i;
            while (j < n && out[j] == out[i]) j++;
            if (out[i] < 12 && j - i < 2) for (int k = i; k < j; k++) out[k] = SEM_NOTA;
            i = j;
        }
        byte[] r = new byte[n];
        for (int i = 0; i < n; i++) r[i] = (byte) out[i];
        return r;
    }

    private static float cobertura(byte[] f) {
        if (f.length == 0) return 0;
        int c = 0;
        for (byte b : f) if ((b & 0xFF) != NAO_GRAVADO) c++;
        return (float) c / f.length;
    }

    private synchronized byte[] faixa(long id, float dur) {
        byte[] f = faixas.get(id);
        if (f == null) {
            try { f = Files.readAllBytes(new File(pastaNotas, String.format("%08x.bin", id)).toPath()); }
            catch (Exception e) { f = new byte[0]; }
            suaves.put(id, suavizar(f));
            coberturas.put(id, cobertura(f));
        }
        int n = dur > 0 ? (int) (dur / SLOT) + 1 : 0;
        if (f.length < n) {
            byte[] g = new byte[n];
            System.arraycopy(f, 0, g, 0, f.length);
            for (int i = f.length; i < n; i++) g[i] = (byte) NAO_GRAVADO;
            f = g;
        }
        faixas.put(id, f);
        return f;
    }

    private synchronized void gravar(long id, float dur, float pos, int nota) {
        byte[] f = faixa(id, dur);
        int i = (int) (pos / SLOT);
        if (i >= 0 && i < f.length && (f[i] & 0xFF) == NAO_GRAVADO) {
            f[i] = (byte) nota;
            mudou.add(id);
        }
    }

    private synchronized byte[] suave(long id) { byte[] s = suaves.get(id); return s == null ? new byte[0] : s; }

    private void salvarNotas() {
        Map<Long, byte[]> copias = new HashMap<>();
        synchronized (this) {
            for (long id : mudou) copias.put(id, faixas.get(id).clone());
            mudou.clear();
        }
        for (Map.Entry<Long, byte[]> e : copias.entrySet()) {
            synchronized (this) {
                suaves.put(e.getKey(), suavizar(e.getValue()));
                coberturas.put(e.getKey(), cobertura(e.getValue()));
            }
            File arq = new File(pastaNotas, String.format("%08x.bin", e.getKey()));
            File tmp = new File(arq.getPath() + ".tmp");
            try (FileOutputStream o = new FileOutputStream(tmp)) { o.write(e.getValue()); }
            catch (Exception ex) { continue; }
            tmp.renameTo(arq);
        }
    }

    // ================================================================ perfis e recordes
    private final File arqPerfis;
    private JSONObject dados;

    private static int[] nivel(int xp) {
        int n = 1;
        while (250 * n * (n + 1) <= xp) n++;
        int base = 250 * (n - 1) * n, prox = 250 * n * (n + 1);
        return new int[] { n, 100 * (xp - base) / (prox - base) };
    }

    private void carregarPerfis() {
        try { dados = new JSONObject(new String(Files.readAllBytes(arqPerfis.toPath()), StandardCharsets.UTF_8)); }
        catch (Exception e) { dados = new JSONObject(); }
        try {
            if (!dados.has("perfis")) dados.put("perfis", new JSONArray());
            if (!dados.has("recordes")) dados.put("recordes", new JSONObject());
            if (!dados.has("proximo")) dados.put("proximo", 1);
        } catch (Exception ignorado) {}
    }

    private void salvarPerfis() {
        File tmp = new File(arqPerfis.getPath() + ".tmp");
        try (FileOutputStream o = new FileOutputStream(tmp)) { o.write(dados.toString(1).getBytes(StandardCharsets.UTF_8)); }
        catch (Exception e) { return; }
        tmp.renameTo(arqPerfis);
    }

    private JSONArray perfis() { return dados.optJSONArray("perfis"); }

    private JSONObject acharPerfil(int pid) {
        JSONArray l = perfis();
        for (int i = 0; i < l.length(); i++) if (l.optJSONObject(i).optInt("id") == pid) return l.optJSONObject(i);
        return null;
    }

    private void criarPerfil(String nome, int cor) throws Exception {
        nome = String.join(" ", nome.trim().split("\\s+")).trim();
        if (nome.length() > 16) nome = nome.substring(0, 16);
        JSONArray l = perfis();
        if (nome.isEmpty() || l.length() >= 12) return;
        Set<Integer> usados = new HashSet<>();
        for (int i = 0; i < l.length(); i++) usados.add(l.getJSONObject(i).optInt("id"));
        int pid = dados.optInt("proximo", 1);
        while (usados.contains(pid) || pid < 1 || pid > 250) pid = pid % 250 + 1;
        dados.put("proximo", pid % 250 + 1);
        l.put(new JSONObject().put("id", pid).put("nome", nome).put("cor", cor % 8).put("xp", 0).put("partidas", 0));
        salvarPerfis();
    }

    private void apagarPerfil(int pid) throws Exception {
        JSONArray l = perfis(), nova = new JSONArray();
        for (int i = 0; i < l.length(); i++) if (l.getJSONObject(i).optInt("id") != pid) nova.put(l.get(i));
        dados.put("perfis", nova);
        salvarPerfis();
    }

    private JSONArray top(long id) {
        JSONObject r = dados.optJSONObject("recordes").optJSONObject(String.format("%08x", id));
        return r == null ? new JSONArray() : r.optJSONArray("top");
    }

    private Map<String, Object> registrar(int pid, long id, String titulo, String artista, Resultado res) {
        Map<String, Object> info = new HashMap<>();
        info.put("recorde", false); info.put("recorde_geral", false); info.put("subiu", false);
        info.put("xp", 0); info.put("nivel", 0);
        try {
            JSONObject p = acharPerfil(pid);
            if (p == null) return info;
            int xp = 50 + res.pontos / 20, antes = nivel(p.optInt("xp"))[0];
            p.put("xp", p.optInt("xp") + xp);
            p.put("partidas", p.optInt("partidas") + 1);
            int depois = nivel(p.optInt("xp"))[0];
            info.put("xp", xp); info.put("nivel", depois); info.put("subiu", depois > antes);
            JSONObject recs = dados.getJSONObject("recordes");
            String chave = String.format("%08x", id);
            JSONObject rec = recs.optJSONObject(chave);
            if (rec == null) recs.put(chave, rec = new JSONObject().put("titulo", titulo).put("artista", artista).put("top", new JSONArray()));
            JSONArray t = rec.getJSONArray("top");
            if (res.pontos > 0) {
                int meu = -1;
                for (int i = 0; i < t.length(); i++) if (t.getJSONObject(i).optInt("perfil") == pid) meu = Math.max(meu, t.getJSONObject(i).optInt("pontos"));
                info.put("recorde", res.pontos > meu);
                info.put("recorde_geral", t.length() == 0 || res.pontos > t.getJSONObject(0).optInt("pontos"));
                List<JSONObject> lista = new ArrayList<>();
                for (int i = 0; i < t.length(); i++) lista.add(t.getJSONObject(i));
                lista.add(new JSONObject().put("perfil", pid).put("nome", p.optString("nome")).put("pontos", res.pontos)
                        .put("estrelas", res.estrelas).put("precisao", res.precisao).put("dif", res.dificuldade)
                        .put("data", new SimpleDateFormat("yyyy-MM-dd", Locale.ROOT).format(new Date())));
                lista.sort((a, b) -> b.optInt("pontos") - a.optInt("pontos"));
                JSONArray nova = new JSONArray();
                for (int i = 0; i < Math.min(5, lista.size()); i++) nova.put(lista.get(i));
                rec.put("top", nova);
            }
            salvarPerfis();
        } catch (Exception e) {
            android.util.Log.w("3dshub", "Karaokê: registrar", e);
        }
        return info;
    }

    // ================================================================ a vez de um jogador
    static final class Resultado {
        int pontos, precisao, estrelas, comboMax, dificuldade;
        int[] contagem = new int[5];
    }

    final class Vez {
        final int pid, cor, dif;
        final String nome;
        int pontos, combo, comboMax, julg, julgSeq;
        int[] contagem = new int[5];
        float turbo, turboAte, somaAcerto, somaDur, tocado;
        float[] nota;              // nota, duração, acerto, tempo com voz (null = nenhuma)
        boolean soComVoz;
        Resultado resultado;
        Map<String, Object> info = new HashMap<>();

        Vez(int pid, String nome, int cor, int dif) { this.pid = pid; this.nome = nome; this.cor = cor; this.dif = dif; }

        int mult() { return 1 + Math.min(3, combo / 8); }

        int passo(Integer alvo, Integer minha, float dt, boolean tocando, float agora, float[] cr, boolean soComVoz) {
            if (!tocando) return 0;
            tocado += dt;
            this.soComVoz = soComVoz;
            if (nota != null && (alvo == null || alvo != (int) nota[0])) julgar(agora);
            if (alvo == null) return 0;
            if (nota == null) nota = new float[] { alvo, 0, 0, 0 };
            float q = 0;
            if (minha != null) {
                int d = Math.abs(minha - alvo) % 12;
                d = Math.min(d, 12 - d);
                q = d <= 2 ? CREDITOS[dif][d] : 0;
                if (CROMA_MIN[dif] >= 0 && cr != null && cr[minha] >= CROMA_MIN[dif]) q = 1;
            }
            nota[1] += dt;
            if (minha != null) nota[3] += dt;
            nota[2] += q * dt;
            return (int) (q * 100);
        }

        void julgar(float agora) {
            float dur = nota[1], acerto = nota[2], comVoz = nota[3];
            nota = null;
            if (dur < NOTA_MIN_S || (soComVoz && comVoz < 0.15f * dur)) return;
            float frac = acerto / dur;
            float[] lim = LIMITES[dif];
            int j = frac >= lim[0] ? PERFEITO : frac >= lim[1] ? OTIMO : frac >= lim[2] ? BOM : ERROU;
            contagem[j]++;
            somaAcerto += frac * dur;
            somaDur += dur;
            combo = j != ERROU ? combo + 1 : 0;
            comboMax = Math.max(comboMax, combo);
            boolean ligado = agora < turboAte;
            pontos += (int) (PONTOS_JULG[j] * Math.min(4f, Math.max(1f, dur / 0.5f)) * mult() * (ligado ? 2 : 1));
            if (j == PERFEITO && !ligado) turbo = Math.min(100, turbo + 12.5f);
            julg = j;
            julgSeq = (julgSeq + 1) % 256;
        }

        void ativarTurbo(float agora) {
            if (turbo >= 100 && agora >= turboAte) { turbo = 0; turboAte = agora + TURBO_S; }
        }

        Resultado fechar(float agora) {
            if (nota != null) julgar(agora);
            Resultado r = new Resultado();
            r.pontos = pontos;
            r.precisao = somaDur > 0 ? Math.round(100 * somaAcerto / somaDur) : 0;
            int[][] faixas = { { 5, 90 }, { 4, 75 }, { 3, 60 }, { 2, 40 }, { 1, 15 } };
            for (int[] f : faixas) if (r.precisao >= f[1]) { r.estrelas = f[0]; break; }
            r.comboMax = comboMax;
            r.contagem = contagem.clone();
            r.dificuldade = dif;
            return resultado = r;
        }
    }

    // ================================================================ a rodada
    static final int CANTANDO = 1, FIM_DA_VEZ = 2, FIM = 3;

    final class Sessao {
        final List<Object[]> jogadores;        // {pid, nome, cor}
        Object[] musica;                       // {id, título, artista} ou null
        final int dif;
        final List<Vez> vezes = new ArrayList<>();
        int estado = CANTANDO;
        float desde = agora(), outraDesde = -1;

        Sessao(List<Object[]> jogadores, Object[] musica, int dif) {
            this.jogadores = jogadores;
            this.musica = musica;
            this.dif = dif;
            vezes.add(novaVez(0));
        }

        Vez novaVez(int i) {
            Object[] j = jogadores.get(i);
            return new Vez((int) j[0], (String) j[1], (int) j[2], dif);
        }

        Vez vez() { return vezes.get(vezes.size() - 1); }

        void proxima() {
            vezes.add(novaVez(vezes.size()));
            estado = CANTANDO;
            desde = agora();
            outraDesde = -1;
        }
    }

    // ================================================================ a música tocando
    /** título, artista, tocando, posição (s), duração (s), quando leu */
    private static final class Musica {
        String titulo, artista;
        boolean tocando;
        float pos, dur, quando;
    }

    private final Context ctx;
    private final Midia midia;
    private volatile Musica musica;
    private final Map<Long, List<Letras.Linha>> letras = new ConcurrentHashMap<>();
    private final Set<Long> buscando = ConcurrentHashMap.newKeySet();
    private final Map<Long, float[]> reprocurar = new ConcurrentHashMap<>();   // {tentativas, quando}
    private final Map<Long, byte[]> canto = new ConcurrentHashMap<>();
    private final LinkedBlockingQueue<String> comandos = new LinkedBlockingQueue<>();
    private volatile float ultimo3ds = -100, ultimoPonto = agora(), ultimoSalvar = agora();
    private int pontosAntigo, acertoAntigo;
    private final List<Integer> alvos = new ArrayList<>();
    private Sessao sessao;
    private DatagramSocket sock;
    private volatile boolean rodando = true;

    Karaoke(Context ctx) {
        super("karaoke");
        setDaemon(true);
        this.ctx = ctx.getApplicationContext();
        this.midia = new Midia(ctx);
        File pasta = new File(this.ctx.getFilesDir(), "karaoke");
        pastaNotas = new File(pasta, "notas");
        pastaNotas.mkdirs();
        arqPerfis = new File(pasta, "perfis.json");
        carregarPerfis();
    }

    static long idMusica(String titulo, String artista) {
        CRC32 c = new CRC32();
        c.update((titulo + "\n" + artista).getBytes(StandardCharsets.UTF_8));
        return c.getValue();
    }

    private static float posicao(Musica m) {
        return m.pos + (m.tocando ? agora() - m.quando : 0);
    }

    private Musica lerMusica() {
        MediaController c = midia.atual();
        if (c == null) return null;
        MediaMetadata md = c.getMetadata();
        PlaybackState s = c.getPlaybackState();
        Musica m = new Musica();
        m.titulo = md == null ? "" : nn(md.getString(MediaMetadata.METADATA_KEY_TITLE));
        m.artista = md == null ? "" : nn(md.getString(MediaMetadata.METADATA_KEY_ARTIST));
        m.dur = md == null ? 0 : Math.max(0, md.getLong(MediaMetadata.METADATA_KEY_DURATION)) / 1000f;
        m.tocando = s != null && s.getState() == PlaybackState.STATE_PLAYING;
        float pos = s == null ? 0 : s.getPosition() / 1000f;
        if (m.tocando && s.getLastPositionUpdateTime() > 0)
            pos += (SystemClock.elapsedRealtime() - s.getLastPositionUpdateTime()) / 1000f * s.getPlaybackSpeed();
        m.pos = Math.max(0, pos);
        m.quando = agora();
        return m;
    }

    private static String nn(String s) { return s == null ? "" : s; }

    private void comando(String qual) {
        MediaController c = midia.atual();
        if (c == null) return;
        MediaController.TransportControls t = c.getTransportControls();
        switch (qual) {
            case "pausar": t.pause(); break;
            case "proxima": t.skipToNext(); break;
            case "anterior": t.skipToPrevious(); t.play(); break;
            case "reiniciar": t.seekTo(0); t.play(); break;
            default: break;
        }
    }

    /** Lê a música 4 vezes por segundo enquanto o karaokê está aberto no 3DS. */
    private void loopMusica() {
        while (rodando) {
            String qual = null;
            try { qual = comandos.poll(250, TimeUnit.MILLISECONDS); } catch (InterruptedException e) { return; }
            try {
                if (qual != null) comando(qual);
                if (agora() - ultimo3ds > 5) continue;
                Musica m = lerMusica();
                musica = m;
                if (m != null && !m.titulo.isEmpty()) {
                    long id = idMusica(m.titulo, m.artista);
                    float[] rp = reprocurar.get(id);
                    if (!letras.containsKey(id) && !buscando.contains(id)) {
                        pontosAntigo = 0;
                        procurar(id, m, 0);
                    } else if (rp != null && rp[1] > 0 && agora() > rp[1]) {
                        procurar(id, m, (int) rp[0]);
                    }
                }
                vigiarVez();
                if (agora() - ultimoSalvar > 10) {
                    ultimoSalvar = agora();
                    salvarNotas();
                }
            } catch (Exception e) {
                android.util.Log.w("3dshub", "Karaokê: música", e);
            }
        }
    }

    private void procurar(long id, Musica m, int tentativas) {
        letras.remove(id);
        buscando.add(id);
        reprocurar.put(id, new float[] { tentativas, 0 });
        new Thread(() -> {
            List<Letras.Linha> l = Letras.buscar(m.titulo, m.artista, m.dur);
            if (l == null && tentativas < 4) reprocurar.put(id, new float[] { tentativas + 1, agora() + 30 * (1 << tentativas) });
            letras.put(id, l == null ? new ArrayList<>() : l);
            buscando.remove(id);
        }, "karaoke-letra").start();
    }

    private synchronized void vigiarVez() {
        Sessao s = sessao;
        Musica m = musica;
        if (s == null || s.estado != CANTANDO || m == null || m.titulo.isEmpty()) return;
        long id = idMusica(m.titulo, m.artista);
        if (s.musica == null) { s.musica = new Object[] { id, m.titulo, m.artista }; return; }
        float agora = agora();
        if (agora - s.desde < 4) return;
        if (id != (long) s.musica[0]) {
            if (s.outraDesde < 0) s.outraDesde = agora;
            else if (agora - s.outraDesde > 3) fimDaVez();
            return;
        }
        s.outraDesde = -1;
        float dur = m.dur, pos = posicao(m);
        if (dur > 5 && dur - 1.2f <= pos && pos <= dur + 5 && s.vez().tocado >= Math.min(dur * 0.6f, dur - 10)) {
            fimDaVez();
            comandos.add("pausar");
        }
    }

    private void fimDaVez() {
        Sessao s = sessao;
        Vez v = s.vez();
        Resultado r = v.fechar(agora());
        if (v.pid != 0 && s.musica != null) v.info = registrar(v.pid, (long) s.musica[0], (String) s.musica[1], (String) s.musica[2], r);
        s.estado = s.vezes.size() >= s.jogadores.size() ? FIM : FIM_DA_VEZ;
        salvarNotas();
    }

    private void voltarMusica() {
        Sessao s = sessao;
        Musica m = musica;
        boolean mesma = s.musica == null || (m != null && !m.titulo.isEmpty() && idMusica(m.titulo, m.artista) == (long) s.musica[0]);
        comandos.add(mesma ? "reiniciar" : "anterior");
    }

    // ================================================================ julgar
    private int alvoAntigo(int freq, int volume) {
        float[] cr = croma;
        int alvo = cr == null ? 255 : maisForte(cr);
        float agora = agora(), dt = Math.min(0.5f, agora - ultimoPonto);
        ultimoPonto = agora;
        if (cr == null || freq == 0 || volume < 8) {
            acertoAntigo = Math.max(0, acertoAntigo - 5);
            return alvo;
        }
        int nota = notaDaFreq(freq);
        acertoAntigo = (int) (cr[nota] * 100);
        Musica m = musica;
        if (m != null && m.tocando) pontosAntigo += (int) (acertoAntigo * dt * 2);
        return alvo;
    }

    private static int notaDaFreq(int freqDhz) {
        return (int) (((Math.round(12 * Math.log(freqDhz / 10.0 / 440) / Math.log(2) + 69)) % 12 + 12) % 12);
    }

    private int[] jogar(int freq, int volume, int flags, Musica m, float pos, long id) {
        float agora = agora(), dt = Math.min(0.2f, agora - ultimoPonto);
        ultimoPonto = agora;
        boolean tocando = m != null && m.tocando;
        float[] cr = croma;
        boolean ouvindoForte = forte;
        Integer viva = null;
        if (cr != null && ouvindoForte) {
            alvos.add(maisForte(cr));
            while (alvos.size() > 3) alvos.remove(0);
            int melhor = alvos.get(alvos.size() - 1), vezes = 0;
            for (int a : alvos) {
                int c = 0;
                for (int b : alvos) if (a == b) c++;
                if (c > vezes) { vezes = c; melhor = a; }
            }
            viva = melhor;
        } else {
            alvos.clear();
        }
        Integer alvo = viva;
        if (m != null && id != 0 && m.dur > 0) {
            if (tocando && cr != null) gravar(id, m.dur, pos - 0.12f, viva == null ? SEM_NOTA : viva);
            byte[] s = suave(id);
            int i = (int) ((pos - LATENCIA_VOZ) / SLOT);
            if (i >= 0 && i < s.length && (s[i] & 0xFF) != NAO_GRAVADO) alvo = (s[i] & 0xFF) == SEM_NOTA ? null : (s[i] & 0xFF);
        }
        byte[] cantoDaMusica = m != null && id != 0 ? cantoDe(id, m.dur) : new byte[0];
        if (cantoDaMusica.length > 0) {
            int j = (int) ((pos - LATENCIA_VOZ) / SLOT);
            if (!(j >= 0 && j < cantoDaMusica.length && cantoDaMusica[j] != 0)) alvo = null;
        }
        Integer minha = freq > 0 && volume >= 8 ? notaDaFreq(freq) : null;
        int acerto = 0;
        synchronized (this) {
            Sessao s = sessao;
            if (s != null && s.estado == CANTANDO && s.musica != null && id == (long) s.musica[0]) {
                if ((flags & 1) != 0) s.vez().ativarTurbo(agora);
                acerto = s.vez().passo(alvo, minha, dt, tocando, agora, ouvindoForte ? cr : null, cantoDaMusica.length == 0);
            }
        }
        return new int[] { alvo == null ? 255 : alvo, acerto };
    }

    /** Casas em que tem letra (a faixa mostra "vai ter canto aqui" antes de saber a nota). */
    private byte[] cantoDe(long id, float dur) {
        List<Letras.Linha> letra = letras.get(id);
        if (letra == null || letra.isEmpty()) return new byte[0];
        byte[] c = canto.get(id);
        if (c != null) return c;
        int n = (int) (Math.max(dur, letra.get(letra.size() - 1).ms / 1000f + 5) / SLOT) + 1;
        c = new byte[n];
        for (int k = 0; k < letra.size(); k++) {
            Letras.Linha l = letra.get(k);
            if (l.texto.trim().isEmpty()) continue;
            int palavras = l.texto.trim().split("\\s+").length;
            int fim = Math.min(Math.min(k + 1 < letra.size() ? letra.get(k + 1).ms : l.ms + 5000,
                    l.ms + Math.max(2500, 550 * palavras + 1200)), l.ms + 8000);
            for (int i = l.ms / 100; i < Math.min(n, fim / 100); i++) c[i] = 1;
        }
        canto.put(id, c);
        return c;
    }

    private byte[] janelaFaixa(long id, float dur, float pos) {
        int atual = (int) (pos / SLOT);
        byte[] crua, sv;
        synchronized (this) {
            byte[] f = faixas.get(id);
            crua = f == null ? new byte[0] : f;
            sv = suave(id);
        }
        byte[] c = cantoDe(id, dur);
        byte[] out = new byte[ANTES + DEPOIS];
        for (int k = 0, i = atual - ANTES; i < atual + DEPOIS; i++, k++) {
            int v = NAO_GRAVADO;
            if (i >= 0 && i < sv.length && (sv[i] & 0xFF) != NAO_GRAVADO) v = sv[i] & 0xFF;
            else if (i <= atual && i >= 0 && i < crua.length && (crua[i] & 0xFF) != NAO_GRAVADO) v = crua[i] & 0xFF;
            else if (i >= 0 && i < c.length && c[i] != 0) v = CANTO;
            if (v < 12 && c.length > 0 && !(i >= 0 && i < c.length && c[i] != 0)) v += INSTRUMENTAL;
            out[k] = (byte) v;
        }
        return out;
    }

    private byte[] estadoJogo(Musica m, float pos, long id, int alvo, int acerto) {
        float agora = agora();
        String titulo = "", artista = "";
        boolean tocando = false;
        int estadoLetra = 2, gravada = 0, progresso = 0;
        byte[] jan;
        if (m != null) {
            titulo = m.titulo; artista = m.artista; tocando = m.tocando;
            List<Letras.Linha> l = letras.get(id);
            estadoLetra = l == null ? 0 : (l.isEmpty() ? 2 : 1);
            Float cob;
            synchronized (this) { faixa(id, m.dur); cob = coberturas.get(id); }
            gravada = cob != null && cob >= 0.8f ? 1 : 0;
            jan = janelaFaixa(id, m.dur, pos);
            progresso = m.dur > 0 ? (int) Math.min(100, 100 * pos / m.dur) : 0;
        } else {
            jan = new byte[ANTES + DEPOIS];
            java.util.Arrays.fill(jan, (byte) NAO_GRAVADO);
        }
        int rodada = 0, vez = 0, n = 0, julgSeq = 0, julg = 0, pontos = 0, combo = 0, turbo = 0, turboS = 0, mult = 1;
        String jogador = "";
        synchronized (this) {
            Sessao s = sessao;
            if (s != null) {
                Vez v = s.vez();
                rodada = s.estado; vez = s.vezes.size() - 1; n = s.jogadores.size(); jogador = v.nome;
                julgSeq = v.julgSeq; julg = v.julg; pontos = v.pontos; combo = v.combo; mult = v.mult();
                turbo = (int) v.turbo; turboS = Math.max(0, (int) Math.ceil(v.turboAte - agora));
            }
        }
        byte[] nome = (SegundaTela.limpo(titulo, 50) + "\n" + SegundaTela.limpo(artista, 40) + "\n"
                + SegundaTela.limpo(jogador, 16)).getBytes(StandardCharsets.UTF_8);
        ByteBuffer b = ByteBuffer.allocate(4 + 28 + jan.length + nome.length).order(ByteOrder.LITTLE_ENDIAN);
        b.put("3DSG".getBytes(StandardCharsets.US_ASCII));
        b.put((byte) (tocando ? 1 : 0)).putInt((int) (pos * 1000)).putInt((int) id).put((byte) estadoLetra).put((byte) gravada)
         .put((byte) alvo).put((byte) julgSeq).put((byte) julg).putInt(pontos).putShort((short) Math.min(combo, 65535))
         .put((byte) mult).put((byte) turbo).put((byte) Math.min(turboS, 255)).put((byte) rodada).put((byte) vez)
         .put((byte) n).put((byte) acerto).put((byte) progresso);
        b.put(jan).put(nome);
        return b.array();
    }

    // ================================================================ perfis e rodadas (3DSP)
    private String lista() {
        StringBuilder b = new StringBuilder();
        JSONArray l = perfis();
        for (int i = 0; i < l.length(); i++) {
            JSONObject p = l.optJSONObject(i);
            int[] nv = nivel(p.optInt("xp"));
            b.append(p.optInt("id")).append('\t').append(SegundaTela.limpo(p.optString("nome"), 16)).append('\t')
             .append(p.optInt("cor")).append('\t').append(nv[0]).append('\t').append(nv[1]).append('\t')
             .append(p.optInt("partidas")).append('\n');
        }
        return b.toString();
    }

    private String tops(long id) {
        StringBuilder b = new StringBuilder();
        JSONArray t = top(id);
        for (int i = 0; t != null && i < t.length(); i++) {
            JSONObject x = t.optJSONObject(i);
            int d = x.optInt("dif", NORMAL);
            b.append("T\t").append(SegundaTela.limpo(x.optString("nome"), 16)).append('\t').append(x.optInt("pontos"))
             .append('\t').append(x.optInt("estrelas")).append('\t').append(d >= 0 && d <= 2 ? NOMES_DIF[d] : "N").append('\n');
        }
        return b.toString();
    }

    private synchronized String comando3ds(char c, byte[] d) throws Exception {
        Sessao s = sessao;
        Musica m = musica;
        long id = m != null && !m.titulo.isEmpty() ? idMusica(m.titulo, m.artista) : 0;
        switch (c) {
            case 'L': return lista();
            case 'N':
                if (d.length > 0) criarPerfil(new String(d, 1, d.length - 1, StandardCharsets.UTF_8), d[0] & 0xFF);
                return lista();
            case 'A':
                if (d.length > 0) apagarPerfil(d[0] & 0xFF);
                return lista();
            case 'S': {
                if (d.length == 0) return "";
                int n = Math.min(d[0] & 0xFF, MAX_JOGADORES);
                List<Object[]> jogadores = new ArrayList<>();
                for (int i = 1; i <= n && i < d.length; i++) {
                    int pid = d[i] & 0xFF;
                    JSONObject p = pid != 0 ? acharPerfil(pid) : null;
                    jogadores.add(p != null ? new Object[] { pid, p.optString("nome"), p.optInt("cor") } : new Object[] { 0, "Convidado", 7 });
                }
                if (jogadores.isEmpty()) jogadores.add(new Object[] { 0, "Convidado", 7 });
                int dif = d.length > 1 + n && (d[1 + n] & 0xFF) <= DIFICIL ? d[1 + n] & 0xFF : NORMAL;
                sessao = new Sessao(jogadores, id != 0 ? new Object[] { id, m.titulo, m.artista } : null, dif);
                if (id != 0) comandos.add("reiniciar");
                return "ok";
            }
            case 'P':
                if (s != null && s.estado == FIM_DA_VEZ) { s.proxima(); voltarMusica(); return "ok"; }
                return "";
            case 'J':
                if (s != null && s.estado == FIM) { sessao = new Sessao(s.jogadores, s.musica, s.dif); voltarMusica(); return "ok"; }
                return "";
            case 'F':
                if (s != null && s.estado == CANTANDO) { fimDaVez(); comandos.add("pausar"); return "ok"; }
                return "";
            case 'X': sessao = null; return "ok";
            case 'C':
                if (s != null && s.estado == CANTANDO) { s.vezes.set(s.vezes.size() - 1, s.novaVez(s.vezes.size() - 1)); s.desde = agora(); }
                comandos.add("reiniciar");
                return "ok";
            case 'Z': comandos.add("proxima"); return "ok";
            case 'R': {
                if (s == null || s.vez().resultado == null) return "";
                Resultado r = s.vez().resultado;
                Map<String, Object> info = s.vez().info;
                StringBuilder b = new StringBuilder();
                b.append(r.pontos).append('\t').append(r.precisao).append('\t').append(r.estrelas).append('\t').append(r.comboMax)
                 .append('\t').append(r.contagem[PERFEITO]).append('\t').append(r.contagem[OTIMO]).append('\t').append(r.contagem[BOM])
                 .append('\t').append(r.contagem[ERROU]).append('\t').append(bit(info.get("recorde"))).append('\t')
                 .append(bit(info.get("recorde_geral"))).append('\t').append(info.getOrDefault("nivel", 0)).append('\t')
                 .append(bit(info.get("subiu"))).append('\t').append(info.getOrDefault("xp", 0)).append('\t')
                 .append(s.vez().pid != 0 ? 1 : 0).append('\n');
                List<Vez> feitas = new ArrayList<>();
                for (Vez v : s.vezes) if (v.resultado != null) feitas.add(v);
                feitas.sort((a, x) -> x.resultado.pontos - a.resultado.pontos);
                for (Vez v : feitas)
                    b.append("J\t").append(SegundaTela.limpo(v.nome, 16)).append('\t').append(v.cor).append('\t')
                     .append(v.resultado.pontos).append('\t').append(v.resultado.estrelas).append('\n');
                if (s.vezes.size() < s.jogadores.size())
                    b.append("N\t").append(SegundaTela.limpo((String) s.jogadores.get(s.vezes.size())[1], 16)).append('\n');
                return b + tops(s.musica != null ? (long) s.musica[0] : id);
            }
            case 'K': return tops(id);
            default: return "";
        }
    }

    private static int bit(Object o) { return Boolean.TRUE.equals(o) ? 1 : 0; }

    // ================================================================ rede
    @Override public void run() {
        Thread t = new Thread(this::loopMusica, "karaoke-musica");
        t.setDaemon(true);
        t.start();
        try {
            sock = new DatagramSocket(null);
            sock.setReuseAddress(true);
            sock.bind(new InetSocketAddress(PORTA));
            sock.setSoTimeout(1000);
            byte[] buf = new byte[512];
            while (rodando) {
                DatagramPacket p = new DatagramPacket(buf, buf.length);
                try { sock.receive(p); } catch (SocketTimeoutException e) { continue; }
                try {
                    byte[] r = pacote(java.util.Arrays.copyOf(buf, p.getLength()), p.getAddress().getHostAddress());
                    if (r != null) sock.send(new DatagramPacket(r, r.length, p.getSocketAddress()));
                } catch (Exception e) {
                    android.util.Log.w("3dshub", "Karaokê: pacote", e);
                }
            }
        } catch (Exception e) {
            if (rodando) android.util.Log.w("3dshub", "Karaokê parou", e);
        }
    }

    private byte[] pacote(byte[] d, String ip) throws Exception {
        if (d.length < 4 || d[0] != '3' || d[1] != 'D' || d[2] != 'S') return null;
        ByteBuffer in = ByteBuffer.wrap(d).order(ByteOrder.LITTLE_ENDIAN);
        if (d[3] == 'K' && d.length >= 7) {
            Estado.viu3ds(ip);
            ultimo3ds = agora();
            ativoAte = ultimo3ds + 5;
            int freq = in.getShort(4) & 0xFFFF, volume = d[6] & 0xFF, flags = d.length >= 8 ? d[7] & 0xFF : 0;
            Musica m = musica;
            float pos = m != null ? posicao(m) : 0;
            long id = m != null && !m.titulo.isEmpty() ? idMusica(m.titulo, m.artista) : 0;
            if ((flags & 2) != 0) {
                int[] r = jogar(freq, volume, flags, m, pos, id);
                return estadoJogo(m, pos, id, r[0], r[1]);
            }
            int alvo = alvoAntigo(freq, volume);
            byte[] nome = new byte[0];
            int estadoLetra = 2;
            boolean tocando = false;
            if (m != null) {
                tocando = m.tocando;
                List<Letras.Linha> l = letras.get(id);
                estadoLetra = l == null ? 0 : (l.isEmpty() ? 2 : 1);
                nome = (SegundaTela.limpo(m.titulo, 50) + "\n" + SegundaTela.limpo(m.artista, 40)).getBytes(StandardCharsets.UTF_8);
            }
            ByteBuffer b = ByteBuffer.allocate(4 + 16 + nome.length).order(ByteOrder.LITTLE_ENDIAN);
            b.put("3DSK".getBytes(StandardCharsets.US_ASCII)).put((byte) (tocando ? 1 : 0)).putInt((int) (pos * 1000))
             .putInt((int) id).put((byte) alvo).put((byte) acertoAntigo).putInt(pontosAntigo).put((byte) estadoLetra).put(nome);
            return b.array();
        }
        if (d[3] == 'L' && d.length >= 10) {
            long id = in.getInt(4) & 0xFFFFFFFFL;
            int parte = in.getShort(8) & 0xFFFF;
            List<Letras.Linha> l = letras.get(id);
            StringBuilder corpo = new StringBuilder();
            if (l != null) for (Letras.Linha x : l) corpo.append(x.ms).append('\t').append(x.texto).append('\n');
            byte[] tudo = corpo.toString().getBytes(StandardCharsets.UTF_8);
            int total = Math.max(1, (tudo.length + PEDACO - 1) / PEDACO);
            int ini = Math.min(tudo.length, parte * PEDACO), fim = Math.min(tudo.length, ini + PEDACO);
            ByteBuffer b = ByteBuffer.allocate(4 + 8 + fim - ini).order(ByteOrder.LITTLE_ENDIAN);
            b.put("3DSL".getBytes(StandardCharsets.US_ASCII)).putInt((int) id).putShort((short) parte).putShort((short) total);
            b.put(tudo, ini, fim - ini);
            return b.array();
        }
        if (d[3] == 'P' && d.length >= 6) {
            byte[] dados = java.util.Arrays.copyOfRange(d, 6, d.length);
            byte[] r = comando3ds((char) (d[5] & 0xFF), dados).getBytes(StandardCharsets.UTF_8);
            if (r.length > 1100) r = java.util.Arrays.copyOf(r, 1100);
            byte[] saida = new byte[6 + r.length];
            saida[0] = '3'; saida[1] = 'D'; saida[2] = 'S'; saida[3] = 'P'; saida[4] = d[4]; saida[5] = d[5];
            System.arraycopy(r, 0, saida, 6, r.length);
            return saida;
        }
        return null;
    }

    void parar() {
        rodando = false;
        interrupt();
        if (sock != null) sock.close();
        salvarNotas();
    }
}
