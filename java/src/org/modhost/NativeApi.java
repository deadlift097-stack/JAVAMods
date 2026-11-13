package org.modhost;

/** What the Java host needs from the native side (the game process). */
public interface NativeApi {
    /**
     * Registers the in-game menu module. Each settings entry is
     * key, label, type, current value, min, max joined with the character U+001F.
     */
    boolean registerMenu(String moduleId, String name, String description, boolean enabled, String[] settings);

    void unregisterMenu(String moduleId);

    void log(String level, String modId, String message);
}
