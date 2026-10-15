package org.projectcane.cane;

import android.app.ActivityManager;
import android.app.ApplicationExitInfo;
import android.content.Context;
import android.content.pm.ConfigurationInfo;
import android.content.pm.PackageInfo;
import android.os.Build;
import android.os.Environment;
import android.os.StatFs;

import java.io.BufferedReader;
import java.io.BufferedWriter;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.OutputStreamWriter;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.Arrays;
import java.util.Comparator;
import java.util.Date;
import java.util.List;
import java.util.Locale;

/**
 * Writes one log file per launch to &lt;external files dir&gt;/Logs so a problem can be reported without adb: a
 * device header (model, Android, SoC, app version, how the previous run ended) followed by this app's logcat output.
 * The last {@link #KEEP} sessions are kept. {@link SavesActivity} can export them as one zip.
 */
final class DebugLog {
    private static final int KEEP = 5;
    private static volatile boolean started;

    private DebugLog() {}

    static File logsDir(Context c) {
        File base = c.getExternalFilesDir(null);
        if (base == null) base = c.getFilesDir();
        return new File(base, "Logs");
    }

    /** Starts capturing for this process. Safe to call more than once; never throws. */
    static synchronized void start(Context c) {
        if (started) return;
        started = true;
        try {
            File dir = logsDir(c);
            if (!dir.isDirectory() && !dir.mkdirs()) return;
            prune(dir);
            String stamp = new SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(new Date());
            final File file = new File(dir, "session-" + stamp + ".log");
            final String header = header(c);
            Thread t = new Thread(() -> capture(file, header), "cane-debuglog");
            t.setDaemon(true);
            t.start();
        } catch (Throwable ignored) {
            // logging must never be the reason the game does not start
        }
    }

    private static void prune(File dir) {
        File[] old = dir.listFiles((d, n) -> n.startsWith("session-") && n.endsWith(".log"));
        if (old == null || old.length < KEEP) return;
        Arrays.sort(old, Comparator.comparing(File::getName));
        for (int i = 0; i <= old.length - KEEP; i++) //noinspection ResultOfMethodCallIgnored
            old[i].delete();
    }

    private static void capture(File file, String header) {
        Process p = null;
        try (BufferedWriter w = new BufferedWriter(new OutputStreamWriter(new FileOutputStream(file), StandardCharsets.UTF_8))) {
            w.write(header);
            w.flush();
            p = new ProcessBuilder("logcat", "-v", "threadtime", "--pid=" + android.os.Process.myPid()).redirectErrorStream(true).start();
            try (BufferedReader r = new BufferedReader(new InputStreamReader(p.getInputStream(), StandardCharsets.UTF_8))) {
                String line;
                while ((line = r.readLine()) != null) {
                    w.write(line);
                    w.newLine();
                    if (!r.ready()) w.flush();
                }
            }
        } catch (Throwable ignored) {
            // the process (and this log) ends with the app
        } finally {
            if (p != null) p.destroy();
        }
    }

    /** Device and app information that is useful in every bug report. */
    static String header(Context c) {
        StringBuilder s = new StringBuilder();
        s.append("ProjectCane Android debug log\n");
        s.append("time: ").append(new SimpleDateFormat("yyyy-MM-dd HH:mm:ss Z", Locale.US).format(new Date())).append('\n');
        try {
            PackageInfo pi = c.getPackageManager().getPackageInfo(c.getPackageName(), 0);
            s.append("app: ").append(pi.packageName).append(' ').append(pi.versionName).append(" (code ").append(pi.versionCode).append(")\n");
        } catch (Throwable t) {
            s.append("app: unknown\n");
        }
        s.append("device: ").append(Build.MANUFACTURER).append(' ').append(Build.MODEL)
                .append(" (").append(Build.DEVICE).append(", board ").append(Build.BOARD).append(")\n");
        s.append("android: ").append(Build.VERSION.RELEASE).append(" (API ").append(Build.VERSION.SDK_INT).append(")\n");
        if (Build.VERSION.SDK_INT >= 31) s.append("soc: ").append(Build.SOC_MANUFACTURER).append(' ').append(Build.SOC_MODEL).append('\n');
        s.append("hardware: ").append(Build.HARDWARE).append(", abis ").append(Arrays.toString(Build.SUPPORTED_ABIS)).append('\n');
        try {
            ActivityManager am = (ActivityManager) c.getSystemService(Context.ACTIVITY_SERVICE);
            ConfigurationInfo ci = am.getDeviceConfigurationInfo();
            s.append("gles: ").append(ci.getGlEsVersion()).append('\n');
            ActivityManager.MemoryInfo mi = new ActivityManager.MemoryInfo();
            am.getMemoryInfo(mi);
            s.append("ram: ").append(mi.totalMem >> 20).append(" MB total, ").append(mi.availMem >> 20).append(" MB free\n");
        } catch (Throwable ignored) { }
        try {
            StatFs fs = new StatFs(Environment.getDataDirectory().getPath());
            s.append("storage free: ").append(fs.getAvailableBytes() >> 20).append(" MB\n");
        } catch (Throwable ignored) { }
        File files = c.getExternalFilesDir(null);
        s.append("data present: ").append(files != null && new File(files, "Worlds").isDirectory()).append('\n');
        if (Build.VERSION.SDK_INT >= 30) {
            try {
                ActivityManager am = (ActivityManager) c.getSystemService(Context.ACTIVITY_SERVICE);
                List<ApplicationExitInfo> exits = am.getHistoricalProcessExitReasons(c.getPackageName(), 0, 3);
                s.append("previous runs ended:\n");
                for (ApplicationExitInfo e : exits)
                    s.append("  ").append(new SimpleDateFormat("MM-dd HH:mm:ss", Locale.US).format(new Date(e.getTimestamp())))
                            .append(" reason=").append(reason(e.getReason())).append(" status=").append(e.getStatus())
                            .append(" ").append(e.getDescription() == null ? "" : e.getDescription()).append('\n');
            } catch (Throwable ignored) { }
        }
        s.append("----- logcat -----\n");
        return s.toString();
    }

    private static String reason(int r) {
        switch (r) {
            case ApplicationExitInfo.REASON_EXIT_SELF: return "exit-self";
            case ApplicationExitInfo.REASON_SIGNALED: return "signaled";
            case ApplicationExitInfo.REASON_LOW_MEMORY: return "low-memory";
            case ApplicationExitInfo.REASON_CRASH: return "java-crash";
            case ApplicationExitInfo.REASON_CRASH_NATIVE: return "native-crash";
            case ApplicationExitInfo.REASON_ANR: return "anr";
            case ApplicationExitInfo.REASON_INITIALIZATION_FAILURE: return "init-failure";
            case ApplicationExitInfo.REASON_PERMISSION_CHANGE: return "permission-change";
            case ApplicationExitInfo.REASON_EXCESSIVE_RESOURCE_USAGE: return "excessive-resource-usage";
            case ApplicationExitInfo.REASON_USER_REQUESTED: return "user-requested";
            case ApplicationExitInfo.REASON_USER_STOPPED: return "user-stopped";
            case ApplicationExitInfo.REASON_DEPENDENCY_DIED: return "dependency-died";
            case ApplicationExitInfo.REASON_OTHER: return "other";
            default: return "unknown(" + r + ")";
        }
    }
}
