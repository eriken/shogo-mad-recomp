#include "engine/input.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
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
    if (dev == Keyboard) {
        *code = keyCode(obj);
        return *code != 0;
    }
    // Mouse objects are named by DirectInput, in the system's language
    // ("X Axis", "Oś X", "Button 0", "Przycisk 0"): go by the last character.
    std::string s = obj;
    while (!s.empty() && isspace((unsigned char)s.back())) s.pop_back();
    if (s.empty()) return false;
    static const char *const buttons[3] = {"left button", "right button", "middle button"};  // mouseObjectName
    for (int i = 0; i < 3; ++i)
        if (lower(s) == buttons[i]) {
            *code = i;
            return true;
        }
    char last = (char)toupper((unsigned char)s.back());
    std::string l = lower(s);
    if (l == "x-axis" || l == "y-axis" || l == "z-axis" || l == "x axis" || l == "y axis" || l == "z axis") last = (char)toupper((unsigned char)l[0]);
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
    if (dev == Mouse && parseObject(dev, obj, &axis, &code) && axis) {
        m_axisScale[code] = scale;
        m_scaleObjects[code] = obj;
    }
}

bool Input::command(const std::vector<std::string> &a)
{
    if (a.empty()) return false;
    std::string cmd = lower(a[0]);
    if (cmd == "addaction" && a.size() >= 3) {
        m_actions[lower(a[1])] = atoi(a[2].c_str());
        m_actionNames[lower(a[1])] = a[1];
        return true;
    }
    if (cmd == "enabledevice") return true;
    if ((cmd == "rangebind" || cmd == "bind") && a.size() >= 4) {
        Device dev = isDevice(a[1], "##mouse") ? Mouse : Keyboard;
        if (a[1].empty()) return true;  // a device GetDeviceName didn't name
        Binding b;
        b.device = dev;
        b.object = a[2];
        bool parsed = (isDevice(a[1], "##mouse") || isDevice(a[1], "##keyboard")) && parseObject(dev, a[2], &b.axis, &b.code);
        if (parsed && dev == Keyboard) b.object = "##" + std::to_string(b.code);  // key names are written as scan codes
        if (!parsed) {
            // Joysticks, and keys by name ("1"): not read yet, but kept for
            // autoexec.cfg; the last binding of each object wins.
            m_otherLines.erase(std::remove_if(m_otherLines.begin(), m_otherLines.end(),
                                              [&](const std::vector<std::string> &l) {
                                                  return l.size() >= 3 && lower(l[0]) != "scale" && lower(l[1]) == lower(a[1]) && l[2] == a[2];
                                              }),
                               m_otherLines.end());
            m_otherLines.push_back(a);
            return true;
        }
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
        if (isDevice(a[1], "##mouse") || isDevice(a[1], "##keyboard"))
            setScale(isDevice(a[1], "##mouse") ? Mouse : Keyboard, a[2], (float)atof(a[3].c_str()));
        else
            m_otherLines.push_back(a);
        return true;
    }
    if (cmd == "unbindall" || cmd == "clearbindings") {
        m_bindings.clear();
        m_otherLines.clear();
        return true;
    }
    return false;
}

namespace {
std::string quoted(const std::string &s) { return "\"" + s + "\""; }
}  // namespace

std::string Input::configLines() const
{
    std::string out;
    char buf[64];
    std::map<int, std::string> nameOf;
    for (const auto &[key, id] : m_actions) {
        auto it = m_actionNames.find(key);
        const std::string &name = it == m_actionNames.end() ? key : it->second;
        snprintf(buf, sizeof(buf), " %d\n", id);
        out += "AddAction " + name + buf;
        nameOf[id] = name;
    }
    for (Device dev : {Keyboard, Mouse}) {
        const char *devName = dev == Keyboard ? "##keyboard" : "##mouse";
        out += std::string("enabledevice ") + quoted(devName) + "\n";
        // One rangebind per object, with every action bound to it, in the
        // order the objects were bound.
        std::vector<const Binding *> done;
        for (const Binding &b : m_bindings) {
            if (b.device != dev) continue;
            bool seen = false;
            for (const Binding *d : done) seen |= d->axis == b.axis && d->code == b.code;
            if (seen) continue;
            done.push_back(&b);
            out += std::string("rangebind ") + quoted(devName) + " " + quoted(b.object);
            for (const Binding &o : m_bindings)
                if (o.device == dev && o.axis == b.axis && o.code == b.code && nameOf.count(o.action))
                    out += " 0.000000 0.000000 " + quoted(nameOf[o.action]);
            out += "\n";
            if (dev == Mouse && b.axis && m_axisScale.count(b.code)) {
                auto it = m_scaleObjects.find(b.code);
                snprintf(buf, sizeof(buf), " %f\n", m_axisScale.at(b.code));
                out += std::string("scale ") + quoted(devName) + " " + quoted(it == m_scaleObjects.end() ? b.object : it->second) + buf;
            }
        }
    }
    for (const std::vector<std::string> &line : m_otherLines) {
        out += line[0];
        for (size_t i = 1; i < line.size(); ++i) out += " " + quoted(line[i]);
        out += "\n";
    }
    return out;
}

