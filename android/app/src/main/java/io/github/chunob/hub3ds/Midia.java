package io.github.chunob.hub3ds;

import android.content.ComponentName;
import android.content.Context;
import android.media.AudioManager;
import android.media.MediaMetadata;
import android.media.session.MediaController;
import android.media.session.MediaSessionManager;
import android.media.session.PlaybackState;
import android.os.SystemClock;
import android.provider.Settings;
import android.view.KeyEvent;

import java.util.List;

/**
 * A música que está tocando no celular (Spotify, YouTube Music, Deezer...) e os
 * botões dela. Ler o nome da música precisa do "acesso às notificações"; sem ele,
 * os botões ainda funcionam como as teclas de mídia de um fone.
 */
final class Midia {
    private final Context ctx;
    private final AudioManager audio;

    Midia(Context ctx) {
        this.ctx = ctx.getApplicationContext();
        this.audio = (AudioManager) ctx.getSystemService(Context.AUDIO_SERVICE);
    }

    static boolean temAcesso(Context ctx) {
        String lista = Settings.Secure.getString(ctx.getContentResolver(), "enabled_notification_listeners");
        return lista != null && lista.contains(ctx.getPackageName());
    }

    /** O player mais importante agora: o que está tocando, senão o primeiro da lista. */
    MediaController atual() {
        if (!temAcesso(ctx)) return null;
        try {
            MediaSessionManager m = (MediaSessionManager) ctx.getSystemService(Context.MEDIA_SESSION_SERVICE);
            List<MediaController> lista = m.getActiveSessions(new ComponentName(ctx, OuvinteNotificacoes.class));
            for (MediaController c : lista) {
                PlaybackState s = c.getPlaybackState();
                if (s != null && s.getState() == PlaybackState.STATE_PLAYING) return c;
            }
            return lista.isEmpty() ? null : lista.get(0);
        } catch (SecurityException e) {
            return null;
        }
    }

    String[] tocando() {   // título, artista, "1" se tocando
        MediaController c = atual();
        if (c == null) return new String[] { "", "", audio.isMusicActive() ? "1" : "0" };
        MediaMetadata md = c.getMetadata();
        String titulo = md == null ? "" : texto(md.getString(MediaMetadata.METADATA_KEY_TITLE));
        String artista = md == null ? "" : texto(md.getString(MediaMetadata.METADATA_KEY_ARTIST));
        PlaybackState s = c.getPlaybackState();
        boolean toca = s != null && s.getState() == PlaybackState.STATE_PLAYING;
        return new String[] { titulo, artista, toca ? "1" : "0" };
    }

    private static String texto(String s) { return s == null ? "" : s; }

    void playPause() {
        MediaController c = atual();
        if (c != null) {
            PlaybackState s = c.getPlaybackState();
            if (s != null && s.getState() == PlaybackState.STATE_PLAYING) c.getTransportControls().pause();
            else c.getTransportControls().play();
        } else {
            tecla(KeyEvent.KEYCODE_MEDIA_PLAY_PAUSE);
        }
    }

    void proxima() {
        MediaController c = atual();
        if (c != null) c.getTransportControls().skipToNext();
        else tecla(KeyEvent.KEYCODE_MEDIA_NEXT);
    }

    void anterior() {
        MediaController c = atual();
        if (c != null) c.getTransportControls().skipToPrevious();
        else tecla(KeyEvent.KEYCODE_MEDIA_PREVIOUS);
    }

    private void tecla(int codigo) {
        long t = SystemClock.uptimeMillis();
        audio.dispatchMediaKeyEvent(new KeyEvent(t, t, KeyEvent.ACTION_DOWN, codigo, 0));
        audio.dispatchMediaKeyEvent(new KeyEvent(t, t, KeyEvent.ACTION_UP, codigo, 0));
    }

    // ------------------------------------------------------------ volume
    int volume() {
        int max = audio.getStreamMaxVolume(AudioManager.STREAM_MUSIC);
        return max <= 0 ? -1 : Math.round(audio.getStreamVolume(AudioManager.STREAM_MUSIC) * 100f / max);
    }

    boolean mudo() { return audio.isStreamMute(AudioManager.STREAM_MUSIC); }

    void volumeMais() { audio.adjustStreamVolume(AudioManager.STREAM_MUSIC, AudioManager.ADJUST_RAISE, 0); }
    void volumeMenos() { audio.adjustStreamVolume(AudioManager.STREAM_MUSIC, AudioManager.ADJUST_LOWER, 0); }
    void alternarMudo() { audio.adjustStreamVolume(AudioManager.STREAM_MUSIC, AudioManager.ADJUST_TOGGLE_MUTE, 0); }
}
