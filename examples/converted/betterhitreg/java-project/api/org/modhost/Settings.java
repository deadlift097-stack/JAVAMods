package org.modhost;

/** Current values of the settings declared in a {@link Menu}. */
public interface Settings {
    boolean getBoolean(String key);

    int getInt(String key);

    double getDouble(String key);

    String getString(String key);
}
