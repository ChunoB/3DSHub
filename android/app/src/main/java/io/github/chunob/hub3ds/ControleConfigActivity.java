package io.github.chunob.hub3ds;

import android.app.Activity;
import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.os.Bundle;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.HorizontalScrollView;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.util.Map;

/**
 * Marcar onde cada botão do 3DS toca. Abre transparente por cima do jogo (pela
 * notificação "Botões do controle"): escolha um botão na faixa de cima e toque no
 * lugar da tela. Nos analógicos, o segundo toque perto do centro ajusta o tamanho.
 */
public class ControleConfigActivity extends Activity {
    private String app;
    private Map<Integer, float[]> mapa;
    private int escolhido = 0;
    private final Button[] chips = new Button[MapaControle.NOMES.length];
    private Marcas marcas;
    private TextView titulo;

    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        app = Toque.ultimoApp;
        mapa = MapaControle.ler(this, app);
        float d = getResources().getDisplayMetrics().density;

        FrameLayout raiz = new FrameLayout(this);
        marcas = new Marcas(this);
        raiz.addView(marcas, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));

        LinearLayout topo = new LinearLayout(this);
        topo.setOrientation(LinearLayout.VERTICAL);
        topo.setBackgroundColor(0xE6151823);
        topo.setPadding((int) (12 * d), (int) (10 * d), (int) (12 * d), (int) (8 * d));
        titulo = new TextView(this);
        titulo.setTextColor(0xFFE8EAF2);
        titulo.setTextSize(15);
        titulo.setTypeface(Typeface.DEFAULT_BOLD);
        topo.addView(titulo);

        HorizontalScrollView rol = new HorizontalScrollView(this);
        LinearLayout linha = new LinearLayout(this);
        for (int i = 0; i < chips.length; i++) {
            final int k = i;
            Button c = new Button(this);
            c.setText(MapaControle.NOMES[i]);
            c.setAllCaps(false);
            c.setTextSize(13);
            c.setMinWidth(0);
            c.setMinimumWidth(0);
            c.setPadding((int) (12 * d), 0, (int) (12 * d), 0);
            c.setOnClickListener(v -> { escolhido = k; atualizar(); });
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, (int) (38 * d));
            lp.rightMargin = (int) (6 * d);
            linha.addView(c, lp);
            chips[i] = c;
        }
        rol.addView(linha);
        LinearLayout.LayoutParams lpRol = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        lpRol.topMargin = (int) (6 * d);
        topo.addView(rol, lpRol);

        LinearLayout acoes = new LinearLayout(this);
        acoes.setGravity(Gravity.END);
        Button apagar = acao("Tirar este botão", 0xFF2A3048);
        apagar.setOnClickListener(v -> { mapa.remove(escolhido); salvar(); });
        Button pronto = acao("Pronto", 0xFF1D9E75);
        pronto.setOnClickListener(v -> finish());
        acoes.addView(apagar);
        acoes.addView(pronto);
        LinearLayout.LayoutParams lpA = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        lpA.topMargin = (int) (6 * d);
        topo.addView(acoes, lpA);

        raiz.addView(topo, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.TOP));
        setContentView(raiz);
        atualizar();
    }

    private Button acao(String texto, int cor) {
        Button b = new Button(this);
        b.setText(texto);
        b.setAllCaps(false);
        b.setTextColor(Color.WHITE);
        GradientDrawable g = new GradientDrawable();
        g.setColor(cor);
        g.setCornerRadius(10 * getResources().getDisplayMetrics().density);
        b.setBackground(g);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.leftMargin = (int) (8 * getResources().getDisplayMetrics().density);
        b.setLayoutParams(lp);
        return b;
    }

    private void atualizar() {
        String nome = MapaControle.nomeDoApp(this, app);
        titulo.setText(nome.isEmpty()
                ? "Abra o jogo e toque em \"Botões do controle\" na notificação do 3DS Hub"
                : "Botões para " + nome + ": escolha um e toque no lugar");
        for (int i = 0; i < chips.length; i++) {
            GradientDrawable g = new GradientDrawable();
            g.setCornerRadius(19 * getResources().getDisplayMetrics().density);
            g.setColor(i == escolhido ? 0xFF7F77DD : mapa.containsKey(i) ? 0xFF2A3048 : 0xFF20253A);
            chips[i].setBackground(g);
            chips[i].setTextColor(mapa.containsKey(i) || i == escolhido ? Color.WHITE : 0xFF9AA0B5);
        }
        marcas.invalidate();
    }

    private void salvar() {
        if (!app.isEmpty()) MapaControle.gravar(this, app, mapa);
        atualizar();
    }

    final class Marcas extends View {
        private final Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);

        Marcas(Context c) { super(c); }

        @Override protected void onDraw(Canvas c) {
            c.drawColor(0x33000000);
            float d = getResources().getDisplayMetrics().density;
            for (Map.Entry<Integer, float[]> e : mapa.entrySet()) {
                float x = e.getValue()[0] * getWidth(), y = e.getValue()[1] * getHeight();
                boolean analog = e.getKey() >= MapaControle.ANALOGICO;
                p.setStyle(Paint.Style.FILL);
                p.setColor(e.getKey() == escolhido ? 0xCC7F77DD : 0xAA20253A);
                if (analog) {
                    float r = e.getValue()[2] * getWidth();
                    p.setColor(0x447F77DD);
                    c.drawCircle(x, y, r, p);
                    p.setColor(e.getKey() == escolhido ? 0xCC7F77DD : 0xAA20253A);
                }
                c.drawCircle(x, y, 22 * d, p);
                p.setStyle(Paint.Style.STROKE);
                p.setStrokeWidth(2 * d);
                p.setColor(Color.WHITE);
                c.drawCircle(x, y, 22 * d, p);
                p.setStyle(Paint.Style.FILL);
                p.setTextAlign(Paint.Align.CENTER);
                p.setTextSize(12 * d);
                p.setTypeface(Typeface.DEFAULT_BOLD);
                String n = MapaControle.NOMES[e.getKey()];
                c.drawText(n.length() > 6 ? n.substring(0, 5) + "." : n, x, y + 4 * d, p);
            }
        }

        @Override public boolean onTouchEvent(MotionEvent ev) {
            if (ev.getAction() != MotionEvent.ACTION_UP || app.isEmpty()) return true;
            float fx = ev.getX() / getWidth(), fy = ev.getY() / getHeight();
            float[] atual = mapa.get(escolhido);
            if (escolhido >= MapaControle.ANALOGICO && atual != null) {
                // analógico já marcado: toque perto ajusta o tamanho, longe muda o centro
                float dx = (fx - atual[0]) * getWidth(), dy = (fy - atual[1]) * getHeight();
                float dist = (float) Math.hypot(dx, dy) / getWidth();
                if (dist < 0.3f) { atual[2] = Math.max(0.03f, dist); salvar(); return true; }
            }
            mapa.put(escolhido, new float[] { fx, fy, atual != null ? atual[2] : MapaControle.RAIO_PADRAO });
            salvar();
            return true;
        }
    }
}
