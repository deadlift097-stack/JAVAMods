package org.modhost;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/** Describes the in-game menu module of a mod. Pass it to {@link ModContext#registerMenu}. */
public final class Menu {
    /** Called when the module's on/off toggle changes. */
    public interface ToggleListener {
        void onToggle(boolean enabled);
    }

    /** Called when a setting changes. The value is a Boolean, Integer or Double. */
    public interface ChangeListener {
        void onChange(String key, Object value);
    }

    private final List<Setting> settings = new ArrayList<>();
    private String name;
    private String description = "";
    private boolean enabled = true;
    private ToggleListener toggleListener;
    private ChangeListener changeListener;

    public Menu(String name) {
        this.name = name;
    }

    public Menu description(String text) {
        this.description = text == null ? "" : text;
        return this;
    }

    /** Starting state of the module toggle. */
    public Menu enabled(boolean value) {
        this.enabled = value;
        return this;
    }

    public Menu toggle(String key, String label, boolean defaultValue) {
        settings.add(new Setting(key, label, Setting.TOGGLE, String.valueOf(defaultValue), "", ""));
        return this;
    }

    public Menu slider(String key, String label, int defaultValue, int min, int max) {
        settings.add(new Setting(key, label, Setting.SLIDER, String.valueOf(defaultValue),
                String.valueOf(min), String.valueOf(max)));
        return this;
    }

    public Menu floatSlider(String key, String label, double defaultValue, double min, double max) {
        settings.add(new Setting(key, label, Setting.FLOAT, String.valueOf(defaultValue),
                String.valueOf(min), String.valueOf(max)));
        return this;
    }

    public Menu onToggle(ToggleListener listener) {
        this.toggleListener = listener;
        return this;
    }

    public Menu onChange(ChangeListener listener) {
        this.changeListener = listener;
        return this;
    }

    public String name() {
        return name;
    }

    public String description() {
        return description;
    }

    public boolean enabled() {
        return enabled;
    }

    public List<Setting> settings() {
        return Collections.unmodifiableList(settings);
    }

    public ToggleListener toggleListener() {
        return toggleListener;
    }

    public ChangeListener changeListener() {
        return changeListener;
    }
}
