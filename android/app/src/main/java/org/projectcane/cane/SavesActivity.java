package org.projectcane.cane;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.Process;
import android.view.Gravity;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;
import java.util.Map;
import java.util.TreeMap;
import java.util.regex.Pattern;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;
import java.util.zip.ZipOutputStream;

/**
 * Back up the game's saves to a zip the user picks (any location the system file picker offers, including
 * cloud drives) and restore them from such a zip. Opened from the in-game Options menu. Plain buttons, so a
 * gamepad (D-pad + A) drives it, like the import screen.
 *
 * The saves live in &lt;external files dir&gt;/Saves: slot1..slot6.sav and system_settings.dat (see save.cpp).
 * Android deletes that folder when the app is uninstalled, which is what this screen protects against.
 */
public class SavesActivity extends Activity {
    private static final int REQ_BACKUP = 1, REQ_RESTORE = 2, REQ_DEBUGLOG = 3;
    private static final Pattern SAVE_NAME = Pattern.compile("slot[1-9]\\.sav");
    private static final String SETTINGS_NAME = "system_settings.dat";
    private static final int MAX_FILE_BYTES = 1 << 20;   // a slot is tiny; anything bigger is not ours
    private static final byte[] MAGIC_SAVE = "PCANESAV".getBytes(StandardCharsets.US_ASCII);
    private static final byte[] MAGIC_SETTINGS = "PCANESYS".getBytes(StandardCharsets.US_ASCII);

    private TextView status;

    private File filesDir() {
        File d = getExternalFilesDir(null);
        return d != null ? d : getFilesDir();
    }

    private File savesDir() {
        return new File(filesDir(), "Saves");
    }

    private static boolean isSaveFile(String name) {
        return SAVE_NAME.matcher(name).matches() || SETTINGS_NAME.equals(name);
    }

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setGravity(Gravity.CENTER);
        root.setPadding(64, 32, 64, 32);

        status = new TextView(this);
        status.setTextSize(18);
        status.setGravity(Gravity.CENTER);
        status.setText("Save backup\n\nBack up writes your saved games (and settings) to a zip file in a place you choose, "
                + "for example a cloud drive. Restore reads such a zip back. Uninstalling the app deletes the saves "
                + "stored on the device, so keep a backup.\n\nHaving a problem? \"Save debug log\" writes the recent game logs "
                + "and device details to a zip you can attach to a bug report.");

        Button backup = new Button(this);
        backup.setText("Back up saves to a file");
        backup.setOnClickListener(v -> startBackup());
        Button restore = new Button(this);
        restore.setText("Restore saves from a file");
        restore.setOnClickListener(v -> startRestore());
        Button debug = new Button(this);
        debug.setText("Save debug log to a file");
        debug.setOnClickListener(v -> startDebugLog());
        Button close = new Button(this);
        close.setText("Back to the game");
        close.setOnClickListener(v -> finish());

