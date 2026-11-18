// Game input: the actions and bindings the config files set up (AddAction,
// rangebind, scale), key and mouse state, and the queries the game makes
// (IsCommandOn, GetAxisOffsets).
//
// Keys are tracked by DirectInput scan code, which is what the config files
// bind ("##57" is the space bar).
#ifndef SHOGO_ENGINE_INPUT_H
#define SHOGO_ENGINE_INPUT_H

#include <map>
#include <string>
#include <vector>

namespace shogo {

class Input {
public:
    // A console command for the input system. Returns false if it isn't one.
    bool command(const std::vector<std::string> &args);

    void keyDown(int dik)
    {
        if (dik <= 0 || dik >= 256) return;
        m_keys[dik] = true;
        if (m_trackKeyboard) m_tracked.push_back({false, dik});
    }
    void keyUp(int dik) { if (dik > 0 && dik < 256) m_keys[dik] = false; }
    void mouseButton(int button, bool down)
    {
        if (button < 0 || button >= 8) return;
        m_buttons[button] = down;
        if (down && m_trackMouse) m_tracked.push_back({true, button});
    }
    void mouseMotion(float dx, float dy) { m_delta[0] += dx; m_delta[1] += dy; }
    void mouseWheel(float dz) { m_delta[2] += dz; }

    bool enabled = true;  // SetInputState
    bool isCommandOn(int action) const;
    // Fills offsets[3] with the mouse movement since the last call, scaled.
    void axisOffsets(float *offsets);
    void clear();

    int actionId(const std::string &name) const;
    // The actions and bindings as config lines (AddAction, rangebind, scale),
    // for autoexec.cfg.
    std::string configLines() const;
    size_t bindingCount() const { return m_bindings.size(); }

    // All commands that are on now (for OnCommandOn/Off edges).
    std::vector<int> activeCommands() const;

    // Key names as DirectInput gives them ("Space", "Up", "A"), by scan code;
    // "" if unknown. keyCode is the reverse (any case), 0 if unknown.
    static std::string keyName(int dik);
    static int keyCode(const std::string &name);
    // A mouse object's name: "X Axis", "Y Axis", "Z Axis", "Left Button",
    // "Right Button", "Middle Button", "Button 3"...
    static std::string mouseObjectName(bool axis, int code);

    // What's bound to each key or mouse object, for GetDeviceBindings.
    struct Trigger {
        bool axis = false;
        int code = 0;
        std::string name;  // keyName / mouseObjectName
        float scale = 1;
        std::vector<std::pair<int, std::string>> actions;  // id, name
    };
    std::vector<Trigger> triggers(bool mouse) const;

    // Device tracking (StartDeviceTrack...): key and mouse button presses
    // are queued until taken.
    struct Tracked {
        bool mouse = false;
        int code = 0;  // scan code or mouse button
    };
    void startTrack(bool keyboard, bool mouse)
    {
        m_trackKeyboard = keyboard;
        m_trackMouse = mouse;
        m_tracked.clear();
    }
    void endTrack() { startTrack(false, false); }
    bool trackingMouse() const { return m_trackMouse; }
    std::vector<Tracked> takeTracked() { return std::move(m_tracked); }

private:
    enum Device { Keyboard, Mouse };
    struct Binding {
        Device device = Keyboard;
        bool axis = false;
        int code = 0;     // scan code, mouse button, or axis 0..2
        int action = 0;
        std::string object;  // as written in the config (for scale lines)
    };
    static bool parseObject(Device dev, const std::string &obj, bool *axis, int *code);
    void setScale(Device dev, const std::string &obj, float scale);

    std::map<std::string, int> m_actions;  // lower-case name -> id
    std::map<std::string, std::string> m_actionNames;  // lower-case name -> as written
    std::map<int, std::string> m_scaleObjects;  // mouse axis -> object name as written
    // Bindings for devices we don't read yet (joysticks), kept to write back.
    std::vector<std::vector<std::string>> m_otherLines;
    std::vector<Binding> m_bindings;
    std::map<int, float> m_axisScale;      // mouse axis -> scale
    bool m_keys[256] = {};
    bool m_trackKeyboard = false, m_trackMouse = false;
    std::vector<Tracked> m_tracked;
    bool m_buttons[8] = {};
    float m_delta[3] = {};
};

// Windows virtual-key code -> DirectInput scan code (0 if unknown).
int scanCodeFromVK(int vk);

}  // namespace shogo

#endif
