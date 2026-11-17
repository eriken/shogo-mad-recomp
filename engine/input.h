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

    void keyDown(int dik) { if (dik > 0 && dik < 256) m_keys[dik] = true; }
    void keyUp(int dik) { if (dik > 0 && dik < 256) m_keys[dik] = false; }
    void mouseButton(int button, bool down) { if (button >= 0 && button < 8) m_buttons[button] = down; }
    void mouseMotion(float dx, float dy) { m_delta[0] += dx; m_delta[1] += dy; }
    void mouseWheel(float dz) { m_delta[2] += dz; }

    bool enabled = true;  // SetInputState
    bool isCommandOn(int action) const;
    // Fills offsets[3] with the mouse movement since the last call, scaled.
    void axisOffsets(float *offsets);
    void clear();

    int actionId(const std::string &name) const;
    size_t bindingCount() const { return m_bindings.size(); }

    // All commands that are on now (for OnCommandOn/Off edges).
    std::vector<int> activeCommands() const;

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
    std::vector<Binding> m_bindings;
    std::map<int, float> m_axisScale;      // mouse axis -> scale
    bool m_keys[256] = {};
    bool m_buttons[8] = {};
    float m_delta[3] = {};
};

// Windows virtual-key code -> DirectInput scan code (0 if unknown).
int scanCodeFromVK(int vk);

}  // namespace shogo

#endif
