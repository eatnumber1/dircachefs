// MainProtocolEvents() for the testonly checking build of the daemon
// (//dcfs:main_static_checked): the invariant checker and the cost counter
// (testonly/checking_observers.h). See dcfs/protocol_events.h.

#include "dcfs/protocol_events.h"
#include "dcfs/testonly/checking_observers.h"

namespace dcfs {

ProtocolEvents &MainProtocolEvents() { return testonly::CheckingObservers(); }

}  // namespace dcfs
