// MainProtocolEvents() for production binaries: no events are recorded. The
// testonly recording build links dcfs/testonly/main_recorder.cc instead (see
// dcfs/protocol_events.h).

#include "dcfs/protocol_events.h"

namespace dcfs {

ProtocolEvents &MainProtocolEvents() { return NoProtocolEvents(); }

}  // namespace dcfs
