package io.github.chunob.hub3ds;

import android.app.Notification;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.content.Intent;
import android.os.SystemClock;

import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetSocketAddress;
import java.net.SocketAddress;
import java.net.SocketTimeoutException;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Random;

/**
 * Pescaria (UDP 5015), o mesmo jogo do PC (pc/pesca.py): o lago aparece na tela do
 * celular (PescariaActivity) e cada 3DS é a vara de um pescador (até 4).
 *
 * 3DS -> "3DSF" + s16 mira + u8 evento + u8 tipo (1 lançar, 2 recolher) + u8 força + u16 giro
 * cel -> "3DSF" + u8 estado + u8 jogador + u8 tensão + u8 distância + u16 pontos + u8 mordiscando + texto
 */
final class Pescaria extends Thread {
    static final int PORTA = 5015;
    static final float W = 1280, H = 720, AGUA_Y0 = 110, AGUA_Y1 = 600;
    static final int[] CORES = { 0xFFE8483C, 0xFF3C8CE8, 0xFFF2B632, 0xFF5CC85C };
    static final String[] NOMES_CORES = { "Vermelho", "Azul", "Amarelo", "Verde" };
    static final int MIRANDO = 0, VOANDO = 1, NA_AGUA = 2, FISGOU = 3, PUXANDO = 4, PEGOU = 5, PERDEU = 6;

    /** nome, corpo, barbatana, comprimento, cm, pontos, força, raridade */
    static final class Especie {
        final String nome; final int corpo, barbatana; final float comp, cm, pontos, forca, raridade;
        Especie(String n, int c, int b, float comp, float cm, float pts, float f, float r) {
            nome = n; corpo = c; barbatana = b; this.comp = comp; this.cm = cm; pontos = pts; forca = f; raridade = r;
        }
    }

    static final Especie[] ESPECIES = {
        new Especie("Lambari", 0xFFE8D44C, 0xFFC8A830, 34, 12, 10, 0.4f, 10),
        new Especie("Tilápia", 0xFF8CA4C0, 0xFF5C7490, 48, 30, 20, 0.6f, 8),
        new Especie("Pacu", 0xFFE89448, 0xFFB86C2C, 56, 40, 30, 0.7f, 6),
        new Especie("Tucunaré", 0xFF7CB85C, 0xFFE8743C, 64, 55, 45, 0.9f, 4),
        new Especie("Dourado", 0xFFF4C430, 0xFFE89418, 74, 70, 80, 1.1f, 2),
        new Especie("Bota velha", 0xFF5C4A3C, 0xFF3C2E24, 40, 0, 1, 0.2f, 2),
    };

    static final Random sorte = new Random();

    static float uniforme(float a, float b) { return a + sorte.nextFloat() * (b - a); }

    static final class Peixe {
        final Especie especie;
        final float tam;
        float x, y, ang, vel;
        Jogador alvo;
        float mordiscaAte;       // segundos (relógio do jogo); 0 = não
        boolean preso;

        Peixe() {
            float total = 0;
            for (Especie e : ESPECIES) total += e.raridade;
            float r = sorte.nextFloat() * total;
            Especie esc = ESPECIES[0];
            for (Especie e : ESPECIES) { if ((r -= e.raridade) <= 0) { esc = e; break; } }
            especie = esc;
            tam = uniforme(0.75f, 1.35f);
            x = uniforme(80, W - 80);
            y = uniforme(AGUA_Y0 + 40, AGUA_Y1 - 40);
            ang = uniforme(0, (float) (2 * Math.PI));
            vel = uniforme(30, 60);
        }

        float comp() { return especie.comp * tam; }
    }

    static final class Jogador {
        final int numero;
        final SocketAddress origem;
        final float x;
        int estado = MIRANDO, mira, pontos, giro;
        Integer ultimoEvento;
        float visto;
        float bx, by;
        float vooX0, vooY0, vooX1, vooY1, vooT0;
        Peixe peixe;
        float tensao, distancia, parado, proximoPuxao, fisgouEm;
        String mensagem = "";
        float mensagemAte;

        Jogador(int numero, SocketAddress origem, float agora) {
            this.numero = numero;
            this.origem = origem;
            x = 200 + numero * 260;
            bx = x;
            by = 640;
            visto = agora;
        }

