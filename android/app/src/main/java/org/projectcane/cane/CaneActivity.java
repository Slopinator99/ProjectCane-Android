package org.projectcane.cane;

import android.app.ActivityOptions;
import android.content.Intent;
import android.content.SharedPreferences;
import android.hardware.display.DisplayManager;
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

    // ---- second screen (dual-display handhelds such as the AYN Thor) -------------------------------------------------

    private Display secondDisplay() {
        try {
            DisplayManager dm = (DisplayManager) getSystemService(DISPLAY_SERVICE);
            Display[] presentation = dm.getDisplays(DisplayManager.DISPLAY_CATEGORY_PRESENTATION);
            for (Display d : presentation)
                if (d.getDisplayId() != getWindowManager().getDefaultDisplay().getDisplayId() && d.getState() != Display.STATE_OFF)
                    return d;
        } catch (Throwable ignored) {
            // no second display
        }
        return null;
    }

    private boolean secondScreenWanted() {
        return getSharedPreferences("cane", MODE_PRIVATE).getBoolean("secondScreen", true);
    }

    private void updateSecondScreen() {
        runOnUiThread(() -> {
            try {
                SecondScreenActivity.sArt = getSharedPreferences("cane", MODE_PRIVATE).getInt("secondScreenArt", 0);
                final boolean running = SecondScreenActivity.sInstance != null;
                final Display d = secondDisplay();
                if (d != null && secondScreenWanted() && !running) {
                    Intent intent = new Intent(this, SecondScreenActivity.class);
                    intent.putExtra("mainDisplay", getWindowManager().getDefaultDisplay().getDisplayId());
                    intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_MULTIPLE_TASK | Intent.FLAG_ACTIVITY_NO_ANIMATION);
                    ActivityOptions options = ActivityOptions.makeBasic();
                    options.setLaunchDisplayId(d.getDisplayId());
                    startActivity(intent, options.toBundle());
                } else if ((d == null || !secondScreenWanted()) && running) {
                    SecondScreenActivity.sInstance.finish();
                }
            } catch (Throwable t) {
                android.util.Log.w("cane", "second screen: " + t);
            }
        });
    }

    @Override
    protected void onResume() {
        super.onResume();
        updateSecondScreen();
    }

    @Override
    protected void onStop() {
        super.onStop();   // not onPause: handing the focus between the two displays pauses this activity briefly
        SecondScreenActivity second = SecondScreenActivity.sInstance;
        if (second != null && !isChangingConfigurations())
            second.finish();
    }

    /** Called from native code: -1 = no second display, 0 = available but switched off, 1 = on. */
    public int secondScreenState() {
        return secondDisplay() == null ? -1 : (secondScreenWanted() ? 1 : 0);
    }

    /** Called from native code (Options menu): switch the second screen on or off. */
    public void toggleSecondScreen() {
        SharedPreferences p = getSharedPreferences("cane", MODE_PRIVATE);
        p.edit().putBoolean("secondScreen", !secondScreenWanted()).apply();
        updateSecondScreen();
    }

    /** Called from native code: which picture the second screen shows (0 follow the game, 1 black, 2 title, 3..8 fixed episode). */
    public int secondScreenArt() {
        return getSharedPreferences("cane", MODE_PRIVATE).getInt("secondScreenArt", 0);
    }

    /** Called from native code (Options menu): next choice in the list. */
    public void cycleSecondScreenArt() {
        int next = (secondScreenArt() + 1) % 10;
        getSharedPreferences("cane", MODE_PRIVATE).edit().putInt("secondScreenArt", next).apply();
        SecondScreenActivity.sArt = next;
        SecondScreenActivity.refresh();
    }

    /** Called from native code: the episode (game world) being played changed; -1 on the title screen. */
    public void setEpisode(int episode) {
        SecondScreenActivity.episodeChanged(episode);
    }

    /** Called from native code (the Options menu): opens the save backup / restore screen. */
    public void openSavesScreen() {
        runOnUiThread(() -> startActivity(new Intent(this, SavesActivity.class)));
    }

    /** Called from native code: a short message on top of the game. */
    public void showToast(final String text) {
        runOnUiThread(() -> android.widget.Toast.makeText(this, text, android.widget.Toast.LENGTH_SHORT).show());
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
