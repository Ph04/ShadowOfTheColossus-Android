package org.ph04.sotcandroid;

import android.app.Activity;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.res.AssetFileDescriptor;
import android.graphics.Color;
import android.net.Uri;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;
import android.view.Choreographer;
import android.view.Gravity;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;

public final class MainActivity extends Activity
        implements SurfaceHolder.Callback, Choreographer.FrameCallback {
    private static final int REQUEST_OPEN_ISO = 41;
    private static final String KEY_DISC_URI = "disc_uri";

    private TextView statusView;
    private boolean surfaceReady;
    private Uri selectedDisc;

    static {
        System.loadLibrary("sotc_android");
    }

    private static native boolean nativeAttachIso(int fd, long offset, long length);
    private static native void nativeClearIso();
    private static native boolean nativeSetSurface(Surface surface);
    private static native boolean nativeResizeSurface(int width, int height);
    private static native boolean nativeRenderFrame();
    private static native void nativeClearSurface();
    private static native String nativeStatus();

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
        buildInterface();

        String saved = getPreferences(MODE_PRIVATE).getString(KEY_DISC_URI, null);
        if (saved != null) {
            try {
                selectedDisc = Uri.parse(saved);
                attachSelectedDisc(selectedDisc);
            } catch (RuntimeException error) {
                updateStatus("Não foi possível reabrir a imagem salva: " + error.getMessage());
            }
        }
        refreshNativeStatus();
    }

    private void buildInterface() {
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(Color.rgb(8, 10, 15));
        root.setPadding(18, 14, 18, 14);

        TextView title = new TextView(this);
        title.setText("SHADOW OF THE COLOSSUS");
        title.setTextColor(Color.WHITE);
        title.setTextSize(18);
        title.setGravity(Gravity.CENTER_VERTICAL);
        root.addView(title, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));

        statusView = new TextView(this);
        statusView.setTextColor(Color.rgb(195, 205, 220));
        statusView.setTextSize(12);
        statusView.setText("Preparando runtime Android ARM64...");
        statusView.setPadding(0, 8, 0, 8);
        root.addView(statusView, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));

        Button chooseDisc = new Button(this);
        chooseDisc.setText("Selecionar imagem do jogo");
        chooseDisc.setOnClickListener(view -> openDiscPicker());
        root.addView(chooseDisc, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));

        SurfaceView gameSurface = new SurfaceView(this);
        gameSurface.setBackgroundColor(Color.BLACK);
        gameSurface.getHolder().addCallback(this);
        LinearLayout.LayoutParams surfaceParams = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 0, 1.0f);
        surfaceParams.topMargin = 10;
        root.addView(gameSurface, surfaceParams);
        setContentView(root);
    }

    private void openDiscPicker() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION |
                Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        startActivityForResult(intent, REQUEST_OPEN_ISO);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != REQUEST_OPEN_ISO || resultCode != RESULT_OK ||
                data == null || data.getData() == null) {
            return;
        }

        Uri uri = data.getData();
        int flags = data.getFlags() & Intent.FLAG_GRANT_READ_URI_PERMISSION;
        try {
            getContentResolver().takePersistableUriPermission(uri, flags);
        } catch (SecurityException ignored) {
            // Some document providers grant a temporary read-only descriptor instead.
        }

        selectedDisc = uri;
        getPreferences(MODE_PRIVATE).edit().putString(KEY_DISC_URI, uri.toString()).apply();
        attachSelectedDisc(uri);
    }

    private void attachSelectedDisc(Uri uri) {
        try (AssetFileDescriptor descriptor =
                     getContentResolver().openAssetFileDescriptor(uri, "r")) {
            if (descriptor == null) {
                updateStatus("O provedor de arquivos não abriu a imagem selecionada.");
                return;
            }

            ParcelFileDescriptor parcel = descriptor.getParcelFileDescriptor();
            long offset = descriptor.getStartOffset();
            long length = descriptor.getLength();
            int fd = parcel.detachFd();

            // O descritor passa a pertencer ao C++; a função também o fecha em caso de erro.
            boolean accepted = nativeAttachIso(fd, offset, length);
            updateStatus(nativeStatus());
            if (!accepted) {
                return;
            }
        } catch (Exception error) {
            updateStatus("Falha ao abrir a imagem: " + error.getMessage());
        }
    }

    private void updateStatus(String text) {
        if (statusView != null) {
            statusView.setText(text);
        }
    }

    private void refreshNativeStatus() {
        if (statusView != null) {
            statusView.setText(nativeStatus());
        }
    }

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        surfaceReady = nativeSetSurface(holder.getSurface());
        refreshNativeStatus();
        if (surfaceReady) {
            Choreographer.getInstance().postFrameCallback(this);
        }
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        surfaceReady = nativeResizeSurface(width, height);
        refreshNativeStatus();
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        surfaceReady = false;
        Choreographer.getInstance().removeFrameCallback(this);
        nativeClearSurface();
    }

    @Override
    public void doFrame(long frameTimeNanos) {
        if (!surfaceReady) {
            return;
        }
        if (!nativeRenderFrame()) {
            refreshNativeStatus();
            surfaceReady = false;
            return;
        }
        Choreographer.getInstance().postFrameCallback(this);
    }

    @Override
    protected void onPause() {
        Choreographer.getInstance().removeFrameCallback(this);
        super.onPause();
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (surfaceReady) {
            Choreographer.getInstance().postFrameCallback(this);
        }
    }

    @Override
    protected void onDestroy() {
        surfaceReady = false;
        Choreographer.getInstance().removeFrameCallback(this);
        nativeClearSurface();
        nativeClearIso();
        super.onDestroy();
    }
}
