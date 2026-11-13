package org.modhost;

import java.io.File;

/** Everything a mod can do. Passed to {@link Mod#onLoad}. */
public interface ModContext {
    /** Return true to consume the event. */
    interface KeyHandler {
        boolean onKey(int keyCode, boolean down, int unicodeChar);
    }

    /** Return true to consume the event. */
    interface TouchHandler {
        boolean onTouch(int action, int pointerId, float x, float y);
    }

    /** Return true to consume the event. */
    interface MouseHandler {
        boolean onMouse(int button, boolean down);
    }

    /** Needs "attackEvent": true in the host's config. */
    interface AttackHandler {
        void onAttack(long timeMs);
    }

    /** About 20 times a second from a host thread. This is NOT the game tick. */
    interface TickHandler {
        void onTick(long dtMs);
    }

    String id();

    String name();

    String version();

    String author();

    String description();

    /** Folder the jar lives in. */
    File dir();

    /** Private folder for this mod's files. */
    File dataDir();

    void log(String message);

    void warn(String message);

    void error(String message);

    void onKey(KeyHandler handler);

    void onTouch(TouchHandler handler);

    void onMouse(MouseHandler handler);

    void onAttack(AttackHandler handler);

    void onTick(TickHandler handler);

    TimerHandle after(long delayMs, Runnable task);

    TimerHandle every(long periodMs, Runnable task);

    /** Register the in-game menu. Call at most once, from onLoad. */
    void registerMenu(Menu menu);

    Settings settings();

    Storage storage();

    /** False while the module's menu toggle is off. */
    boolean isEnabled();
}
