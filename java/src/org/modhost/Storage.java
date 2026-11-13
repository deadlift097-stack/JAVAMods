package org.modhost;

/** Small key/value store kept between runs, private to the mod. */
public interface Storage {
    String getString(String key, String defaultValue);

    long getLong(String key, long defaultValue);

    boolean getBoolean(String key, boolean defaultValue);

    void putString(String key, String value);

    void putLong(String key, long value);

    void putBoolean(String key, boolean value);

    void remove(String key);
}
