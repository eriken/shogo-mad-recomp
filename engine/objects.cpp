#include "engine/objects.h"

#include "engine/messages.h"

#include <algorithm>

namespace shogo {

EngineObject *ObjectTable::create(bool clientOnly)
{
    auto o = std::make_unique<EngineObject>();
    o->id = m_nextId++;
    o->clientOnly = clientOnly;
    o->scale = DVector(1, 1, 1);
    EngineObject *p = o.get();
    m_objects.push_back(std::move(o));
    m_order.push_back(p);
    return p;
}

void ObjectTable::remove(EngineObject *o)
{
    objectRemoved(o);
    m_order.erase(std::remove(m_order.begin(), m_order.end(), o), m_order.end());
    for (auto it = m_objects.begin(); it != m_objects.end(); ++it)
        if (it->get() == o) {
            o->magic = 0;
            m_objects.erase(it);
            return;
        }
}

EngineObject *ObjectTable::find(uint32_t id) const
{
    if (!id) return nullptr;
    for (EngineObject *o : m_order)
        if (o->id == id) return o;
    return nullptr;
}

void ObjectTable::clear()
{
    for (auto &o : m_objects) o->magic = 0;
    m_objects.clear();
    m_order.clear();
}

ObjectTable &objects()
{
    static ObjectTable table;
    return table;
}

uint32_t objectId(EngineObject *obj) { return (obj && obj->magic == EngineObject::kMagic) ? obj->id : 0; }
EngineObject *objectFromId(uint32_t id) { return objects().find(id); }

}  // namespace shogo
