package io.github.chunob.hub3ds;

import android.app.Activity;
import android.app.NotificationManager;
import android.content.Context;
import android.graphics.Canvas;
import android.graphics.DashPathEffect;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.os.Bundle;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;

import java.util.ArrayList;
import java.util.List;

import static io.github.chunob.hub3ds.Pescaria.*;

/** O lago da Pescaria na tela do celular (deitado), desenhado como no PC. */
public class PescariaActivity extends Activity {
    static volatile boolean aberta;

    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        setContentView(new Lago(this));
        WindowInsetsController c = getWindow().getInsetsController();
        if (c != null) {
            c.hide(WindowInsets.Type.systemBars());
            c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
        }
    }

    @Override protected void onResume() {
        super.onResume();
        aberta = true;
        getSystemService(NotificationManager.class).cancel(3);
    }

    @Override protected void onPause() {
        super.onPause();
        aberta = false;
    }

    static final class Lago extends View {
        private final Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Path caminho = new Path();
        private final RectF r = new RectF();
        private final long inicio = System.nanoTime();
        private float s, ox, oy;
        private static final int[] FAIXAS = { 0xFF2E86C1, 0xFF2874A6, 0xFF21618C, 0xFF1B4F72, 0xFF154360, 0xFF11364F };

        Lago(Context c) { super(c); }

        private float X(float x) { return x * s + ox; }
        private float Y(float y) { return y * s + oy; }

        private void ret(Canvas c, float x0, float y0, float x1, float y1, int cor) {
            p.setStyle(Paint.Style.FILL);
            p.setColor(cor);
            c.drawRect(X(x0), Y(y0), X(x1), Y(y1), p);
        }

        private void oval(Canvas c, float x0, float y0, float x1, float y1, int cor) {
            p.setStyle(Paint.Style.FILL);
            p.setColor(cor);
            r.set(X(x0), Y(y0), X(x1), Y(y1));
            c.drawOval(r, p);
        }

        private void linha(Canvas c, float x0, float y0, float x1, float y1, int cor, float largura) {
            p.setStyle(Paint.Style.STROKE);
            p.setColor(cor);
            p.setStrokeWidth(Math.max(1, largura * s));
            c.drawLine(X(x0), Y(y0), X(x1), Y(y1), p);
        }

        private void poligono(Canvas c, int cor, float... pts) {
            caminho.reset();
            caminho.moveTo(X(pts[0]), Y(pts[1]));
            for (int i = 2; i < pts.length; i += 2) caminho.lineTo(X(pts[i]), Y(pts[i + 1]));
            caminho.close();
            p.setStyle(Paint.Style.FILL);
            p.setColor(cor);
            c.drawPath(caminho, p);
        }

        private void texto(Canvas c, String t, float x, float y, float tam, int cor, Paint.Align al) {
            p.setStyle(Paint.Style.FILL);
            p.setColor(cor);
            p.setTextSize(tam * s);
            p.setTypeface(Typeface.DEFAULT_BOLD);
            p.setTextAlign(al);
            c.drawText(t, X(x), Y(y) + tam * s * 0.35f, p);
        }

        @Override protected void onDraw(Canvas c) {
            float t = (System.nanoTime() - inicio) / 1e9f;
            s = Math.min(getWidth() / W, getHeight() / H);
            ox = (getWidth() - W * s) / 2;
            oy = (getHeight() - H * s) / 2;
            c.drawColor(0xFF000000);

            ret(c, 0, 0, W, AGUA_Y0, 0xFF9ED4F0);                       // céu
            oval(c, 1080, 18, 1150, 88, 0xFFFFE066);                    // sol
            float faixa = (AGUA_Y1 - AGUA_Y0) / 6;
            for (int i = 0; i < 6; i++) ret(c, 0, AGUA_Y0 + i * faixa, W, AGUA_Y0 + (i + 1) * faixa + 1, FAIXAS[i]);
            for (int i = 0; i < 14; i++) {                              // ondinhas
                float x = (i * 97 + t * 30) % W, y = AGUA_Y0 + 20 + (i * 61) % (AGUA_Y1 - AGUA_Y0 - 40);
                linha(c, x, y, x + 24, y, 0xFF5DADE2, 2);
            }
            for (int i = 0; i < 9; i++) {                               // plantas
                float x = 70 + i * 140, bal = (float) Math.sin(t * 1.5 + i) * 10;
                caminho.reset();
                caminho.moveTo(X(x), Y(AGUA_Y1));
                caminho.quadTo(X(x + bal), Y(AGUA_Y1 - 60), X(x - bal), Y(AGUA_Y1 - 110));
                p.setStyle(Paint.Style.STROKE);
                p.setColor(0xFF1E8449);
                p.setStrokeWidth(Math.max(2, 6 * s));
                c.drawPath(caminho, p);
            }
            ret(c, 0, AGUA_Y1, W, H, 0xFF8B5A2B);                       // píer
            for (int x = 0; x < W; x += 80) linha(c, x, AGUA_Y1, x, H, 0xFF6E4520, 3);

            Pescaria jogo = Pescaria.atual;
            List<Pescaria.Jogador> placar = new ArrayList<>();
            if (jogo != null) {
                synchronized (jogo) {
                    for (Pescaria.Peixe f : jogo.peixes) peixe(c, f, t);
                    for (Pescaria.Jogador j : jogo.jogadores.values()) { jogador(c, j, t); placar.add(j); }
                }
            }
            placar.sort((a, b) -> b.pontos - a.pontos);
            if (placar.isEmpty())
                texto(c, "Abra a Pescaria no 3DS para entrar", W / 2, H / 2, 28, 0xFFFFFFFF, Paint.Align.CENTER);
            for (int i = 0; i < placar.size(); i++) {
                Pescaria.Jogador j = placar.get(i);
                float y = 16 + i * 30;
                ret(c, 16, y, 260, y + 26, 0xFF0B1E2D);
                p.setStyle(Paint.Style.STROKE);
                p.setStrokeWidth(2 * s);
                p.setColor(CORES[j.numero]);
                c.drawRect(X(16), Y(y), X(260), Y(y + 26), p);
                texto(c, NOMES_CORES[j.numero] + ": " + j.pontos + " pontos", 28, y + 13, 14, 0xFFFFFFFF, Paint.Align.LEFT);
            }
            postInvalidateOnAnimation();
        }

        private void peixe(Canvas c, Pescaria.Peixe f, float t) {
            float comp = f.comp();
            int d = Math.cos(f.ang) >= 0 ? 1 : -1;
            float x = f.x, y = f.y + (float) Math.sin(t * 3 + f.x) * 2;
            float rabo = (float) Math.sin(t * 10 + f.y) * 4;
            if (f.especie.nome.equals("Bota velha")) {
                ret(c, x - comp / 2, y - comp / 3, x, y + comp / 3, f.especie.corpo);
                ret(c, x - comp / 2, y + comp / 6, x + comp / 2, y + comp / 3, f.especie.corpo);
                return;
            }
            poligono(c, f.especie.barbatana, x - d * comp * 0.45f, y, x - d * comp * 0.8f, y - comp * 0.28f + rabo,
                    x - d * comp * 0.8f, y + comp * 0.28f + rabo);
            oval(c, x - comp / 2, y - comp * 0.25f, x + comp / 2, y + comp * 0.25f, f.especie.corpo);
            poligono(c, f.especie.barbatana, x - d * comp * 0.1f, y - comp * 0.2f, x + d * comp * 0.1f, y - comp * 0.38f,
                    x + d * comp * 0.2f, y - comp * 0.2f);
            oval(c, x + d * comp * 0.28f - 3, y - 6, x + d * comp * 0.28f + 3, y, 0xFFFFFFFF);
            oval(c, x + d * comp * 0.29f - 1.5f, y - 4.5f, x + d * comp * 0.29f + 1.5f, y - 1.5f, 0xFF000000);
        }

        private void jogador(Canvas c, Pescaria.Jogador j, float t) {
            int cor = CORES[j.numero];
            float x = j.x, y = AGUA_Y1 + 10;
            oval(c, x - 14, y - 4, x + 14, y + 24, cor);                 // cabeça
            ret(c, x - 18, y + 22, x + 18, y + 80, cor);                 // corpo
            p.setStyle(Paint.Style.FILL);                                // chapéu
            p.setColor(0xFFF2D16B);
            r.set(X(x - 20), Y(y - 14), X(x + 20), Y(y + 14));
            c.drawArc(r, 180, 180, true, p);
            float px = x + 40, py = y - 50;
            linha(c, x + 10, y + 40, px, py, 0xFF5C3A1E, 4);             // vara
            float agora = Pescaria.relogio();
            if (j.estado == MIRANDO) {
                float mx = Math.max(60, Math.min(W - 60, j.x + j.mira * 4.5f));
                p.setPathEffect(new DashPathEffect(new float[] { 6 * s, 6 * s }, 0));
                linha(c, mx, AGUA_Y0 + 40, mx, AGUA_Y1 - 20, cor, 2);
                p.setPathEffect(null);
                poligono(c, cor, mx - 8, AGUA_Y1 - 30, mx + 8, AGUA_Y1 - 30, mx, AGUA_Y1 - 18);
            } else {
                float bx = j.bx, by = j.by;
                boolean mordisca = j.peixe != null && j.estado == NA_AGUA && agora < j.peixe.mordiscaAte;
                if (mordisca || j.estado == FISGOU || j.estado == PUXANDO)
                    by += Math.sin(t * 25) * 4 + (j.estado != NA_AGUA ? 6 : 0);
                linha(c, px, py, bx, by, 0xFFEEEEEE, 1);
                oval(c, bx - 7, by - 7, bx + 7, by + 7, 0xFFFFFFFF);
                p.setStyle(Paint.Style.FILL);
                p.setColor(cor);
                r.set(X(bx - 7), Y(by - 7), X(bx + 7), Y(by + 7));
                c.drawArc(r, 180, 180, true, p);
                if (j.estado == FISGOU) texto(c, "!", bx, by - 26, 28, 0xFFFFE066, Paint.Align.CENTER);
            }
            if (agora < j.mensagemAte && !j.mensagem.isEmpty())
                texto(c, j.mensagem, x, AGUA_Y1 - 14, 13, 0xFFFFFFFF, Paint.Align.CENTER);
        }
    }
}
