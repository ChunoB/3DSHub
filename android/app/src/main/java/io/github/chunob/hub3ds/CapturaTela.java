package io.github.chunob.hub3ds;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.PixelFormat;
import android.graphics.Point;
import android.hardware.display.DisplayManager;
import android.hardware.display.VirtualDisplay;
import android.media.Image;
import android.media.ImageReader;
import android.media.projection.MediaProjection;
import android.os.Handler;
import android.os.HandlerThread;
import android.util.DisplayMetrics;
import android.view.Display;

import java.nio.ByteBuffer;

/**
 * A imagem da tela do celular, para o Espelho. Usa a mesma permissão do som (a
 * "gravação da tela" do Android). A captura fica em resolução reduzida (lado maior
 * até 1280) e só desenha enquanto o Espelho está aberto no 3DS.
 *
 * No Android 14, cada permissão só pode criar uma tela virtual: ela é criada uma vez
 * e, sem uso, fica sem destino (não gasta nada) em vez de ser fechada.
 */
final class CapturaTela {
    private static final int LADO_MAX = 1280;

    private final Context ctx;
    private final MediaProjection projecao;
    private final HandlerThread thread = new HandlerThread("captura");
    private final Handler handler;
    private VirtualDisplay tela;
    private ImageReader leitor;
    private Bitmap ultimo;
    int largura, altura;          // da captura
    int larguraReal, alturaReal;  // da tela de verdade (para os toques)

    CapturaTela(Context ctx, MediaProjection projecao) {
        this.ctx = ctx.getApplicationContext();
        this.projecao = projecao;
        thread.start();
        handler = new Handler(thread.getLooper());
    }

    private Point tamanhoReal() {
        Display d = ((DisplayManager) ctx.getSystemService(Context.DISPLAY_SERVICE)).getDisplay(Display.DEFAULT_DISPLAY);
        Point p = new Point();
        d.getRealSize(p);
        return p;
    }

    /** Liga (ou ajusta, se o celular girou) a captura. */
    private synchronized void preparar() {
        Point real = tamanhoReal();
        if (leitor != null && real.x == larguraReal && real.y == alturaReal) {
            if (tela != null) tela.setSurface(leitor.getSurface());
            return;
        }
        larguraReal = real.x;
        alturaReal = real.y;
        float esc = Math.min(1f, (float) LADO_MAX / Math.max(real.x, real.y));
        largura = Math.max(2, Math.round(real.x * esc) & ~1);
        altura = Math.max(2, Math.round(real.y * esc) & ~1);
        ImageReader antigo = leitor;
        leitor = ImageReader.newInstance(largura, altura, PixelFormat.RGBA_8888, 2);
        int dpi = ctx.getResources().getDisplayMetrics().densityDpi;
        if (tela == null) {
            tela = projecao.createVirtualDisplay("3dshub-espelho", largura, altura, dpi,
                    DisplayManager.VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR, leitor.getSurface(), null, handler);
        } else {
            tela.resize(largura, altura, dpi);
            tela.setSurface(leitor.getSurface());
        }
        if (antigo != null) antigo.close();
        ultimo = null;
    }

    /** O quadro mais recente (ou o último que chegou, se a tela não mudou). */
    synchronized Bitmap quadro() {
        preparar();
        Image img = null;
        try {
            img = leitor.acquireLatestImage();
            if (img == null) return ultimo;
            Image.Plane pl = img.getPlanes()[0];
            ByteBuffer buf = pl.getBuffer();
            int px = pl.getPixelStride(), linha = pl.getRowStride();
            Bitmap b = Bitmap.createBitmap(linha / px, altura, Bitmap.Config.ARGB_8888);
            b.copyPixelsFromBuffer(buf);
            if (b.getWidth() != largura) {
                Bitmap c = Bitmap.createBitmap(b, 0, 0, largura, altura);
                b.recycle();
                b = c;
            }
            if (ultimo != null) ultimo.recycle();
            ultimo = b;
            return b;
        } catch (Exception e) {
            android.util.Log.w("3dshub", "Captura", e);
            return ultimo;
        } finally {
            if (img != null) img.close();
        }
    }

    /** Para de desenhar (o Espelho saiu). */
    synchronized void pausar() {
        if (tela != null) tela.setSurface(null);
    }

    synchronized void encerrar() {
        if (tela != null) tela.release();
        tela = null;
        if (leitor != null) leitor.close();
        leitor = null;
        thread.quitSafely();
    }
}
