package io.github.chunob.hub3ds;

import android.accessibilityservice.AccessibilityService;
import android.accessibilityservice.GestureDescription;
import android.content.Context;
import android.graphics.Path;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.provider.Settings;
import android.view.accessibility.AccessibilityEvent;
import android.view.accessibility.AccessibilityNodeInfo;

import java.util.HashMap;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;

/**
 * Serviço de acessibilidade: é o jeito que o Android dá para um app tocar na tela e
 * digitar nos outros apps. Usado pelo Espelho (toque no 3DS = toque no celular) e
 * pelo teclado do 3DS. Só age quando o 3DS manda.
 */
public class Toque extends AccessibilityService {
    private static volatile Toque ativo;

    @Override protected void onServiceConnected() { ativo = this; }
    @Override public boolean onUnbind(android.content.Intent i) { ativo = null; return super.onUnbind(i); }
    /** O último app aberto que não é o 3DS Hub nem a barra do sistema (para o mapa do Controle). */
    static volatile String ultimoApp = "";

    @Override public void onAccessibilityEvent(AccessibilityEvent e) {
        if (e.getEventType() != AccessibilityEvent.TYPE_WINDOW_STATE_CHANGED || e.getPackageName() == null) return;
        String p = e.getPackageName().toString();
        if (!p.equals(getPackageName()) && !p.equals("com.android.systemui") && !p.contains("launcher")) ultimoApp = p;
    }
    @Override public void onInterrupt() {}

    static boolean ligado() { return ativo != null; }

    static boolean liberado(Context ctx) {
        String lista = Settings.Secure.getString(ctx.getContentResolver(), Settings.Secure.ENABLED_ACCESSIBILITY_SERVICES);
        return lista != null && lista.contains(ctx.getPackageName() + "/");
    }

    // ------------------------------------------------------------ toques
    private static float dentro(float v) { return Math.max(0, v); }

    static void tocar(float x, float y, boolean longo) {
        Toque t = ativo;
        if (t == null) return;
        x = dentro(x);
        y = dentro(y);
        Path p = new Path();
        p.moveTo(x, y);
        try {
            t.dispatchGesture(new GestureDescription.Builder()
                    .addStroke(new GestureDescription.StrokeDescription(p, 0, longo ? 650 : 40)).build(), null, null);
        } catch (RuntimeException e) {
            android.util.Log.w("3dshub", "Toque", e);
        }
    }

    /** Rolar: passos positivos sobem o conteúdo (o dedo desce), como a rodinha do mouse. */
    static void rolar(float x, float y, int passos, int alturaTela) {
        Toque t = ativo;
        if (t == null || passos == 0) return;
        float dist = Math.min(alturaTela * 0.35f, Math.abs(passos) * alturaTela * 0.12f);
        float y0 = passos > 0 ? y - dist / 2 : y + dist / 2;
        float y1 = passos > 0 ? y + dist / 2 : y - dist / 2;
        y0 = Math.max(1, Math.min(alturaTela - 2, y0));
        y1 = Math.max(1, Math.min(alturaTela - 2, y1));
        Path p = new Path();
        p.moveTo(dentro(x), y0);
        p.lineTo(dentro(x), y1);
        try {
            t.dispatchGesture(new GestureDescription.Builder()
                    .addStroke(new GestureDescription.StrokeDescription(p, 0, 220)).build(), null, null);
        } catch (RuntimeException e) {
            android.util.Log.w("3dshub", "Rolar", e);
        }
    }

    // ------------------------------------------------------------ dedos contínuos (Controle)
    // Cada botão segurado no 3DS é um dedo parado na tela; o analógico é um dedo que
    // arrasta. O Android só aceita um gesto por vez, então os dedos andam em pedaços de
    // ~32 ms que continuam uns aos outros (continueStroke): cada pedaço novo sai quando
    // o anterior termina, com a posição de agora de cada dedo.
    private static final int PEDACO_MS = 32;
    private static final Handler principal = new Handler(Looper.getMainLooper());
    private static Map<Integer, float[]> alvo = new HashMap<>();
    private static final Map<Integer, GestureDescription.StrokeDescription> tracos = new HashMap<>();
    private static final Map<Integer, float[]> posicoes = new HashMap<>();
    private static boolean despachando;

    /** Os dedos que devem estar na tela agora: id -> {x, y} em pixels. */
    static synchronized void dedos(Map<Integer, float[]> novos) {
        alvo = new HashMap<>();
        for (Map.Entry<Integer, float[]> e : novos.entrySet())      // o Android recusa ponto negativo
            alvo.put(e.getKey(), new float[] { dentro(e.getValue()[0]), dentro(e.getValue()[1]) });
        if (!despachando) proximo();
    }

