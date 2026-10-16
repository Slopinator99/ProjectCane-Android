package org.projectcane.cane;

import android.app.Activity;
import android.app.ActivityOptions;
import android.content.Intent;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.os.Bundle;
import android.view.Gravity;
import android.view.View;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.TextView;

import java.io.File;
import java.util.ArrayList;
import java.util.List;
import java.util.Random;

/**
 * Shown on the second display of dual-screen handhelds (for example the AYN Thor) while the game runs on the main one.
 * It shows a picture for the episode being played. The pictures are not part of the app: put your own files in
 * {@code <app files>/SecondScreen/} named {@code 0.png} ... {@code 5.png} (or {@code 1_a.jpg}, {@code 1_b.jpg}: one is picked at random);
 * {@code title.png} is used on the title screen. Without a file a plain title card is drawn.
 * The window never takes focus, so controllers keep talking to the game.
 */
public class SecondScreenActivity extends Activity {
    static volatile SecondScreenActivity sInstance;
    static volatile int sEpisode = -1;   // what the game is playing (-1 = title screen)
    static volatile int sArt = 0;        // the player's choice: 0 follow the game, 1 black, 2 title, 3..9 a fixed picture (0..5 episodes, 6 hideout)

    /** The picture to show now: -9 black, -1 title, 0..5 an episode. */
    static int effective() {
        if (sArt <= 0) return sEpisode;
        if (sArt == 1) return -9;
        if (sArt == 2) return -1;
        return sArt - 3;
    }

    static native int[] nativeDecodeFrame(String path, int mode, double seconds);

    // key -> {cutscene number, mode (0 = the picture at a given time), seconds}: the episode title cards of the retail movies
    private static final Object[][] AUTO_ART = {
        { "title", 14, 0, 30.0 }, { "0", 0, 0, 12.0 }, { "6", 7, 0, 45.5 },
        { "1", 4, 0, 58.0 }, { "2", 6, 0, 87.0 }, { "3", 8, 0, 53.0 }, { "4", 10, 0, 71.0 }, { "5", 12, 0, 32.0 },
    };

    private FrameLayout root;
    private ImageView image;
    private TextView title;
    private TextView subtitle;
    private int shownEpisode = Integer.MIN_VALUE;

    static final String[] EPISODE_NAMES = {
        "Sly Cooper", "Tide of Terror", "Sunset Snake Eyes", "Vicious Voodoo", "Fire in the Sky", "The Cold Heart of Hate", "The Hideout"
    };
    private static final int[][] EPISODE_COLOURS = {
        { 0xFF1B2A41, 0xFF0B1320 }, { 0xFF0E4D64, 0xFF04202B }, { 0xFF7A4A1D, 0xFF2A1708 },
        { 0xFF2F5D3A, 0xFF0D1F12 }, { 0xFF3B4C73, 0xFF0F1626 }, { 0xFF5A1F2B, 0xFF1A080C }, { 0xFF1B2A41, 0xFF0B1320 }
    };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        // Never take focus: the controller and the game keep the input focus on the other display.
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE | WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

        root = new FrameLayout(this);
        root.setBackgroundColor(Color.BLACK);

        image = new ImageView(this);
        image.setScaleType(ImageView.ScaleType.CENTER_CROP);
        root.addView(image, new FrameLayout.LayoutParams(-1, -1));

        title = new TextView(this);
        title.setTextColor(Color.WHITE);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        title.setTextSize(34f);
        title.setShadowLayer(8f, 0f, 2f, Color.BLACK);
        title.setGravity(Gravity.CENTER);
        subtitle = new TextView(this);
        subtitle.setTextColor(0xCCFFFFFF);
        subtitle.setTextSize(16f);
        subtitle.setShadowLayer(6f, 0f, 1f, Color.BLACK);
        subtitle.setGravity(Gravity.CENTER);

        android.widget.LinearLayout labels = new android.widget.LinearLayout(this);
        labels.setOrientation(android.widget.LinearLayout.VERTICAL);
        labels.addView(title);
        labels.addView(subtitle);
        FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(-1, -2, Gravity.BOTTOM);
        lp.bottomMargin = 56;
        root.addView(labels, lp);

        setContentView(root);
        root.setSystemUiVisibility(View.SYSTEM_UI_FLAG_FULLSCREEN | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
        sInstance = this;
        show(effective());
        makeAutoArt();
        returnFocusToGame();
    }

    /**
     * A new activity on another display becomes the "top resumed" one, which moves the input focus off the game. Give it back
     * to the game's own activity on the main display right away (the second screen never needs input).
     */
    private void returnFocusToGame() {
        final int mainDisplay = getIntent().getIntExtra("mainDisplay", 0);
        root.postDelayed(() -> {
            try {
                Intent back = new Intent(this, CaneActivity.class);
                back.addFlags(Intent.FLAG_ACTIVITY_REORDER_TO_FRONT | Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_NO_ANIMATION);
                ActivityOptions options = ActivityOptions.makeBasic();
                options.setLaunchDisplayId(mainDisplay);
                startActivity(back, options.toBundle());
            } catch (Throwable t) {
                android.util.Log.w("cane", "second screen focus: " + t);
            }
        }, 250);
    }

    private File artDir() {
        File dir = new File(getExternalFilesDir(null), "SecondScreen");
        dir.mkdirs();
        return dir;
    }

