package org.projectcane.cane;

import android.app.Activity;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract;
import android.view.Gravity;
import android.view.View;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;

import org.projectcane.cane.extract.BrxRebuild;
import org.projectcane.cane.extract.IsoSource;
import org.projectcane.cane.extract.MediaExtractor;
import org.projectcane.cane.extract.ProgressListener;
import org.projectcane.cane.extract.Sly1Extractor;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.List;

/**
 * Launcher. If the game data is already in the app's files directory it starts the game at once.
 * Otherwise the user either
 *  (a) picks their own Sly Cooper NTSC-U PS2 ISO (SCUS-97198) and the data is extracted on the device
 *      (a port of the desktop File Extractor), or
 *  (b) picks a folder that was already extracted on a PC (Worlds/ + Cutscenes/) and it is copied in.
 * No game data is bundled in the APK. Both buttons are focusable, so a gamepad (D-pad + A) drives it.
 */
public class ImportActivity extends Activity {
    private static final int REQ_ISO = 1, REQ_TREE = 2;
    private TextView status;
    private ProgressBar bar;
    private Button pickIso, pickTree;
    private volatile boolean busy;
    private volatile boolean cancelled;

    private File filesDir() {
        File d = getExternalFilesDir(null);
        return d != null ? d : getFilesDir();
    }