        void avisar(String texto, float segundos) {
            mensagem = texto;
            mensagemAte = relogio() + segundos;
        }
    }

    static float relogio() { return SystemClock.elapsedRealtime() / 1000f; }

    // estado do jogo (lido pela tela; protegido por "this")
    final Map<SocketAddress, Jogador> jogadores = new HashMap<>();
    final List<Peixe> peixes = new ArrayList<>();

    private final HubService hub;
    private DatagramSocket sock;
    private Thread jogo;
    private volatile boolean rodando = true;

    Pescaria(HubService hub) {
        super("pescaria");
        setDaemon(true);
        this.hub = hub;
        for (int i = 0; i < 9; i++) peixes.add(new Peixe());
    }

    static volatile Pescaria atual;

    // ------------------------------------------------------------ rede
    @Override public void run() {
        atual = this;
        jogo = new Thread(this::loopJogo, "pescaria-jogo");
        jogo.setDaemon(true);
        jogo.start();
        try {
            sock = new DatagramSocket(null);
            sock.setReuseAddress(true);
            sock.bind(new InetSocketAddress(PORTA));
            sock.setSoTimeout(1000);
            byte[] buf = new byte[64];
            while (rodando) {
                DatagramPacket p = new DatagramPacket(buf, buf.length);
                try {
                    sock.receive(p);
                } catch (SocketTimeoutException e) {
                    continue;
                }
                if (p.getLength() < 11 || buf[0] != '3' || buf[1] != 'D' || buf[2] != 'S' || buf[3] != 'F') continue;
                int mira = (short) ((buf[4] & 0xFF) | (buf[5] << 8));
                int evento = buf[6] & 0xFF, tipo = buf[7] & 0xFF, forca = buf[8] & 0xFF;
                int giro = (buf[9] & 0xFF) | (buf[10] & 0xFF) << 8;
                Estado.viu3ds(p.getAddress().getHostAddress());
                byte[] resposta = receber(p.getSocketAddress(), mira, evento, tipo, forca, giro);
                if (resposta != null) sock.send(new DatagramPacket(resposta, resposta.length, p.getSocketAddress()));
            }
        } catch (Exception e) {
            if (rodando) android.util.Log.w("3dshub", "Pescaria parou", e);
        }
    }

    private synchronized byte[] receber(SocketAddress origem, int mira, int evento, int tipo, int forca, int giro) {
        float agora = relogio();
        Jogador j = jogadores.get(origem);
        if (j == null) {
            int livre = -1;
            for (int n = 0; n < 4 && livre < 0; n++) {
                boolean usado = false;
                for (Jogador x : jogadores.values()) usado |= x.numero == n;
                if (!usado) livre = n;
            }
            if (livre < 0) return null;
            j = new Jogador(livre, origem, agora);
            jogadores.put(origem, j);
            j.avisar("Você é o pescador " + NOMES_CORES[livre] + "!", 4);
            abrirLago();
        }
        j.visto = agora;
        j.mira = Math.max(-100, Math.min(100, mira));
        j.giro = giro;
        if (j.ultimoEvento == null) j.ultimoEvento = evento;     // o primeiro pacote não é uma ação
        else if (evento != j.ultimoEvento) {
            j.ultimoEvento = evento;
            acao(j, tipo, forca, agora);
        }
        String msg = agora < j.mensagemAte ? j.mensagem : "";
        boolean mordisca = j.peixe != null && j.estado == NA_AGUA && agora < j.peixe.mordiscaAte;
        byte[] texto = msg.getBytes(StandardCharsets.UTF_8);
        byte[] r = new byte[11 + texto.length];
        r[0] = '3'; r[1] = 'D'; r[2] = 'S'; r[3] = 'F';
        r[4] = (byte) j.estado;
        r[5] = (byte) (j.numero + 1);
        r[6] = (byte) (int) j.tensao;
        r[7] = (byte) (int) j.distancia;
        int pts = Math.min(65535, j.pontos);
        r[8] = (byte) pts; r[9] = (byte) (pts >> 8);
        r[10] = (byte) (mordisca ? 1 : 0);
        System.arraycopy(texto, 0, r, 11, texto.length);
        return r;
    }

