// Physics() and Common(): the small C++ interfaces ClientDE and ServerDE hand out.
#ifndef SHOGO_ENGINE_INTERFACES_H
#define SHOGO_ENGINE_INTERFACES_H

#include "engine/api.h"

namespace shogo {

void installInterfaces(ClientDE *client);
void installInterfaces(ServerDE *server);

// GetPointStatus: DE_INSIDE if p is in the level's open space, DE_OUTSIDE in
// solid or outside it, DE_NOTINWORLD without a world.
DRESULT pointStatus(const DVector *p);

}  // namespace shogo

#endif
