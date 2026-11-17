#include "engine/input.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstdint>
#include <cstring>

namespace shogo {

namespace {
std::string lower(std::string s)
{
    for (char &c : s) c = (char)tolower((unsigned char)c);
    return s;
}
bool isDevice(const std::string &s, const char *name) { return lower(s) == name; }
}  // namespace

bool Input::parseObject(Device dev, const std::string &obj, bool *axis, int *code)
{
    *axis = false;
    if (obj.size() > 2 && obj[0] == '#' && obj[1] == '#') {
        *code = atoi(obj.c_str() + 2);
        return true;
    }
    if (dev == Keyboard) return false;
    // Mouse objects are named by DirectInput, in the system's language
    // ("X Axis", "Oś X", "Button 0", "Przycisk 0"): go by the last character.
    std::string s = obj;
    while (!s.empty() && isspace((unsigned char)s.back())) s.pop_back();
    if (s.empty()) return false;
    char last = (char)toupper((unsigned char)s.back());
    std::string l = lower(s);
    if (l == "x-axis" || l == "y-axis" || l == "z-axis") last = (char)toupper((unsigned char)l[0]);
    if (last == 'X' || last == 'Y' || last == 'Z') {
        *axis = true;
        *code = last - 'X';
        return true;
    }
    if (isdigit((unsigned char)last)) {
        size_t i = s.size();
        while (i > 0 && isdigit((unsigned char)s[i - 1])) --i;
        *code = atoi(s.c_str() + i);
        return true;
    }
    return false;
}

void Input::setScale(Device dev, const std::string &obj, float scale)
{
    bool axis = false;
    int code = 0;
    if (dev == Mouse && parseObject(dev, obj, &axis, &code) && axis) m_axisScale[code] = scale;
}

bool Input::command(const std::vector<std::string> &a)
{
    if (a.empty()) return false;
    std::string cmd = lower(a[0]);
    if (cmd == "addaction" && a.size() >= 3) {
        m_actions[lower(a[1])] = atoi(a[2].c_str());
        return true;
    }
    if (cmd == "enabledevice") return true;
    if ((cmd == "rangebind" || cmd == "bind") && a.size() >= 4) {
        Device dev = isDevice(a[1], "##mouse") ? Mouse : Keyboard;
        if (!isDevice(a[1], "##mouse") && !isDevice(a[1], "##keyboard")) return true;  // joysticks: not yet
        Binding b;
        b.device = dev;
        b.object = a[2];
        if (!parseObject(dev, a[2], &b.axis, &b.code)) return true;
        // Remove what this object was bound to before.
        m_bindings.erase(std::remove_if(m_bindings.begin(), m_bindings.end(),
                                        [&](const Binding &o) { return o.device == dev && o.axis == b.axis && o.code == b.code; }),
                         m_bindings.end());
        // rangebind dev obj min max action [min max action ...]; bind dev obj action [action ...]
        size_t first = cmd == "rangebind" ? 5 : 3, step = cmd == "rangebind" ? 3 : 1;
        for (size_t i = first; i < a.size(); i += step) {
            int id = actionId(a[i]);
            if (id == INT32_MIN) continue;
            b.action = id;
            m_bindings.push_back(b);
        }
        return true;
    }
    if ((cmd == "unbind" || cmd == "rangeunbind") && a.size() >= 3) {
        Device dev = isDevice(a[1], "##mouse") ? Mouse : Keyboard;
        bool axis = false;
        int code = 0;
        if (parseObject(dev, a[2], &axis, &code))
            m_bindings.erase(std::remove_if(m_bindings.begin(), m_bindings.end(),
                                            [&](const Binding &o) { return o.device == dev && o.axis == axis && o.code == code; }),
                             m_bindings.end());
        return true;
    }
    if (cmd == "scale" && a.size() >= 4) {
        setScale(isDevice(a[1], "##mouse") ? Mouse : Keyboard, a[2], (float)atof(a[3].c_str()));
        return true;
    }
    if (cmd == "unbindall" || cmd == "clearbindings") {
        m_bindings.clear();
        return true;
    }
    return false;
}

int Input::actionId(const std::string &name) const
{
    auto it = m_actions.find(lower(name));
    return it == m_actions.end() ? INT32_MIN : it->second;
}

bool Input::isCommandOn(int action) const
{
    if (!enabled) return false;
    for (const Binding &b : m_bindings) {
        if (b.action != action || b.axis) continue;
        if (b.device == Keyboard ? m_keys[b.code & 255] : (b.code < 8 && m_buttons[b.code])) return true;
    }
    return false;
}

std::vector<int> Input::activeCommands() const
{
    std::vector<int> out;
    if (!enabled) return out;
    for (const Binding &b : m_bindings) {
        if (b.axis || b.action < 0) continue;
        bool on = b.device == Keyboard ? m_keys[b.code & 255] : (b.code < 8 && m_buttons[b.code]);
        if (on && std::find(out.begin(), out.end(), b.action) == out.end()) out.push_back(b.action);
    }
    return out;
}

void Input::axisOffsets(float *offsets)
{
    if (!offsets) return;
    offsets[0] = offsets[1] = offsets[2] = 0;
    if (enabled)
        for (const Binding &b : m_bindings) {
            // Axis actions have negative ids: Axis1 = -1 fills offsets[0].
            if (!b.axis || b.device != Mouse || b.action >= 0 || b.action < -3) continue;
            auto s = m_axisScale.find(b.code);
            offsets[-b.action - 1] += m_delta[b.code] * (s == m_axisScale.end() ? 1.0f : s->second);
        }
    m_delta[0] = m_delta[1] = m_delta[2] = 0;
}

void Input::clear()
{
    memset(m_keys, 0, sizeof(m_keys));
    memset(m_buttons, 0, sizeof(m_buttons));
    m_delta[0] = m_delta[1] = m_delta[2] = 0;
}

int scanCodeFromVK(int vk)
{
    if (vk >= 'A' && vk <= 'Z') {
        static const unsigned char letters[26] = {0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,
                                                  0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C};
        return letters[vk - 'A'];
    }
    if (vk >= '1' && vk <= '9') return 0x02 + (vk - '1');
    if (vk == '0') return 0x0B;
    if (vk >= 0x70 && vk <= 0x79) return 0x3B + (vk - 0x70);  // F1-F10
    switch (vk) {
    case 0x7A: return 0x57;  // F11
    case 0x7B: return 0x58;  // F12
    case 0x1B: return 0x01;  // escape
    case 0x08: return 0x0E;  // backspace
    case 0x09: return 0x0F;  // tab
    case 0x0D: return 0x1C;  // return
    case 0x10: return 0x2A;  // shift
    case 0x11: return 0x1D;  // control
    case 0x12: return 0x38;  // alt
    case 0x14: return 0x3A;  // caps lock
    case 0x20: return 0x39;  // space
    case 0x21: return 0xC9;  // page up
    case 0x22: return 0xD1;  // page down
    case 0x23: return 0xCF;  // end
    case 0x24: return 0xC7;  // home
    case 0x25: return 0xCB;  // left
    case 0x26: return 0xC8;  // up
    case 0x27: return 0xCD;  // right
    case 0x28: return 0xD0;  // down
    case 0x2D: return 0xD2;  // insert
    case 0x2E: return 0xD3;  // delete
    case 0xBA: return 0x27;  // ;
    case 0xBB: return 0x0D;  // =
    case 0xBC: return 0x33;  // ,
    case 0xBD: return 0x0C;  // -
    case 0xBE: return 0x34;  // .
    case 0xBF: return 0x35;  // /
    case 0xC0: return 0x29;  // `
    case 0xDB: return 0x1A;  // [
    case 0xDC: return 0x2B;  // backslash
    case 0xDD: return 0x1B;  // ]
    case 0xDE: return 0x28;  // '
    }
    return 0;
}

}  // namespace shogo
