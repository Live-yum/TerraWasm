#include <assert.h>
#include <stddef.h>
#include "terra_types.h"

_Static_assert(offsetof(TxWorld, document) == offsetof(TxWorld, workspace_revision),
    "WorldDocument must be embedded at the first persistent document field");
_Static_assert(offsetof(TxWorld, version) == offsetof(TxWorld, document.version),
    "legacy TxWorld version access must alias WorldDocument storage");
_Static_assert(offsetof(TxWorld, section_overrides) == offsetof(TxWorld, document.section_overrides),
    "section overrides must be owned by WorldDocument");

int main(void) {
    TxWorld world = {0};
    world.document.version = 326u;
    world.document.workspace_revision = 7u;
    world.document.worldId = 42;
    assert(world.version == 326u);
    assert(world.workspace_revision == 7u);
    assert(world.worldId == 42);

    world.version = 325u;
    world.workspace_revision = 8u;
    assert(world.document.version == 325u);
    assert(world.document.workspace_revision == 8u);
    return 0;
}