    private boolean hasData() {
        File dir = filesDir();
        if (new File(dir, Sly1Extractor.IN_PROGRESS_MARKER).exists()) return false;
        // The last file the extractor writes; also present in data staged by hand or copied from a PC.
        // Levels extracted before the BRX rebuilder existed are rejected by the game, so they count as "no data".
        return new File(dir, "Worlds/Sounds/Prompt/PromptPrk18.wav").isFile()
                && new File(dir, "Cutscenes/Cutscene_17.wav").isFile()
                && BrxRebuild.allLevelsRebuilt(new File(dir, "Worlds"));
    }

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        if (hasData()) { startGame(); return; }

        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setGravity(Gravity.CENTER);
        root.setPadding(64, 32, 64, 32);
        status = new TextView(this);
        status.setTextSize(18);
        status.setGravity(Gravity.CENTER);
        status.setText("Game data not found.\n\nSelect your Sly Cooper and the Thievius Raccoonus (NTSC-U, SCUS-97198) "
                + "PS2 .iso file. The game data is extracted on this device. You need about 4 GB free.");
        bar = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        bar.setMax(1000);
        bar.setVisibility(View.GONE);
        pickIso = new Button(this);
        pickIso.setText("Select PS2 ISO and extract");
        pickIso.setOnClickListener(v -> {
            Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            i.addCategory(Intent.CATEGORY_OPENABLE);
            i.setType("*/*");
            startActivityForResult(i, REQ_ISO);
        });
        pickTree = new Button(this);
        pickTree.setText("Or: copy a folder already extracted on a PC");
        pickTree.setOnClickListener(v -> startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), REQ_TREE));
        root.addView(status);
        root.addView(bar, new LinearLayout.LayoutParams(900, LinearLayout.LayoutParams.WRAP_CONTENT));
        root.addView(pickIso);
        root.addView(pickTree);
        setContentView(root);
        pickIso.requestFocus();
    }

    private void startGame() {
        startActivity(new Intent(this, CaneActivity.class));
        finish();
    }

    @Override
    public void onBackPressed() {
        if (busy) { cancelled = true; setStatus("Cancelling..."); return; }
        super.onBackPressed();
    }

    @Override
    protected void onActivityResult(int req, int res, Intent data) {
        super.onActivityResult(req, res, data);
        if (res != RESULT_OK || data == null || data.getData() == null || busy) return;
        final Uri uri = data.getData();
        busy = true;
        cancelled = false;
        pickIso.setEnabled(false);
        pickTree.setEnabled(false);
        bar.setVisibility(View.VISIBLE);
        setStatus("Starting...");
        Thread t = new Thread(() -> {
            try {
                if (req == REQ_ISO) extractIso(uri); else copyTree(uri);
                runOnUiThread(this::startGame);
            } catch (Throwable e) {
                fail((cancelled ? "Cancelled." : "Failed: " + e.getMessage())
                        + "\nSelect the file again to resume; finished files are kept.");
            }
        }, "cane-import");
        t.start();
    }

    // ---- (a) extract from the ISO ------------------------------------------------------------

    private void extractIso(Uri uri) throws Exception {
        String table;
        try (InputStream in = getAssets().open("DialogAudioTable.inc")) { table = MediaExtractor.readAll(in); }
        ParcelFileDescriptor pfd = getContentResolver().openFileDescriptor(uri, "r");
        if (pfd == null) throw new java.io.IOException("Cannot open the selected file.");
        try (FileInputStream fis = new ParcelFileDescriptor.AutoCloseInputStream(pfd)) {
            IsoSource iso = new IsoSource(fis.getChannel());
            File out = filesDir();
            if (out.getUsableSpace() < 4L * 1024 * 1024 * 1024)
                throw new java.io.IOException("Not enough free space (about 4 GB needed).");
            Sly1Extractor.run(iso, out, table, new ProgressListener() {
                private long lastUi;
                @Override public void onProgress(String item, double f) {
                    long now = System.currentTimeMillis();
                    if (now - lastUi < 150 && f < 1.0) return;
                    lastUi = now;
                    final int p = (int) (f * 1000);
                    final String msg = "Extracting game data... " + (p / 10) + "%\n" + item;
                    runOnUiThread(() -> { bar.setProgress(p); status.setText(msg); });
                }
                @Override public boolean isCancelled() { return cancelled; }
            });
        }
    }

    // ---- (b) copy an already-extracted folder -----------------------------------------------

    private void copyTree(Uri tree) throws Exception {
        File out = filesDir();
        File marker = new File(out, Sly1Extractor.IN_PROGRESS_MARKER);
        out.mkdirs();
        new FileOutputStream(marker).close();
        String root = DocumentsContract.getTreeDocumentId(tree);
        String base = findWorldsParent(tree, root);
        if (base == null)
            throw new java.io.IOException("No 'Worlds' folder found. Pick the folder that contains Worlds and Cutscenes.");
        List<String[]> files = new ArrayList<>(); // {docId, relPath, size}
        long[] total = {0};
        walk(tree, base, "", files, total);
        long done = 0;
        for (String[] f : files) {
            if (cancelled) throw new java.io.IOException("Cancelled.");
            File dst = new File(out, f[1]);
            long size = Long.parseLong(f[2]);
            if (dst.isFile() && dst.length() == size) { done += size; progress(done, total[0], f[1]); continue; }
            File dir = dst.getParentFile();
            if (dir != null) dir.mkdirs();
            File tmp = new File(dst.getPath() + ".part");
            Uri src = DocumentsContract.buildDocumentUriUsingTree(tree, f[0]);
            try (InputStream in = getContentResolver().openInputStream(src); OutputStream os = new FileOutputStream(tmp)) {
                byte[] buf = new byte[1 << 20];
                int n;
                while ((n = in.read(buf)) > 0) {
                    os.write(buf, 0, n);
                    done += n;
                    progress(done, total[0], f[1]);
                }
            }
            if (dst.exists()) dst.delete();
            if (!tmp.renameTo(dst)) throw new java.io.IOException("rename failed: " + f[1]);
        }
        marker.delete();
    }

    private String findWorldsParent(Uri tree, String rootId) {
        List<String[]> kids = children(tree, rootId);
        for (String[] k : kids) if (k[1].equals("Worlds") && isDir(k[2])) return rootId;
        for (String[] k : kids) {
            if (!isDir(k[2])) continue;
            for (String[] g : children(tree, k[0]))
                if (g[1].equals("Worlds") && isDir(g[2])) return k[0];
        }
        return null;
    }

    private void walk(Uri tree, String docId, String rel, List<String[]> out, long[] total) {
        for (String[] k : children(tree, docId)) {
            String path = rel.isEmpty() ? k[1] : rel + "/" + k[1];
            if (isDir(k[2])) {
                walk(tree, k[0], path, out, total);
            } else {
                out.add(new String[] {k[0], path, k[3]});
                total[0] += Long.parseLong(k[3]);
            }
        }
    }

    private static boolean isDir(String mime) { return DocumentsContract.Document.MIME_TYPE_DIR.equals(mime); }

    /** Returns {docId, name, mime, size} per child. */
    private List<String[]> children(Uri tree, String docId) {
        List<String[]> r = new ArrayList<>();
        Uri u = DocumentsContract.buildChildDocumentsUriUsingTree(tree, docId);
        String[] cols = {DocumentsContract.Document.COLUMN_DOCUMENT_ID, DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                DocumentsContract.Document.COLUMN_MIME_TYPE, DocumentsContract.Document.COLUMN_SIZE};
        try (Cursor c = getContentResolver().query(u, cols, null, null, null)) {
            while (c != null && c.moveToNext())
                r.add(new String[] {c.getString(0), c.getString(1), c.getString(2), Long.toString(c.getLong(3))});
        }
        return r;
    }

    private long lastUi;
    private void progress(long done, long total, String name) {
        long now = System.currentTimeMillis();
        if (now - lastUi < 200 && done < total) return;
        lastUi = now;
        final int p = total > 0 ? (int) (done * 1000 / total) : 0;
        final String msg = String.format("Copying game data... %d%%  (%d / %d MB)\n%s", p / 10, done >> 20, total >> 20, name);
        runOnUiThread(() -> { bar.setProgress(p); status.setText(msg); });
    }

    private void setStatus(String s) { runOnUiThread(() -> status.setText(s)); }

    private void fail(String s) {
        busy = false;
        runOnUiThread(() -> {
            status.setText(s);
            pickIso.setEnabled(true);
            pickTree.setEnabled(true);
            pickIso.requestFocus();
        });
    }
}
