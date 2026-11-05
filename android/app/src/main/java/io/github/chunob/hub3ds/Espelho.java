package io.github.chunob.hub3ds;

import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Rect;
import android.graphics.RectF;

import java.io.BufferedInputStream;
import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.InetSocketAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;

/**
 * Espelho (TCP 5008), o mesmo protocolo do PC (pc/espelho.py): a tela do celular na
 * tela de cima do 3DS, com zoom; tocar no 3DS toca no celular e o teclado do 3DS
 * digita nele (precisa do serviço de acessibilidade "3DS Hub (toques do 3DS)").
 *
 *   "L"         -> "3DSL" + u32 tamanho + títulos (só "Tela do celular")
 *   "S n"       -> escolhe
 *   "F [z x y]" -> "3DSF"/"3DSZ" + quadro JPEG 400x240
 *   "T u v b"   -> toque (b 2 = toque longo); "R u v n" -> rolar; "M u v" -> nada
 *   "K hex"     -> digita; "E tecla" -> enter, backspace, tab, esc, voltar, inicio, recentes
 */
final class Espelho extends Thread {
    static final int PORTA = 5008;
    private static final int LARGURA = 400, ALTURA = 240, QUALIDADE = 65;

    private final HubService hub;
    private ServerSocket servidor;
    private final Paint pintura = new Paint(Paint.FILTER_BITMAP_FLAG);
    private final Bitmap saida = Bitmap.createBitmap(LARGURA, ALTURA, Bitmap.Config.ARGB_8888);
    // o que o último quadro mostrou (em pixels da captura), para converter os toques
    private float rx, ry, rw, rh;
    private boolean temRegiao;

    Espelho(HubService hub) {
        super("espelho");
        setDaemon(true);
        this.hub = hub;
    }

    @Override public void run() {
        try {
            servidor = new ServerSocket();
            servidor.setReuseAddress(true);
            servidor.bind(new InetSocketAddress(PORTA));
            while (!isInterrupted()) {
                try (Socket s = servidor.accept()) {
                    s.setTcpNoDelay(true);
                    s.setSoTimeout(300000);
                    Estado.viu3ds(s.getInetAddress().getHostAddress());
                    atender(s);
                } catch (Exception e) {
                    if (isInterrupted()) return;
                } finally {
                    CapturaTela c = hub.captura();
                    if (c != null) c.pausar();
                    temRegiao = false;
                }
            }
        } catch (Exception e) {
            if (!isInterrupted()) android.util.Log.w("3dshub", "Espelho parou", e);
        }
    }

    private static String linha(InputStream in) throws Exception {
        StringBuilder b = new StringBuilder();
        int c;
        while ((c = in.read()) >= 0) {
            if (c == '\n') return b.toString().trim();
            b.append((char) c);
        }
        return b.length() > 0 ? b.toString().trim() : null;
    }

    private void atender(Socket s) throws Exception {
        InputStream in = new BufferedInputStream(s.getInputStream());
        OutputStream out = s.getOutputStream();
        while (true) {
            String cmd = linha(in);
            if (cmd == null) return;
            String[] p = cmd.split(" ");
            switch (p[0]) {
                case "L": {
                    String t = hub.captura() != null ? "Tela do celular" : "Tela do celular (permita no app do celular)";
                    byte[] titulos = t.getBytes(StandardCharsets.UTF_8);
                    out.write(cabecalho("3DSL", titulos.length, 0));
                    out.write(titulos);
                    out.flush();
                    break;
                }
                case "S":
                    temRegiao = false;
                    break;
                case "F": {
                    boolean comZoom = p.length == 4;
                    int zoom = 100, cx = 5000, cy = 5000;
                    try {
                        if (comZoom) { zoom = Integer.parseInt(p[1]); cx = Integer.parseInt(p[2]); cy = Integer.parseInt(p[3]); }
                    } catch (NumberFormatException ignorado) {}
                    int[] centro = { cx, cy };
                    byte[] jpeg = quadro(zoom, centro);
                    if (comZoom) {
                        out.write(cabecalho("3DSZ", jpeg.length, 4));
                        out.write(new byte[] { (byte) centro[0], (byte) (centro[0] >> 8), (byte) centro[1], (byte) (centro[1] >> 8) });
                    } else {
                        out.write(cabecalho("3DSF", jpeg.length, 0));
                    }
                    out.write(jpeg);
                    out.flush();
                    break;
                }
                case "T": case "R": {
                    if (p.length < 3) break;
                    try {
                        int u = Integer.parseInt(p[1]), v = Integer.parseInt(p[2]);
                        int n = p.length > 3 ? Integer.parseInt(p[3]) : 0;
                        tocar(p[0].charAt(0), u, v, n);
                    } catch (NumberFormatException ignorado) {}
                    break;
                }
                case "K":
                    if (p.length > 1) Toque.digitar(new String(hex(p[1]), StandardCharsets.UTF_8));
                    break;
                case "E":
                    if (p.length > 1) Toque.tecla(p[1]);
                    break;
                default:
                    break;   // "M": no celular não tem seta de mouse
            }
        }
    }