    private void acao(Jogador j, int tipo, int forca, float agora) {
        if (tipo == 1 && j.estado == MIRANDO) {
            forca = Math.max(5, Math.min(100, forca));
            j.vooX0 = j.x + 18; j.vooY0 = 590;
            j.vooX1 = Math.max(60, Math.min(W - 60, j.x + j.mira * 4.5f));
            j.vooY1 = AGUA_Y1 - 30 - forca / 100f * (AGUA_Y1 - AGUA_Y0 - 70);
            j.vooT0 = agora;
            j.estado = VOANDO;
        } else if (tipo == 2 && (j.estado == NA_AGUA || j.estado == VOANDO || j.estado == FISGOU || j.estado == PUXANDO)) {
            soltar(j);
            j.estado = MIRANDO;
            j.avisar("Linha recolhida.", 2);
        }
    }

    private void soltar(Jogador j) {
        if (j.peixe != null) {
            j.peixe.alvo = null;
            j.peixe.preso = false;
            j.peixe.mordiscaAte = 0;
            j.peixe = null;
        }
    }

    // ------------------------------------------------------------ jogo
    private void loopJogo() {
        float antes = relogio();
        while (rodando) {
            SystemClock.sleep(33);
            float agora = relogio();
            try {
                passo(Math.min(0.1f, agora - antes), agora);
            } catch (RuntimeException e) {
                android.util.Log.w("3dshub", "Pescaria: jogo", e);
            }
            antes = agora;
        }
    }

    private synchronized void passo(float dt, float agora) {
        jogadores.values().removeIf(j -> {
            if (agora - j.visto > 5) { soltar(j); return true; }
            return false;
        });
        for (Jogador j : jogadores.values()) passoJogador(j, dt, agora);
        for (Peixe p : new ArrayList<>(peixes)) passoPeixe(p, dt, agora);
    }

    private void passoJogador(Jogador j, float dt, float agora) {
        float giro = Math.min(j.giro / 720f, 1.6f);
        if (j.estado == VOANDO) {
            float t = Math.min(1f, (agora - j.vooT0) / 0.7f);
            j.bx = j.vooX0 + (j.vooX1 - j.vooX0) * t;
            j.by = j.vooY0 + (j.vooY1 - j.vooY0) * t - (float) Math.sin(t * Math.PI) * 120;
            if (t >= 1) j.estado = NA_AGUA;
        } else if (j.estado == NA_AGUA) {
            Peixe p = j.peixe;
            if (p != null && p.mordiscaAte != 0 && agora >= p.mordiscaAte) {
                j.estado = FISGOU;
                j.fisgouEm = agora;
                j.avisar("FISGOU! Gire o molinete!", 1.5f);
            }
        } else if (j.estado == FISGOU) {
            if (giro > 0.25f) {
                j.estado = PUXANDO;
                j.tensao = 30; j.distancia = 100; j.parado = 0;
                j.peixe.preso = true;
                j.proximoPuxao = agora + uniforme(0.6f, 1.4f);
            } else if (agora - j.fisgouEm > 1.4f) {
                soltar(j);
                j.estado = PERDEU;
                j.avisar("Demorou... o peixe soltou a isca.", 2.5f);
            }
        } else if (j.estado == PUXANDO) {
            Peixe p = j.peixe;
            float forca = p.especie.forca * p.tam;
            j.distancia -= giro * 22 * dt;
            j.tensao += (giro * 55 - 30) * dt;
            if (agora >= j.proximoPuxao) {
                float puxao = uniforme(0.4f, 1f) * forca;
                j.tensao += puxao * 22;
                j.distancia = Math.min(100, j.distancia + puxao * 6);
                j.proximoPuxao = agora + uniforme(0.5f, 1.5f) / Math.max(0.5f, forca);
            }
            j.tensao = Math.max(0, j.tensao);
            j.parado = giro < 0.1f ? j.parado + dt : 0;
            float k = 1 - j.distancia / 100;
            j.bx += (j.x + 18 - j.bx) * k * dt * 2;
            j.by += (590 - j.by) * k * dt * 2;
            if (j.tensao >= 100) {
                soltar(j);
                j.estado = PERDEU;
                j.avisar("A linha arrebentou!", 2.5f);
            } else if (j.parado > 3) {
                soltar(j);
                j.estado = PERDEU;
                j.avisar("Ficou parado... o peixe fugiu.", 2.5f);
            } else if (j.distancia <= 0) {
                int ganho = Math.max(1, Math.round(p.especie.pontos * p.tam));
                j.pontos += ganho;
                if (p.especie.cm > 0)
                    j.avisar("Pegou um " + p.especie.nome + " de " + Math.round(p.especie.cm * p.tam) + " cm! +" + ganho, 4);
                else
                    j.avisar("Pegou uma " + p.especie.nome + "... +" + ganho, 4);
                peixes.remove(p);
                peixes.add(new Peixe());
                j.peixe = null;
                j.estado = PEGOU;
            }
        }
        if ((j.estado == PEGOU || j.estado == PERDEU) && agora >= j.mensagemAte) j.estado = MIRANDO;
        if (j.estado == MIRANDO) { j.bx = j.x + 18; j.by = 640; }
    }