    /**
     * Makes the episode pictures from the player's own cutscene files, once: the title card at the end of each episode's intro
     * movie. Nothing is shipped with the app. Pictures the player puts in the folder by hand take priority.
     */
    private void makeAutoArt() {
        final File dir = artDir();
        final File cutscenes = new File(getExternalFilesDir(null), "Cutscenes");
        new Thread(() -> {
            boolean any = false;
            for (Object[] a : AUTO_ART) {
                File out = new File(dir, "ep_" + a[0] + ".jpg");
                if (out.exists()) continue;
                File movie = new File(cutscenes, String.format(java.util.Locale.US, "Cutscene_%02d.m2v", (Integer) a[1]));
                if (!movie.exists()) continue;
                try {
                    int[] raw = nativeDecodeFrame(movie.getAbsolutePath(), (Integer) a[2], (Double) a[3]);
                    if (raw == null || raw.length < 3) continue;
                    int w = raw[0], h = raw[1];
                    Bitmap full = Bitmap.createBitmap(raw, 2, w, w, h, Bitmap.Config.ARGB_8888);
                    Bitmap cropped = cropBlackBars(full);
                    try (java.io.FileOutputStream fos = new java.io.FileOutputStream(out)) {
                        cropped.compress(Bitmap.CompressFormat.JPEG, 92, fos);
                    }
                    any = true;
                } catch (Throwable t) {
                    android.util.Log.w("cane", "second screen art " + a[0] + ": " + t);
                }
            }
            if (any) runOnUiThread(() -> { shownEpisode = Integer.MIN_VALUE; show(effective()); });
        }, "cane-second-screen-art").start();
    }

    private static boolean dark(int p) {
        return (((p >> 16) & 0xFF) + ((p >> 8) & 0xFF) + (p & 0xFF)) < 36;
    }

    /** The movies are letterboxed: keep only the picture. */
    private static Bitmap cropBlackBars(Bitmap b) {
        int w = b.getWidth(), h = b.getHeight();
        int top = 0, bottom = h - 1, left = 0, right = w - 1;
        while (top < bottom && rowDark(b, top, w)) top++;
        while (bottom > top && rowDark(b, bottom, w)) bottom--;
        while (left < right && colDark(b, left, top, bottom)) left++;
        while (right > left && colDark(b, right, top, bottom)) right--;
        if (right - left < w / 4 || bottom - top < h / 4) return b;
        return Bitmap.createBitmap(b, left, top, right - left + 1, bottom - top + 1);
    }

    private static boolean rowDark(Bitmap b, int y, int w) {
        for (int x = 0; x < w; x += 4) if (!dark(b.getPixel(x, y))) return false;
        return true;
    }

    private static boolean colDark(Bitmap b, int x, int top, int bottom) {
        for (int y = top; y <= bottom; y += 4) if (!dark(b.getPixel(x, y))) return false;
        return true;
    }

    @Override
    protected void onDestroy() {
        if (sInstance == this) sInstance = null;
        super.onDestroy();
    }

    /** Called when the game moves to another episode (any thread). */
    static void episodeChanged(final int episode) {
        sEpisode = episode;
        refresh();
    }

    static void refresh() {
        final SecondScreenActivity a = sInstance;
        if (a != null) a.runOnUiThread(() -> a.show(effective()));
    }

    private void show(int episode) {
        if (episode == shownEpisode) return;
        shownEpisode = episode;
        if (episode == -9) {   // black
            image.setImageBitmap(null);
            image.setBackgroundColor(Color.BLACK);
            title.setVisibility(View.GONE);
            subtitle.setVisibility(View.GONE);
            return;
        }
        Bitmap bitmap = loadPicture(episode);
        if (bitmap != null) {
            image.setImageBitmap(bitmap);
            image.setBackground(null);
        } else {
            image.setImageBitmap(null);
            int[] c = EPISODE_COLOURS[episode >= 0 && episode < EPISODE_COLOURS.length ? episode : 0];
            GradientDrawable g = new GradientDrawable(GradientDrawable.Orientation.TOP_BOTTOM, c);
            image.setBackground(g);
        }
        title.setText(episode >= 0 && episode < EPISODE_NAMES.length ? EPISODE_NAMES[episode] : EPISODE_NAMES[0]);
        title.setVisibility(bitmap != null ? View.GONE : View.VISIBLE);
        subtitle.setText("");
        subtitle.setVisibility(View.GONE);
    }

    private Bitmap loadPicture(int episode) {
        try {
            File dir = artDir();
            File[] files = dir.listFiles();
            if (files == null) return null;
            String key = episode < 0 ? "title" : Integer.toString(episode);
            List<File> match = new ArrayList<>();
            for (File f : files) {
                String n = f.getName().toLowerCase();
                String stem = n.contains(".") ? n.substring(0, n.lastIndexOf('.')) : n;
                boolean image = n.endsWith(".png") || n.endsWith(".jpg") || n.endsWith(".jpeg") || n.endsWith(".webp");
                if (image && (stem.equals(key) || stem.startsWith(key + "_"))) match.add(f);
            }
            if (match.isEmpty()) {
                File auto = new File(dir, "ep_" + key + ".jpg");   // made from the player's own cutscenes
                if (auto.exists()) match.add(auto);
            }
            if (match.isEmpty()) return null;
            File pick = match.get(new Random().nextInt(match.size()));

            BitmapFactory.Options o = new BitmapFactory.Options();
            o.inJustDecodeBounds = true;
            BitmapFactory.decodeFile(pick.getAbsolutePath(), o);
            int sample = 1;
            while (o.outWidth / sample > 2400 || o.outHeight / sample > 2400) sample *= 2;
            o = new BitmapFactory.Options();
            o.inSampleSize = sample;
            return BitmapFactory.decodeFile(pick.getAbsolutePath(), o);
        } catch (Throwable t) {
            return null;
        }
    }
}