        root.addView(status);
        root.addView(backup);
        root.addView(restore);
        root.addView(debug);
        root.addView(close);
        setContentView(root);
        backup.requestFocus();
    }

    // ---- backup --------------------------------------------------------------------------------

    private void startBackup() {
        if (listSaveFiles().isEmpty()) {
            status.setText("There are no saved games on this device yet, so there is nothing to back up.\n\nSave in the game first (Options > Save Game).");
            return;
        }
        Intent i = new Intent(Intent.ACTION_CREATE_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("application/zip");
        i.putExtra(Intent.EXTRA_TITLE, "ProjectCane-saves-" + new SimpleDateFormat("yyyyMMdd-HHmm", Locale.US).format(new Date()) + ".zip");
        startActivityForResult(i, REQ_BACKUP);
    }

    private TreeMap<String, File> listSaveFiles() {
        TreeMap<String, File> files = new TreeMap<>();
        File[] all = savesDir().listFiles();
        if (all != null) for (File f : all) if (f.isFile() && isSaveFile(f.getName())) files.put(f.getName(), f);
        return files;
    }

    private void writeBackup(Uri uri) {
        status.setText("Backing up...");
        new Thread(() -> {
            String message;
            try {
                int count = 0;
                try (OutputStream raw = getContentResolver().openOutputStream(uri, "wt");
                     ZipOutputStream zip = new ZipOutputStream(raw)) {
                    if (raw == null) throw new IOException("cannot open the destination");
                    for (Map.Entry<String, File> e : listSaveFiles().entrySet()) {
                        zip.putNextEntry(new ZipEntry(e.getKey()));
                        try (InputStream in = new FileInputStream(e.getValue())) {
                            byte[] buf = new byte[8192];
                            for (int n; (n = in.read(buf)) > 0; ) zip.write(buf, 0, n);
                        }
                        zip.closeEntry();
                        count++;
                    }
                }
                message = "Backed up " + count + " file" + (count == 1 ? "" : "s") + ".\n\nKeep the zip somewhere safe.";
            } catch (Throwable t) {
                message = "Backup failed: " + t.getMessage();
            }
            final String m = message;
            runOnUiThread(() -> status.setText(m));
        }, "cane-backup").start();
    }

    // ---- debug log -----------------------------------------------------------------------------

    private void startDebugLog() {
        Intent i = new Intent(Intent.ACTION_CREATE_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("application/zip");
        i.putExtra(Intent.EXTRA_TITLE, "ProjectCane-debuglog-" + new SimpleDateFormat("yyyyMMdd-HHmm", Locale.US).format(new Date()) + ".zip");
        startActivityForResult(i, REQ_DEBUGLOG);
    }

    /** Zips device details, the settings file and the recent session logs (no saves, no game data). */
    private void writeDebugLog(Uri uri) {
        status.setText("Writing the debug log...");
        new Thread(() -> {
            String message;
            try {
                int count = 0;
                try (OutputStream raw = getContentResolver().openOutputStream(uri, "wt");
                     ZipOutputStream zip = new ZipOutputStream(raw)) {
                    if (raw == null) throw new IOException("cannot open the destination");
                    zip.putNextEntry(new ZipEntry("device-info.txt"));
                    zip.write(DebugLog.header(this).getBytes(StandardCharsets.UTF_8));
                    zip.closeEntry();
                    count++;
                    File settings = new File(savesDir(), SETTINGS_NAME);
                    if (settings.isFile()) {
                        zip.putNextEntry(new ZipEntry(SETTINGS_NAME));
                        try (InputStream in = new FileInputStream(settings)) {
                            byte[] buf = new byte[8192];
                            for (int n; (n = in.read(buf)) > 0; ) zip.write(buf, 0, n);
                        }
                        zip.closeEntry();
                        count++;
                    }
                    File[] logs = DebugLog.logsDir(this).listFiles((d, n) -> n.startsWith("session-") && n.endsWith(".log"));
                    if (logs != null) {
                        java.util.Arrays.sort(logs);
                        for (File f : logs) {
                            zip.putNextEntry(new ZipEntry("Logs/" + f.getName()));
                            try (InputStream in = new FileInputStream(f)) {
                                byte[] buf = new byte[8192];
                                for (int n; (n = in.read(buf)) > 0; ) zip.write(buf, 0, n);
                            }
                            zip.closeEntry();
                            count++;
                        }
                    }
                }
                message = "Debug log saved (" + count + " files).\n\nAttach the zip to your bug report. It holds device details, "
                        + "the game settings and recent logs; it does not contain your saves or any game data.";
            } catch (Throwable t) {
                message = "Could not write the debug log: " + t.getMessage();
            }
            final String m = message;
            runOnUiThread(() -> status.setText(m));
        }, "cane-debuglog-export").start();
    }

    // ---- restore -------------------------------------------------------------------------------

    private void startRestore() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        startActivityForResult(i, REQ_RESTORE);
    }

    /** Reads and validates a backup zip. Only our own file names with our own headers are accepted. */
    private TreeMap<String, byte[]> readBackup(Uri uri) throws IOException {
        TreeMap<String, byte[]> files = new TreeMap<>();
        try (InputStream raw = getContentResolver().openInputStream(uri)) {
            if (raw == null) throw new IOException("cannot open the file");
            ZipInputStream zip = new ZipInputStream(raw);
            for (ZipEntry e; (e = zip.getNextEntry()) != null; ) {
                String name = e.getName();
                if (e.isDirectory() || !isSaveFile(name)) continue;   // ignores manifests, paths, anything else
                ByteArrayOutputStream out = new ByteArrayOutputStream();
                byte[] buf = new byte[8192];
                for (int n; (n = zip.read(buf)) > 0; ) {
                    out.write(buf, 0, n);
                    if (out.size() > MAX_FILE_BYTES) throw new IOException(name + " is too large to be a save");
                }
                byte[] data = out.toByteArray();
                byte[] magic = SETTINGS_NAME.equals(name) ? MAGIC_SETTINGS : MAGIC_SAVE;
                if (data.length < magic.length) throw new IOException(name + " is not a ProjectCane file");
                for (int k = 0; k < magic.length; k++)
                    if (data[k] != magic[k]) throw new IOException(name + " is not a ProjectCane file");
                files.put(name, data);
            }
        }
        int slots = 0;
        for (String n : files.keySet()) if (SAVE_NAME.matcher(n).matches()) slots++;
        if (slots == 0) throw new IOException("the zip contains no saved games");
        return files;
    }

    private void confirmRestore(TreeMap<String, byte[]> files) {
        int slots = 0;
        for (String n : files.keySet()) if (SAVE_NAME.matcher(n).matches()) slots++;
        final int slotCount = slots;
        new AlertDialog.Builder(this)
                .setTitle("Replace the saves on this device?")
                .setMessage("The backup holds " + slotCount + " saved game" + (slotCount == 1 ? "" : "s")
                        + (files.containsKey(SETTINGS_NAME) ? " and the game settings" : "") + ".\n\n"
                        + "Your current saves are not deleted: they are moved into a folder named Saves.before-restore-... "
                        + "next to the Saves folder.")
                .setPositiveButton("Replace", (d, w) -> applyRestore(files))
                .setNegativeButton("Cancel", null)
                .show();
    }

    private void applyRestore(TreeMap<String, byte[]> files) {
        try {
            File saves = savesDir();
            if (saves.exists()) {
                File aside = new File(filesDir(), "Saves.before-restore-" + new SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(new Date()));
                if (!saves.renameTo(aside)) throw new IOException("could not move the current saves aside");
            }
            if (!saves.mkdirs() && !saves.isDirectory()) throw new IOException("could not create the Saves folder");
            for (Map.Entry<String, byte[]> e : files.entrySet()) {
                File tmp = new File(saves, e.getKey() + ".tmp");
                try (FileOutputStream out = new FileOutputStream(tmp)) { out.write(e.getValue()); }
                if (!tmp.renameTo(new File(saves, e.getKey()))) throw new IOException("could not write " + e.getKey());
            }
        } catch (Throwable t) {
            status.setText("Restore failed: " + t.getMessage() + "\n\nYour previous saves are kept in the Saves.before-restore-... folder if they were moved.");
            return;
        }
        new AlertDialog.Builder(this)
                .setTitle("Saves restored")
                .setMessage("The game will close now. Open it again to load the restored saves.")
                .setCancelable(false)
                .setPositiveButton("OK", (d, w) -> closeGame())
                .show();
    }

    /** The running game still holds the old saves in memory, so end the process; the next launch reads the new files. */
    private void closeGame() {
        finishAffinity();
        new Handler(Looper.getMainLooper()).postDelayed(() -> Process.killProcess(Process.myPid()), 400);
    }

    @Override
    protected void onActivityResult(int req, int res, Intent data) {
        super.onActivityResult(req, res, data);
        if (res != RESULT_OK || data == null || data.getData() == null) return;
        Uri uri = data.getData();
        if (req == REQ_BACKUP) {
            writeBackup(uri);
        } else if (req == REQ_DEBUGLOG) {
            writeDebugLog(uri);
        } else if (req == REQ_RESTORE) {
            status.setText("Reading the backup...");
            new Thread(() -> {
                try {
                    TreeMap<String, byte[]> files = readBackup(uri);
                    runOnUiThread(() -> { status.setText("Backup read. Confirm to replace the saves."); confirmRestore(files); });
                } catch (Throwable t) {
                    runOnUiThread(() -> status.setText("Cannot restore from that file: " + t.getMessage()));
                }
            }, "cane-restore").start();
        }
    }
}
