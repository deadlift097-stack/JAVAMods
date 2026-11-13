package org.modhost;

/** The real {@link NativeApi}: its static native methods are bound by the C++ side. */
public final class Native implements NativeApi {
    static native boolean registerMenu0(String moduleId, String name, String description, boolean enabled,
            String[] settings);

    static native void unregisterMenu0(String moduleId);

    static native void log0(String level, String modId, String message);

    @Override
    public boolean registerMenu(String moduleId, String name, String description, boolean enabled,
            String[] settings) {
        return registerMenu0(moduleId, name, description, enabled, settings);
    }

    @Override
    public void unregisterMenu(String moduleId) {
        unregisterMenu0(moduleId);
    }

    @Override
    public void log(String level, String modId, String message) {
        log0(level, modId, message);
    }
}
