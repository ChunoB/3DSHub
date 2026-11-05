package io.github.chunob.hub3ds;

import android.app.Activity;
import android.content.ContentValues;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.Bundle;
import android.os.Environment;
import android.os.Handler;
import android.os.Looper;
import android.provider.MediaStore;
import android.view.Gravity;
import android.widget.Button;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.Toast;

import java.io.OutputStream;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;

/** A câmera do 3DS ao vivo no celular, com botão para salvar a foto na galeria. */
public class CameraActivity extends Activity {
    static volatile boolean aberta;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private ImageView imagem;
    private TextView estado;
    private long visto = -1;

    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        getWindow().setStatusBarColor(Color.BLACK);
        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        col.setBackgroundColor(0xFF151823);
        col.setGravity(Gravity.CENTER_HORIZONTAL);
        int p = (int) (16 * getResources().getDisplayMetrics().density);
        col.setPadding(p, p, p, p);

        TextView titulo = new TextView(this);
        titulo.setText("Câmera do 3DS");
        titulo.setTextSize(24);
        titulo.setTypeface(Typeface.DEFAULT_BOLD);
        titulo.setTextColor(0xFFE8EAF2);
        col.addView(titulo);

        estado = new TextView(this);
        estado.setTextColor(0xFF9AA0B5);
        estado.setTextSize(14);
        estado.setPadding(0, p / 4, 0, p);
        col.addView(estado);

        imagem = new ImageView(this);
        imagem.setAdjustViewBounds(true);
        imagem.setBackgroundColor(Color.BLACK);
        col.addView(imagem, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f));

        Button salvar = new Button(this);
        salvar.setText("Salvar foto na galeria");
        salvar.setAllCaps(false);
        salvar.setTextSize(16);
        salvar.setTextColor(Color.WHITE);
        GradientDrawable fundo = new GradientDrawable();
        fundo.setColor(0xFFD85A30);
        fundo.setCornerRadius(p * 0.75f);
        salvar.setBackground(fundo);
        salvar.setOnClickListener(v -> salvar());
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.topMargin = p;
        col.addView(salvar, lp);
        setContentView(col);
    }

    @Override protected void onResume() {
        super.onResume();
        aberta = true;
        handler.post(atualizar);
    }

    @Override protected void onPause() {
        super.onPause();
        aberta = false;
        handler.removeCallbacks(atualizar);
    }

    private final Runnable atualizar = new Runnable() {
        @Override public void run() {
            estado.setText(Camera.conectado
                    ? (Camera.olhos > 1 ? "Ao vivo, em 3D (mostrando o olho esquerdo)" : "Ao vivo")
                    : "Abra a Câmera no 3DS Hub do 3DS");
            byte[] j = Camera.esquerdo;
            if (j != null && Camera.numero != visto) {
                visto = Camera.numero;
                Bitmap bmp = BitmapFactory.decodeByteArray(j, 0, j.length);
                if (bmp != null) imagem.setImageBitmap(bmp);
            }
            handler.postDelayed(this, 50);
        }
    };

    private void salvar() {
        byte[][] fotos = Camera.olhos > 1 && Camera.direito != null
                ? new byte[][] { Camera.esquerdo, Camera.direito } : new byte[][] { Camera.esquerdo };
        if (fotos[0] == null) {
            Toast.makeText(this, "Nenhuma foto ainda", Toast.LENGTH_SHORT).show();
            return;
        }
        String base = "3DS_" + new SimpleDateFormat("yyyyMMdd_HHmmss", Locale.ROOT).format(new Date());
        try {
            for (int i = 0; i < fotos.length; i++) {
                ContentValues v = new ContentValues();
                v.put(MediaStore.Images.Media.DISPLAY_NAME, base + (fotos.length > 1 ? (i == 0 ? "_esq" : "_dir") : "") + ".jpg");
                v.put(MediaStore.Images.Media.MIME_TYPE, "image/jpeg");
                v.put(MediaStore.Images.Media.RELATIVE_PATH, Environment.DIRECTORY_PICTURES + "/3DS Hub");
                Uri uri = getContentResolver().insert(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, v);
                if (uri == null) throw new Exception("sem espaço?");
                try (OutputStream o = getContentResolver().openOutputStream(uri)) { o.write(fotos[i]); }
            }
            Toast.makeText(this, "Salva em Imagens > 3DS Hub", Toast.LENGTH_SHORT).show();
        } catch (Exception e) {
            Toast.makeText(this, "Não deu para salvar: " + e.getMessage(), Toast.LENGTH_LONG).show();
        }
    }
}