    private static byte[] cabecalho(String magic, int tamanho, int extra) {
        ByteBuffer b = ByteBuffer.allocate(8).order(ByteOrder.LITTLE_ENDIAN);
        b.put(magic.getBytes(StandardCharsets.US_ASCII)).putInt(tamanho);
        return b.array();
    }

    private static byte[] hex(String s) {
        byte[] b = new byte[s.length() / 2];
        for (int i = 0; i < b.length; i++) b[i] = (byte) Integer.parseInt(s.substring(2 * i, 2 * i + 2), 16);
        return b;
    }

    /** Igual ao recortar/regiao_inteira do PC: com zoom, o recorte tem o formato do 3DS (5:3). */
    private byte[] quadro(int zoomX100, int[] centro) {
        CapturaTela cap = hub.captura();
        Bitmap img = cap == null ? null : cap.quadro();
        if (img == null) {
            temRegiao = false;
            return new byte[0];
        }
        float W = img.getWidth(), H = img.getHeight();
        float z = Math.max(1f, Math.min(8f, zoomX100 / 100f));
        Canvas c = new Canvas(saida);
        c.drawColor(Color.BLACK);
        if (z > 1f) {
            float quadroW = Math.max(W, H * LARGURA / ALTURA), quadroH = quadroW * ALTURA / LARGURA;
            float w = quadroW / z, h = quadroH / z;
            float px = ajustar(centro[0], w, W), py = ajustar(centro[1], h, H);
            rx = px - w / 2; ry = py - h / 2; rw = w; rh = h;
            centro[0] = Math.round(px / W * 10000);
            centro[1] = Math.round(py / H * 10000);
        } else {
            float esc = Math.min(LARGURA / W, ALTURA / H);
            rw = LARGURA / esc; rh = ALTURA / esc;
            rx = (W - rw) / 2; ry = (H - rh) / 2;
            centro[0] = centro[1] = 5000;
        }
        temRegiao = true;
        Rect origem = new Rect(Math.round(rx), Math.round(ry), Math.round(rx + rw), Math.round(ry + rh));
        c.drawBitmap(img, origem, new RectF(0, 0, LARGURA, ALTURA), pintura);
        ByteArrayOutputStream jpeg = new ByteArrayOutputStream(40000);
        saida.compress(Bitmap.CompressFormat.JPEG, QUALIDADE, jpeg);
        return jpeg.toByteArray();
    }

    private static float ajustar(int c, float r, float total) {
        if (r >= total) return total / 2;
        return Math.min(Math.max(Math.max(0, Math.min(10000, c)) / 10000f * total, r / 2), total - r / 2);
    }

    private void tocar(char tipo, int u, int v, int n) {
        CapturaTela cap = hub.captura();
        if (cap == null || !temRegiao) return;
        float escala = (float) cap.larguraReal / cap.largura;
        float x, y;
        if (tipo == 'R' && u == 0 && v == 0) {          // rodinha sem ponto: meio da tela
            x = cap.larguraReal / 2f;
            y = cap.alturaReal / 2f;
        } else {
            float ix = rx + u / 10000f * rw, iy = ry + v / 10000f * rh;
            if (ix < 0 || iy < 0 || ix >= cap.largura || iy >= cap.altura) return;   // faixa preta
            x = ix * escala;
            y = iy * escala;
        }
        if (tipo == 'T') Toque.tocar(x, y, n == 2);
        else Toque.rolar(x, y, n, cap.alturaReal);
    }

    void parar() {
        interrupt();
        try { if (servidor != null) servidor.close(); } catch (Exception ignorado) {}
    }
}
