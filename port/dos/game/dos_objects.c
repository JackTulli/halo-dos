/*
DOS_OBJECTS.C

The node matrices of an object that has not changed (objects.c's
object_compute_node_matrices, HALO_DOS): every tick the game works out
every updated object's nodes from its position, orientation, scale,
animation and function values, though most of a map's objects lie still
(weapons and grenades on the ground, scenery, equipment): half the
objects' update, the profile said, on a 100 MHz Pentium. Here an object of
those kinds keeps the matrices it has while none of what they come from
has changed since they were worked out:

- its kind is one whose nodes nothing else moves (no unit, machine,
  control or projectile: their own code turns their nodes each tick) and
  nothing post-processes (only units do), or it is a vehicle at rest with
  nobody in it;
- it is attached to nothing (a held weapon moves with its holder's nodes);
- it does not animate on its own clock, nor blend between animations;
- its position, forward, up, scale, animation and outgoing function values
  (overlay animations follow them) are what they were.
*/

#include "cseries.h"

#include "objects/objects.h"
#include "objects/object_types.h"

#include <string.h>

#define NODE_SNAPSHOTS 4096

struct node_snapshot
{
	long object_index;
	long definition_index;
	unsigned long flags;
	real_point3d position;
	real_vector3d forward, up;
	real scale;
	struct object_animation_state animation;
	real functions[NUMBER_OF_OUTGOING_OBJECT_FUNCTIONS];
	boolean valid;
};

static struct node_snapshot snapshots[NODE_SNAPSHOTS];

#define CACHED_TYPES (_object_mask_weapon | _object_mask_equipment | _object_mask_garbage | _object_mask_scenery | \
	_object_mask_light_fixture | _object_mask_placeholder | _object_mask_sound_scenery)

static void take(struct node_snapshot *snapshot, long object_index, const struct object_datum *object)
{
	snapshot->object_index = object_index;
	snapshot->definition_index = object->definition_index;
	snapshot->flags = object->object.flags;
	snapshot->position = object->object.position;
	snapshot->forward = object->object.forward;
	snapshot->up = object->object.up;
	snapshot->scale = object->object.scale;
	snapshot->animation = object->object.animation;
	memcpy(snapshot->functions, object->object.outgoing_function_values, sizeof(snapshot->functions));
	snapshot->valid = TRUE;
}

/* TRUE: the object's node matrices are those of its state now (its
snapshot matches); FALSE: work them out (and its snapshot is taken now, for
the next time) */
boolean dos_node_matrices_current(long object_index)
{
	struct object_datum *object = object_get(object_index);
	struct node_snapshot *snapshot = &snapshots[(unsigned long)object_index % NODE_SNAPSHOTS];
	struct node_snapshot now;

	/* (a vehicle too while it is at rest and empty: its wheels and turret
	stay as they are) */
	boolean resting_vehicle = object->object.type == _object_type_vehicle &&
		TEST_FLAG(object->object.flags, _object_at_rest_bit) && object->object.first_child_object_index == NONE;

	if ((!(FLAG(object->object.type) & CACHED_TYPES) && !resting_vehicle) || object->object.parent_object_index != NONE ||
		TEST_FLAG(object->object.flags, _object_animates_automatically_bit) ||
		object->object.animation.interpolation_frame_count)
	{
		if (snapshot->object_index == object_index)
			snapshot->valid = FALSE;
		return FALSE;
	}
	memset(&now, 0, sizeof(now));
	take(&now, object_index, object);
	if (snapshot->valid && !memcmp(snapshot, &now, sizeof(now)))
		return TRUE;
	*snapshot = now;
	return FALSE;
}

/* a new map: no snapshot stands (its objects' indices start over) */
void dos_node_matrices_forget(void)
{
	memset(snapshots, 0, sizeof(snapshots));
}
