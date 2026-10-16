package org.projectcane.cane;

import android.content.Intent;
import android.os.Bundle;
import android.view.Display;
import android.view.WindowManager;

import java.util.TreeSet;

import org.libsdl.app.SDLActivity;

/** Hosts the native game. SDL's Java glue loads libSDL2 and libmain (ProjectCane). */
public class CaneActivity extends SDLActivity {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        DebugLog.start(this);   // per-launch log file for bug reports (see SavesActivity > Save debug log)
        super.onCreate(savedInstanceState);
    }

    /** Called from native code (the Options menu): opens the save backup / restore screen. */
    public void openSavesScreen() {
        runOnUiThread(() -> startActivity(new Intent(this, SavesActivity.class)));
    }

    /** Called from native code: the refresh rates (Hz, rounded) the screen can run at in its current resolution, ascending. */
    public int[] supportedRefreshRates() {
        try {
            Display display = getWindowManager().getDefaultDisplay();
            Display.Mode current = display.getMode();
            TreeSet<Integer> rates = new TreeSet<>();
            for (Display.Mode m : display.getSupportedModes())
                if (m.getPhysicalWidth() == current.getPhysicalWidth() && m.getPhysicalHeight() == current.getPhysicalHeight())
                    rates.add(Math.round(m.getRefreshRate()));
            if (rates.isEmpty()) rates.add(Math.round(current.getRefreshRate()));
            int[] out = new int[rates.size()];
            int i = 0;
            for (int r : rates) out[i++] = r;
            return out;
        } catch (Throwable t) {
            return new int[] { 60 };
        }
    }

    /**
     * Called from native code when the game's frame-rate target changes: asks Android to run the screen at the lowest
     * supported refresh rate that is at least {@code hz} (many phones stay at 60 Hz for apps that never ask).
     */
    public void requestRefreshRate(final int hz) {
        runOnUiThread(() -> {
            try {
                Display display = getWindowManager().getDefaultDisplay();
                Display.Mode current = display.getMode();
                Display.Mode best = null;
                Display.Mode highest = null;
                for (Display.Mode m : display.getSupportedModes()) {
                    if (m.getPhysicalWidth() != current.getPhysicalWidth() || m.getPhysicalHeight() != current.getPhysicalHeight())
                        continue;
                    int rate = Math.round(m.getRefreshRate());
                    if (highest == null || rate > Math.round(highest.getRefreshRate())) highest = m;
                    if (rate + 1 >= hz && (best == null || rate < Math.round(best.getRefreshRate()))) best = m;
                }
                if (best == null) best = highest;
                if (best == null) return;
                WindowManager.LayoutParams lp = getWindow().getAttributes();
                if (lp.preferredDisplayModeId != best.getModeId()) {
                    lp.preferredDisplayModeId = best.getModeId();
                    getWindow().setAttributes(lp);
                }
            } catch (Throwable ignored) {
                // the refresh rate is a nicety; never let it stop the game
            }
        });
    }

    /** SDL calls finish() when the game exits; also remove the card from the Recents list so no stale "active app" is left. */
    @Override
    public void finish() {
        finishAndRemoveTask();
    }

    /**
     * "Exit Game" ends the native main loop and SDL finishes the activity, but the process (and the game's global state) would
     * stay alive, and tapping the icon again started a second run on top of it. End the process with the activity.
     */
    @Override
    protected void onDestroy() {
        final boolean finishing = isFinishing();
        super.onDestroy();
        if (finishing)
            android.os.Process.killProcess(android.os.Process.myPid());
    }

    @Override
    protected String[] getLibraries() {
        return new String[] {
            "c++_shared",
            "avutil", "swscale", "avcodec", "avformat",   // FFmpeg (cutscenes)
            "SDL2",
            "main"
        };
    }
}
