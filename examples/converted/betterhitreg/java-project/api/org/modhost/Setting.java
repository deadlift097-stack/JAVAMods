package org.modhost;

/** One entry of a {@link Menu}. Created through the Menu helper methods. */
public final class Setting {
    public static final String TOGGLE = "toggle";
    public static final String SLIDER = "slider";
    public static final String FLOAT = "float";

    public final String key;
    public final String label;
    public final String type;
    public final String defaultValue;
    public final String min;
    public final String max;

    Setting(String key, String label, String type, String defaultValue, String min, String max) {
        this.key = key;
        this.label = label;
        this.type = type;
        this.defaultValue = defaultValue;
        this.min = min;
        this.max = max;
    }
}
