#pragma once

namespace relink::ledger {
// Uses only a QTemporaryDir; never opens the user's configuration or ledger.
int runStorageSelfTest();
}