    private void passoPeixe(Peixe p, float dt, float agora) {
        if (p.preso) {
            Jogador dono = null;
            for (Jogador j : jogadores.values()) if (j.peixe == p) dono = j;
            if (dono != null) {
                p.x += (dono.bx - p.x) * Math.min(1, dt * 6) + uniforme(-2, 2);
                p.y += (dono.by + 12 - p.y) * Math.min(1, dt * 6) + uniforme(-2, 2);
                p.ang = (float) (Math.atan2(dono.by - p.y, dono.bx - p.x) + Math.PI);
                return;
            }
            p.preso = false;
        }
        if (p.alvo == null) {
            for (Jogador j : jogadores.values()) {
                if (j.estado == NA_AGUA && j.peixe == null
                        && Math.hypot(j.bx - p.x, j.by - p.y) < 260 && sorte.nextFloat() < dt * 0.35f) {
                    p.alvo = j;
                    j.peixe = p;
                    break;
                }
            }
        }
        Jogador j = p.alvo;
        if (j != null && (j.peixe != p || (j.estado != NA_AGUA && j.estado != FISGOU))) {
            p.alvo = null;
            p.mordiscaAte = 0;
            j = null;
        }
        if (j != null) {
            double d = Math.hypot(j.bx - p.x, j.by - p.y);
            if (d > 18) {
                p.ang = (float) Math.atan2(j.by - p.y, j.bx - p.x);
                p.vel = 70;
            } else {
                p.vel = 0;
                if (p.mordiscaAte == 0) p.mordiscaAte = agora + uniforme(1f, 3f);
            }
        } else if (sorte.nextFloat() < dt * 0.4f) {
            p.ang += uniforme(-1.2f, 1.2f);
            p.vel = uniforme(25, 60);
        }
        p.x += Math.cos(p.ang) * p.vel * dt;
        p.y += Math.sin(p.ang) * p.vel * dt * 0.6f;
        if (!(p.x > 60 && p.x < W - 60)) {
            p.ang = (float) (Math.PI - p.ang);
            p.x = Math.max(60, Math.min(W - 60, p.x));
        }
        if (!(p.y > AGUA_Y0 + 25 && p.y < AGUA_Y1 - 25)) {
            p.ang = -p.ang;
            p.y = Math.max(AGUA_Y0 + 25, Math.min(AGUA_Y1 - 25, p.y));
        }
    }

    // ------------------------------------------------------------ o lago na tela
    private void abrirLago() {
        if (PescariaActivity.aberta) return;
        Intent i = new Intent(hub, PescariaActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        try {
            hub.startActivity(i);     // funciona com a permissão "sobrepor a outros apps"
        } catch (Exception ignorado) {}
        PendingIntent abrir = PendingIntent.getActivity(hub, 3, i, PendingIntent.FLAG_IMMUTABLE);
        hub.getSystemService(NotificationManager.class).notify(3, new Notification.Builder(hub, HubService.CANAL)
                .setSmallIcon(R.drawable.ic_notificacao)
                .setContentTitle("Pescaria")
                .setContentText("Um pescador entrou. Toque para abrir o lago")
                .setContentIntent(abrir)
                .setAutoCancel(true)
                .build());
    }

    void parar() {
        rodando = false;
        interrupt();
        if (sock != null) sock.close();
        if (atual == this) atual = null;
    }
}