namespace {
struct KeyNameEntry {
    int dik;
    const char *name;
};
// DirectInput's English key names.
const KeyNameEntry kKeyNames[] = {
    {0x01, "Esc"}, {0x02, "1"}, {0x03, "2"}, {0x04, "3"}, {0x05, "4"}, {0x06, "5"}, {0x07, "6"}, {0x08, "7"},
    {0x09, "8"}, {0x0A, "9"}, {0x0B, "0"}, {0x0C, "-"}, {0x0D, "="}, {0x0E, "Backspace"}, {0x0F, "Tab"},
    {0x10, "Q"}, {0x11, "W"}, {0x12, "E"}, {0x13, "R"}, {0x14, "T"}, {0x15, "Y"}, {0x16, "U"}, {0x17, "I"},
    {0x18, "O"}, {0x19, "P"}, {0x1A, "["}, {0x1B, "]"}, {0x1C, "Enter"}, {0x1D, "Ctrl"}, {0x1E, "A"},
    {0x1F, "S"}, {0x20, "D"}, {0x21, "F"}, {0x22, "G"}, {0x23, "H"}, {0x24, "J"}, {0x25, "K"}, {0x26, "L"},
    {0x27, ";"}, {0x28, "'"}, {0x29, "`"}, {0x2A, "Shift"}, {0x2B, "\\"}, {0x2C, "Z"}, {0x2D, "X"},
    {0x2E, "C"}, {0x2F, "V"}, {0x30, "B"}, {0x31, "N"}, {0x32, "M"}, {0x33, ","}, {0x34, "."}, {0x35, "/"},
    {0x36, "Right Shift"}, {0x37, "Num *"}, {0x38, "Alt"}, {0x39, "Space"}, {0x3A, "Caps Lock"},
    {0x3B, "F1"}, {0x3C, "F2"}, {0x3D, "F3"}, {0x3E, "F4"}, {0x3F, "F5"}, {0x40, "F6"}, {0x41, "F7"},
    {0x42, "F8"}, {0x43, "F9"}, {0x44, "F10"}, {0x45, "Num Lock"}, {0x46, "Scroll Lock"}, {0x47, "Num 7"},
    {0x48, "Num 8"}, {0x49, "Num 9"}, {0x4A, "Num -"}, {0x4B, "Num 4"}, {0x4C, "Num 5"}, {0x4D, "Num 6"},
    {0x4E, "Num +"}, {0x4F, "Num 1"}, {0x50, "Num 2"}, {0x51, "Num 3"}, {0x52, "Num 0"}, {0x53, "Num Del"},
    {0x57, "F11"}, {0x58, "F12"}, {0x9C, "Num Enter"}, {0x9D, "Right Ctrl"}, {0xB5, "Num /"},
    {0xB7, "Prnt Scrn"}, {0xB8, "Right Alt"}, {0xC7, "Home"}, {0xC8, "Up"}, {0xC9, "Page Up"}, {0xCB, "Left"},
    {0xCD, "Right"}, {0xCF, "End"}, {0xD0, "Down"}, {0xD1, "Page Down"}, {0xD2, "Insert"}, {0xD3, "Delete"},
};
}  // namespace

std::string Input::keyName(int dik)
{
    for (const KeyNameEntry &k : kKeyNames)
        if (k.dik == dik) return k.name;
    return "";
}

int Input::keyCode(const std::string &name)
{
    std::string l = lower(name);
    for (const KeyNameEntry &k : kKeyNames)
        if (lower(k.name) == l) return k.dik;
    return 0;
}

std::string Input::mouseObjectName(bool axis, int code)
{
    if (axis) return std::string(1, char('X' + code)) + " Axis";
    static const char *const buttons[3] = {"Left Button", "Right Button", "Middle Button"};
    if (code >= 0 && code < 3) return buttons[code];
    return "Button " + std::to_string(code);
}

std::vector<Input::Trigger> Input::triggers(bool mouse) const
{
    std::map<int, std::string> nameOf;
    for (const auto &[key, id] : m_actions) {
        auto it = m_actionNames.find(key);
        nameOf[id] = it == m_actionNames.end() ? key : it->second;
    }
    std::vector<Trigger> out;
    for (const Binding &b : m_bindings) {
        if ((b.device == Mouse) != mouse) continue;
        Trigger *t = nullptr;
        for (Trigger &o : out)
            if (o.axis == b.axis && o.code == b.code) t = &o;
        if (!t) {
            out.emplace_back();
            t = &out.back();
            t->axis = b.axis;
            t->code = b.code;
            t->name = mouse ? mouseObjectName(b.axis, b.code) : keyName(b.code);
            if (t->name.empty()) t->name = "##" + std::to_string(b.code);
            if (mouse && b.axis && m_axisScale.count(b.code)) t->scale = m_axisScale.at(b.code);
        }
        t->actions.push_back({b.action, nameOf.count(b.action) ? nameOf[b.action] : std::string()});
    }
    return out;
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
