package org.ph04.sotcandroid;

import android.app.Activity;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.res.AssetFileDescriptor;
import android.graphics.Color;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.os.StatFs;
import android.view.Choreographer;
import android.view.Gravity;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;
import android.widget.Toast;

import org.json.JSONObject;

import java.io.BufferedInputStream;
import java.io.BufferedOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.OutputStream;
import java.util.Locale;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

public final class MainActivity extends Activity
        implements SurfaceHolder.Callback, Choreographer.FrameCallback {
    private static final int REQUEST_OPEN_ISO = 41;
    private static final int REQUEST_EXPORT_ZIP = 42;
    private static final String KEY_DISC_URI = "disc_uri";

    private TextView statusView;
    private TextView percentView;
    private TextView stepView;
    private TextView detailView;
    private TextView remainingView;
    private TextView exportStatusView;
    private ProgressBar progressBar;
    private Button chooseDiscButton;
    private Button prepareButton;
    private Button exportButton;

    private boolean surfaceReady;
    private Uri selectedDisc;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private boolean pollingProgress;

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

    private static native boolean nativeStartPipeline(String appFilesDirectory);
    private static native String nativePipelineProgress();
    private static native boolean nativePipelineRunning();
    private static native String nativePipelineOutputDirectory();

    private final Runnable progressPoll = new Runnable() {
        @Override public void run() {
            pollPipelineProgress();
        }
    };

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

        if (nativePipelineRunning()) {
            keepScreenOn(true);
            beginProgressPolling();
        } else {
            restorePipelineState();
        }
    }

    private void buildInterface() {
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(Color.rgb(8, 10, 15));
        root.setPadding(16, 8, 16, 8);

        TextView title = new TextView(this);
        title.setText("SHADOW OF THE COLOSSUS");
        title.setTextColor(Color.WHITE);
        title.setTextSize(17);
        title.setGravity(Gravity.CENTER_VERTICAL);
        root.addView(title, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 28));

        statusView = new TextView(this);
        statusView.setTextColor(Color.rgb(195, 205, 220));
        statusView.setTextSize(10);
        statusView.setText("Selecione a imagem PAL compatível do jogo.");
        root.addView(statusView, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 38));

        chooseDiscButton = new Button(this);
        chooseDiscButton.setText("Selecionar imagem do jogo");
        chooseDiscButton.setOnClickListener(view -> openDiscPicker());
        root.addView(chooseDiscButton, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 42));

        prepareButton = new Button(this);
        prepareButton.setText("Preparar e recompilar neste telefone");
        prepareButton.setEnabled(false);
        prepareButton.setOnClickListener(view -> startPreparation());
        root.addView(prepareButton, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 42));

        progressBar = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        progressBar.setMax(100);
        progressBar.setProgress(0);
        LinearLayout.LayoutParams barParams = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 8);
        barParams.topMargin = 5;
        root.addView(progressBar, barParams);

        percentView = new TextView(this);
        percentView.setText("0% concluído");
        percentView.setTextColor(Color.WHITE);
        percentView.setTextSize(12);
        root.addView(percentView, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 22));

        stepView = new TextView(this);
        stepView.setText("Etapas: 0/7");
        stepView.setTextColor(Color.rgb(160, 185, 215));
        stepView.setTextSize(10);
        root.addView(stepView, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 20));

        detailView = new TextView(this);
        detailView.setTextColor(Color.rgb(210, 215, 225));
        detailView.setTextSize(10);
        detailView.setText("O aplicativo validará o disco, extrairá os módulos e gerará o ELF e o código C++.");
        root.addView(detailView, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 34));

        remainingView = new TextView(this);
        remainingView.setTextColor(Color.rgb(170, 185, 205));
        remainingView.setTextSize(10);
        remainingView.setText("Restante: 7 etapas.");
        root.addView(remainingView, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 20));

        exportButton = new Button(this);
        exportButton.setText("Exportar resultados (ZIP)");
        exportButton.setEnabled(false);
        exportButton.setOnClickListener(view -> chooseExportLocation());
        root.addView(exportButton, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 40));

        exportStatusView = new TextView(this);
        exportStatusView.setTextColor(Color.rgb(140, 165, 195));
        exportStatusView.setTextSize(9);
        root.addView(exportStatusView, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 18));

        SurfaceView gameSurface = new SurfaceView(this);
        gameSurface.setBackgroundColor(Color.BLACK);
        gameSurface.getHolder().addCallback(this);
        LinearLayout.LayoutParams surfaceParams = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 0, 1.0f);
        surfaceParams.topMargin = 5;
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
        if (resultCode != RESULT_OK || data == null || data.getData() == null) return;

        Uri uri = data.getData();
        if (requestCode == REQUEST_OPEN_ISO) {
            int flags = data.getFlags() & Intent.FLAG_GRANT_READ_URI_PERMISSION;
            try {
                getContentResolver().takePersistableUriPermission(uri, flags);
            } catch (SecurityException ignored) {
                // Some document providers grant a temporary read-only descriptor.
            }
            selectedDisc = uri;
            getPreferences(MODE_PRIVATE).edit().putString(KEY_DISC_URI, uri.toString()).apply();
            attachSelectedDisc(uri);
        } else if (requestCode == REQUEST_EXPORT_ZIP) {
            exportResultsTo(uri);
        }
    }

    private void attachSelectedDisc(Uri uri) {
        if (nativePipelineRunning()) {
            updateStatus("A preparação já está em andamento.");
            return;
        }
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

            boolean accepted = nativeAttachIso(fd, offset, length);
            updateStatus(nativeStatus());
            prepareButton.setEnabled(accepted);
            if (accepted) {
                detailView.setText("Disco reconhecido. A preparação completa será executada no armazenamento privado do telefone.");
            } else {
                detailView.setText("Esta versão inicial exige exatamente a edição PAL SCES-53326 v1.00.");
            }
        } catch (Exception error) {
            prepareButton.setEnabled(false);
            updateStatus("Falha ao abrir a imagem: " + error.getMessage());
        }
    }

    private void startPreparation() {
        if (nativePipelineRunning()) return;
        if (selectedDisc == null) {
            Toast.makeText(this, "Selecione primeiro a imagem do jogo.", Toast.LENGTH_LONG).show();
            return;
        }
        StatFs storage = new StatFs(getFilesDir().getAbsolutePath());
        long freeBytes = storage.getAvailableBytes();
        if (freeBytes < 700L * 1024L * 1024L) {
            Toast.makeText(this,
                    "Há pouco espaço livre. Libere pelo menos 700 MB antes de iniciar.",
                    Toast.LENGTH_LONG).show();
            return;
        }

        detailView.setText("A preparação pode exigir bastante memória e manter o telefone ocupado. Evite fechar o aplicativo.");
        exportStatusView.setText("");
        exportButton.setEnabled(false);
        prepareButton.setEnabled(false);
        chooseDiscButton.setEnabled(false);

        boolean started = nativeStartPipeline(getFilesDir().getAbsolutePath());
        if (!started) {
            chooseDiscButton.setEnabled(true);
            prepareButton.setEnabled(true);
            updateStatus("Não foi possível iniciar a preparação. " + nativePipelineProgress());
            return;
        }
        keepScreenOn(true);
        beginProgressPolling();
    }

    private void beginProgressPolling() {
        if (pollingProgress) return;
        pollingProgress = true;
        handler.removeCallbacks(progressPoll);
        handler.post(progressPoll);
    }

    private void pollPipelineProgress() {
        try {
            JSONObject progress = new JSONObject(nativePipelineProgress());
            String state = progress.optString("state", "idle");
            int percent = progress.optInt("percent", 0);
            int step = progress.optInt("step", 0);
            int steps = progress.optInt("steps", 7);
            String stage = progress.optString("stage", "Preparando");
            String detail = progress.optString("detail", "");
            String error = progress.optString("error", "");
            long eta = progress.optLong("eta_seconds", -1);

            progressBar.setProgress(percent);
            percentView.setText(percent + "% concluído");
            stepView.setText("Etapa " + step + "/" + steps + ": " + stage);
            detailView.setText(detail);

            if ("completed".equals(state)) {
                remainingView.setText("Restante: nenhuma etapa. Código C++ gerado.");
                exportButton.setEnabled(true);
                prepareButton.setEnabled(true);
                chooseDiscButton.setEnabled(true);
                exportStatusView.setText("Resultados armazenados no telefone. Exporte o ZIP para compartilhar.");
                keepScreenOn(false);
                pollingProgress = false;
                return;
            }

            if ("failed".equals(state)) {
                remainingView.setText("Preparação interrompida. Os arquivos parciais podem ser exportados para diagnóstico.");
                exportButton.setEnabled(new File(nativePipelineOutputDirectory()).exists());
                prepareButton.setEnabled(true);
                chooseDiscButton.setEnabled(true);
                exportStatusView.setText(error);
                keepScreenOn(false);
                pollingProgress = false;
                return;
            }

            if (eta >= 0) {
                remainingView.setText("Tempo restante estimado: " + formatDuration(eta) +
                        " · faltam " + Math.max(0, steps - step) + " etapas após a atual.");
            } else {
                remainingView.setText("Tempo restante: calculando · faltam " +
                        Math.max(0, steps - step) + " etapas após a atual.");
            }

            if (nativePipelineRunning()) {
                handler.postDelayed(progressPoll, 700);
            } else {
                pollingProgress = false;
                keepScreenOn(false);
                restorePipelineState();
            }
        } catch (Exception error) {
            detailView.setText("Não foi possível ler o status: " + error.getMessage());
            if (nativePipelineRunning()) handler.postDelayed(progressPoll, 1200);
            else pollingProgress = false;
        }
    }

    private void restorePipelineState() {
        try {
            JSONObject progress = new JSONObject(nativePipelineProgress());
            int percent = progress.optInt("percent", 0);
            int step = progress.optInt("step", 0);
            int steps = progress.optInt("steps", 7);
            progressBar.setProgress(percent);
            percentView.setText(percent + "% concluído");
            stepView.setText("Etapa " + step + "/" + steps + ": " +
                    progress.optString("stage", "Aguardando"));
            detailView.setText(progress.optString("detail", ""));
            String state = progress.optString("state", "idle");
            if ("completed".equals(state)) {
                exportButton.setEnabled(true);
                remainingView.setText("Restante: nenhuma etapa.");
            } else if ("failed".equals(state)) {
                exportButton.setEnabled(new File(nativePipelineOutputDirectory()).exists());
                remainingView.setText("A preparação foi interrompida.");
            }
        } catch (Exception ignored) {
            // The initial empty state remains visible.
        }
    }

    private String formatDuration(long seconds) {
        if (seconds < 60) return seconds + " s";
        long minutes = seconds / 60;
        long remainingSeconds = seconds % 60;
        if (minutes < 60) return minutes + " min " + remainingSeconds + " s";
        return (minutes / 60) + " h " + (minutes % 60) + " min";
    }

    private void chooseExportLocation() {
        if (!exportButton.isEnabled()) return;
        Intent intent = new Intent(Intent.ACTION_CREATE_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("application/zip");
        intent.putExtra(Intent.EXTRA_TITLE, "ShadowOfTheColossus-Android-resultados.zip");
        startActivityForResult(intent, REQUEST_EXPORT_ZIP);
    }

    private void exportResultsTo(Uri destination) {
        final String sourcePath = nativePipelineOutputDirectory();
        exportButton.setEnabled(false);
        exportStatusView.setText("Preparando ZIP para exportação...");
        new Thread(() -> {
            try {
                File root = new File(sourcePath);
                if (!root.isDirectory()) throw new IllegalStateException("Pasta de resultados não encontrada.");
                try (OutputStream raw = getContentResolver().openOutputStream(destination, "w")) {
                    if (raw == null) throw new IllegalStateException("Não foi possível abrir o destino.");
                    try (ZipOutputStream zip = new ZipOutputStream(new BufferedOutputStream(raw))) {
                        addDirectoryToZip(zip, root, root);
                    }
                }
                runOnUiThread(() -> {
                    exportStatusView.setText("ZIP exportado com sucesso.");
                    exportButton.setEnabled(true);
                    Toast.makeText(this, "Resultados exportados.", Toast.LENGTH_LONG).show();
                });
            } catch (Exception error) {
                runOnUiThread(() -> {
                    exportStatusView.setText("Falha ao exportar: " + error.getMessage());
                    exportButton.setEnabled(true);
                });
            }
        }, "sotc-export-zip").start();
    }

    private void addDirectoryToZip(ZipOutputStream zip, File root, File current) throws Exception {
        File[] children = current.listFiles();
        if (children == null) return;
        byte[] buffer = new byte[32 * 1024];
        for (File child : children) {
            if (child.isDirectory()) {
                addDirectoryToZip(zip, root, child);
                continue;
            }
            String name = root.toURI().relativize(child.toURI()).getPath();
            zip.putNextEntry(new ZipEntry(name));
            try (BufferedInputStream input = new BufferedInputStream(new FileInputStream(child))) {
                int count;
                while ((count = input.read(buffer)) != -1) zip.write(buffer, 0, count);
            }
            zip.closeEntry();
        }
    }

    private void keepScreenOn(boolean keep) {
        if (keep) getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        else getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
    }

    private void updateStatus(String text) {
        if (statusView != null) statusView.setText(text);
    }

    private void refreshNativeStatus() {
        if (statusView != null) statusView.setText(nativeStatus());
    }

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        surfaceReady = nativeSetSurface(holder.getSurface());
        if (surfaceReady) Choreographer.getInstance().postFrameCallback(this);
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        surfaceReady = nativeResizeSurface(width, height);
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        surfaceReady = false;
        Choreographer.getInstance().removeFrameCallback(this);
        nativeClearSurface();
    }

    @Override
    public void doFrame(long frameTimeNanos) {
        if (!surfaceReady) return;
        if (!nativeRenderFrame()) {
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
        if (surfaceReady) Choreographer.getInstance().postFrameCallback(this);
        if (nativePipelineRunning()) {
            keepScreenOn(true);
            beginProgressPolling();
        } else {
            restorePipelineState();
        }
    }

    @Override
    protected void onDestroy() {
        handler.removeCallbacks(progressPoll);
        pollingProgress = false;
        surfaceReady = false;
        Choreographer.getInstance().removeFrameCallback(this);
        nativeClearSurface();
        // The native worker owns the ISO until the pipeline finishes. Do not close it here.
        if (!nativePipelineRunning()) nativeClearIso();
        keepScreenOn(false);
        super.onDestroy();
    }
}