    private static synchronized void proximo() {
        try {
            proximoSemProteger();
        } catch (RuntimeException e) {     // gesto recusado: recomeça os dedos do zero
            android.util.Log.w("3dshub", "Dedos", e);
            tracos.clear();
            posicoes.clear();
            despachando = false;
        }
    }

    private static void proximoSemProteger() {
        Toque t = ativo;
        if (t == null) {
            tracos.clear();
            posicoes.clear();
            despachando = false;
            return;
        }
        GestureDescription.Builder g = new GestureDescription.Builder();
        Set<Integer> ids = new HashSet<>(alvo.keySet());
        ids.addAll(tracos.keySet());
        int n = 0, max = GestureDescription.getMaxStrokeCount();
        for (int id : ids) {
            if (n >= max) break;
            float[] para = alvo.get(id), de = posicoes.get(id);
            GestureDescription.StrokeDescription antes = tracos.get(id), s;
            Path p = new Path();
            if (para != null) {
                if (antes == null) {
                    p.moveTo(para[0], para[1]);
                    s = new GestureDescription.StrokeDescription(p, 0, PEDACO_MS, true);
                } else {
                    p.moveTo(de[0], de[1]);
                    p.lineTo(para[0], para[1]);
                    s = antes.continueStroke(p, 0, PEDACO_MS, true);
                }
                tracos.put(id, s);
                posicoes.put(id, para);
            } else {                               // soltou: termina o dedo onde ele está
                p.moveTo(de[0], de[1]);
                s = antes.continueStroke(p, 0, 1, false);
                tracos.remove(id);
                posicoes.remove(id);
            }
            g.addStroke(s);
            n++;
        }
        if (n == 0) {
            despachando = false;
            return;
        }
        despachando = true;
        boolean ok = t.dispatchGesture(g.build(), new GestureResultCallback() {
            @Override public void onCompleted(GestureDescription d) { proximo(); }
            @Override public void onCancelled(GestureDescription d) {
                synchronized (Toque.class) { tracos.clear(); posicoes.clear(); }
                proximo();
            }
        }, principal);
        if (!ok) {
            tracos.clear();
            posicoes.clear();
            despachando = false;
        }
    }

    // ------------------------------------------------------------ teclado
    private static AccessibilityNodeInfo campo() {
        Toque t = ativo;
        if (t == null) return null;
        AccessibilityNodeInfo raiz = t.getRootInActiveWindow();
        if (raiz == null) return null;
        AccessibilityNodeInfo f = raiz.findFocus(AccessibilityNodeInfo.FOCUS_INPUT);
        return f != null && f.isEditable() ? f : null;
    }

    private static String textoAtual(AccessibilityNodeInfo f) {
        if (f.isShowingHintText() || f.getText() == null) return "";
        return f.getText().toString();
    }

    private static void colocar(AccessibilityNodeInfo f, String texto) {
        Bundle b = new Bundle();
        b.putCharSequence(AccessibilityNodeInfo.ACTION_ARGUMENT_SET_TEXT_CHARSEQUENCE, texto);
        f.performAction(AccessibilityNodeInfo.ACTION_SET_TEXT, b);
    }

    /** Escreve no fim do campo de texto que está selecionado no celular. */
    static void digitar(String texto) {
        AccessibilityNodeInfo f = campo();
        if (f != null) colocar(f, textoAtual(f) + texto);
    }

    /** enter, backspace, tab, esc (o PC) e voltar, inicio, recentes (só no celular). */
    static void tecla(String nome) {
        Toque t = ativo;
        if (t == null) return;
        switch (nome) {
            case "voltar": case "esc": t.performGlobalAction(GLOBAL_ACTION_BACK); return;
            case "inicio": t.performGlobalAction(GLOBAL_ACTION_HOME); return;
            case "recentes": t.performGlobalAction(GLOBAL_ACTION_RECENTS); return;
            default: break;
        }
        AccessibilityNodeInfo f = campo();
        if (f == null) return;
        if (nome.equals("backspace")) {
            String s = textoAtual(f);
            if (!s.isEmpty()) colocar(f, s.substring(0, s.offsetByCodePoints(s.length(), -1)));
        } else if (nome.equals("enter")) {
            if (Build.VERSION.SDK_INT >= 30) f.performAction(AccessibilityNodeInfo.AccessibilityAction.ACTION_IME_ENTER.getId());
            else colocar(f, textoAtual(f) + "\n");
        } else if (nome.equals("tab")) {
            AccessibilityNodeInfo prox = f.focusSearch(android.view.View.FOCUS_FORWARD);
            if (prox != null) prox.performAction(AccessibilityNodeInfo.ACTION_FOCUS);
        }
    }
}
