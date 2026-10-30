#include <assert.h>
#include <stddef.h>
#include "terra_types.h"

_Static_assert(offsetof(TxWorld, session) == offsetof(TxWorld, active),
    "TxRuntimeSession must own world identity and handle generation state");
_Static_assert(offsetof(TxWorld, document) == offsetof(TxWorld, workspace_revision),
    "WorldDocument must be embedded at the first persistent document field");
_Static_assert(offsetof(TxWorld, stream_source_id) == offsetof(TxWorld, session.stream_source_id),
    "source lease state must be owned by TxRuntimeSession");
_Static_assert(offsetof(TxWorld, version) == offsetof(TxWorld, document.version),
    "legacy TxWorld version access must alias WorldDocument storage");
_Static_assert(offsetof(TxWorld, section_overrides) == offsetof(TxWorld, document.section_overrides),
    "section overrides must be owned by WorldDocument");

int main(void) {
    TxWorld world = {0};
    world.document.version = 326u;
    world.document.workspace_revision = 7u;
    world.document.worldId = 42;
    world.session.stream_source_id = 19u;
    assert(world.version == 326u);
    assert(world.workspace_revision == 7u);
    assert(world.worldId == 42);
    assert(world.stream_source_id == 19u);

    world.version = 325u;
    world.workspace_revision = 8u;
    assert(world.document.version == 325u);
    assert(world.document.workspace_revision == 8u);
    world.stream_source_id = 20u;
    assert(world.session.stream_source_id == 20u);
    return 0;
}
