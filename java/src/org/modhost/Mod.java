package org.modhost;

/**
 * Entry point of a Java mod. The jar must contain a "modhost-mod.properties" file whose
 * "main" key names a public class implementing this interface (with a public no-argument
 * constructor).
 */
public interface Mod {
    /** Called once after the jar is loaded. Register menus, events and timers here. */
    void onLoad(ModContext ctx) throws Exception;

    /** Called when the host stops (the game closes or the mod is disabled). */
    default void onUnload() {}
}
