package io.github.chunob.hub3ds;

import android.content.Context;
import android.graphics.Point;
import android.hardware.display.DisplayManager;
import android.os.SystemClock;
import android.view.Display;

import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetSocketAddress;
import java.net.SocketTimeoutException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.util.HashMap;
import java.util.Map;

/**
 * Controle (UDP 5005): o mesmo pacote que o 3DS manda para o PC ("3DS2": botões,
 * circle pad, C-stick, toque). No celular não dá para criar um controle de Xbox, então
 * cada botão vira um dedo na tela, onde você marcou para o app aberto (MapaControle;
 * marque pela notificação "Botões do controle", por cima do jogo). O analógico vira um
 * dedo que arrasta em volta do ponto marcado, e tocar na tela do 3DS toca no mesmo
 * lugar da tela do celular. Usa o serviço de acessibilidade, como o Espelho.
 */
final class Controle extends Thread {
    static final int PORTA = 5005;
    private static final int ID_TOQUE = 100, ID_ANALOGICO = 101, ID_CSTICK = 102;

    private final Context ctx;
    private DatagramSocket sock;
    private String app = "";
    private Map<Integer, float[]> mapa = new HashMap<>();
    private long mapaLidoEm, legendaEm;
    private boolean tinhaDedos;
    static volatile boolean conectado;
    static volatile long ultimoPacote;

    Controle(Context ctx) {
        super("controle");
        setDaemon(true);
        this.ctx = ctx.getApplicationContext();
    }

    private Point tela() {
        Display d = ((DisplayManager) ctx.getSystemService(Context.DISPLAY_SERVICE)).getDisplay(Display.DEFAULT_DISPLAY);
        Point p = new Point();
        d.getRealSize(p);
        return p;
    }

    @Override public void run() {
        try {
            sock = new DatagramSocket(null);
            sock.setReuseAddress(true);
            sock.bind(new InetSocketAddress(PORTA));
            sock.setSoTimeout(500);
            byte[] buf = new byte[64];
            while (!isInterrupted()) {
                DatagramPacket p = new DatagramPacket(buf, buf.length);
                try {
                    sock.receive(p);
                } catch (SocketTimeoutException e) {
                    if (tinhaDedos && SystemClock.elapsedRealtime() - ultimoPacote > 400) soltarTudo();
                    conectado = SystemClock.elapsedRealtime() - ultimoPacote < 2000;
                    continue;
                }
                if (p.getLength() < 32 || buf[0] != '3' || buf[1] != 'D' || buf[2] != 'S' || buf[3] != '2') continue;
                ultimoPacote = SystemClock.elapsedRealtime();
                conectado = true;
                Estado.viu3ds(p.getAddress().getHostAddress());
                aplicar(ByteBuffer.wrap(buf, 0, 32).order(ByteOrder.LITTLE_ENDIAN));
                responder(p);
            }
        } catch (Exception e) {
            if (!isInterrupted()) android.util.Log.w("3dshub", "Controle parou", e);
        }
    }

    private void soltarTudo() {
        Toque.dedos(new HashMap<>());
        tinhaDedos = false;
    }

    private void aplicar(ByteBuffer b) {
        long agora = SystemClock.elapsedRealtime();
        if (agora - mapaLidoEm > 1000 || !app.equals(Toque.ultimoApp)) {     // o app mudou ou o mapa foi editado
            app = Toque.ultimoApp;
            mapa = MapaControle.ler(ctx, app);
            mapaLidoEm = agora;
        }
        int keys = b.getInt(8);
        int cx = b.getShort(12), cy = b.getShort(14), sx = b.getShort(16), sy = b.getShort(18);
        int tx = b.getShort(20) & 0xFFFF, ty = b.getShort(22) & 0xFFFF;
        boolean tocando = b.get(24) != 0;
        if ((keys & (1 << 3)) != 0 && (keys & (1 << 2)) != 0) keys &= ~((1 << 3) | (1 << 2));   // START+SELECT = sair

        Point t = tela();
        Map<Integer, float[]> dedos = new HashMap<>();
        for (int i = 0; i < MapaControle.ANALOGICO; i++) {
            float[] m = mapa.get(i);
            if (m != null && (keys & MapaControle.BITS[i]) != 0) dedos.put(i, new float[] { m[0] * t.x, m[1] * t.y });
        }
        analogico(dedos, ID_ANALOGICO, mapa.get(MapaControle.ANALOGICO), cx, cy, t);
        analogico(dedos, ID_CSTICK, mapa.get(MapaControle.CSTICK), sx, sy, t);
        if (tocando && tx < 320 && ty < 240) dedos.put(ID_TOQUE, new float[] { (tx + 0.5f) / 320f * t.x, (ty + 0.5f) / 240f * t.y });

        if (!dedos.isEmpty() || tinhaDedos) Toque.dedos(dedos);
        tinhaDedos = !dedos.isEmpty();
    }

    private static void analogico(Map<Integer, float[]> dedos, int id, float[] m, int x, int y, Point t) {
        if (m == null) return;
        float fx = Math.max(-1f, Math.min(1f, x / 150f)), fy = Math.max(-1f, Math.min(1f, y / 150f));
        if (Math.hypot(fx, fy) < 0.15) return;           // zona morta: solto
        float raio = m[2] * t.x;
        dedos.put(id, new float[] { m[0] * t.x + fx * raio, m[1] * t.y - fy * raio });   // circle pad para cima = subir
    }

    /** "3DSA" + perfil (o app) + modo + "mouse": o 3DS mostra conectado e o nome do app. */
    private void responder(DatagramPacket p) throws Exception {
        String nome = MapaControle.nomeDoApp(ctx, app);
        String perfil = nome.isEmpty() ? "Celular" : nome;
        byte[] a = ("3DSA" + SegundaTela.limpo(perfil, 30) + "\ntoques\nmouse").getBytes(StandardCharsets.UTF_8);
        sock.send(new DatagramPacket(a, a.length, p.getSocketAddress()));
        long agora = SystemClock.elapsedRealtime();
        if (agora - legendaEm > 1000) {        // legenda: o que cada botão faz neste app
            legendaEm = agora;
            String[] chaves = { "A", "B", "X", "Y", "L", "R", "ZL", "ZR", "CIMA", "BAIXO", "ESQUERDA", "DIREITA",
                    "START", "SELECT", "CIRCLE", "CSTICK" };
            StringBuilder s = new StringBuilder("3DSM");
            for (int i = 0; i < chaves.length; i++)
                s.append(chaves[i]).append('=').append(mapa.containsKey(i) ? (i >= MapaControle.ANALOGICO ? "arrasta" : "toque") : "-").append('\n');
            byte[] m = s.toString().getBytes(StandardCharsets.UTF_8);
            sock.send(new DatagramPacket(m, m.length, p.getSocketAddress()));
        }
    }

    void parar() {
        interrupt();
        if (sock != null) sock.close();
        Toque.dedos(new HashMap<>());
    }
}
