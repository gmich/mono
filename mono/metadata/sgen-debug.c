/*
 * sgen-debug.c: Collector debugging
 *
 * Author:
 * 	Paolo Molaro (lupus@ximian.com)
 *  Rodrigo Kumpera (kumpera@gmail.com)
 *
 * Copyright 2005-2011 Novell, Inc (http://www.novell.com)
 * Copyright 2011 Xamarin Inc (http://www.xamarin.com)
 * Copyright 2011 Xamarin, Inc.
 * Copyright (C) 2012 Xamarin Inc
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License 2.0 as published by the Free Software Foundation;
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License 2.0 along with this library; if not, write to the Free
 * Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include "config.h"
#ifdef HAVE_SGEN_GC

#include <string.h>

#include "mono/metadata/sgen-gc.h"
#include "mono/metadata/sgen-cardtable.h"
#include "mono/metadata/sgen-protocol.h"
#include "mono/metadata/sgen-memory-governor.h"
#include "mono/metadata/sgen-pinning.h"
#include "mono/metadata/sgen-client.h"
#ifndef SGEN_WITHOUT_MONO
#include "mono/metadata/sgen-bridge-internal.h"
#endif

#define LOAD_VTABLE	SGEN_LOAD_VTABLE

#define object_is_forwarded	SGEN_OBJECT_IS_FORWARDED
#define object_is_pinned	SGEN_OBJECT_IS_PINNED
#define safe_object_get_size	sgen_safe_object_get_size

void describe_ptr (char *ptr);
void check_object (char *start);

/*
 * ######################################################################
 * ########  Collector debugging
 * ######################################################################
 */

static const char*descriptor_types [] = {
	"INVALID",
	"run length",
	"bitmap",
	"small pointer-free",
	"complex",
	"vector",
	"complex arrray",
	"complex pointer-free"
};

static char* describe_nursery_ptr (char *ptr, gboolean need_setup);

static void
describe_pointer (char *ptr, gboolean need_setup)
{
	GCVTable *vtable;
	mword desc;
	int type;
	char *start;
	char *forwarded;
	mword size;

 restart:
	if (sgen_ptr_in_nursery (ptr)) {
		start = describe_nursery_ptr (ptr, need_setup);
		if (!start)
			return;
		ptr = start;
		vtable = (GCVTable*)LOAD_VTABLE (ptr);
	} else {
		if (sgen_ptr_is_in_los (ptr, &start)) {
			if (ptr == start)
				printf ("Pointer is the start of object %p in LOS space.\n", start);
			else
				printf ("Pointer is at offset 0x%x of object %p in LOS space.\n", (int)(ptr - start), start);
			ptr = start;
			mono_sgen_los_describe_pointer (ptr);
			vtable = (GCVTable*)LOAD_VTABLE (ptr);
		} else if (major_collector.ptr_is_in_non_pinned_space (ptr, &start)) {
			if (ptr == start)
				printf ("Pointer is the start of object %p in oldspace.\n", start);
			else if (start)
				printf ("Pointer is at offset 0x%x of object %p in oldspace.\n", (int)(ptr - start), start);
			else
				printf ("Pointer inside oldspace.\n");
			if (start)
				ptr = start;
			vtable = (GCVTable*)major_collector.describe_pointer (ptr);
		} else if (major_collector.obj_is_from_pinned_alloc (ptr)) {
			// FIXME: Handle pointers to the inside of objects
			printf ("Pointer is inside a pinned chunk.\n");
			vtable = (GCVTable*)LOAD_VTABLE (ptr);
		} else {
			printf ("Pointer unknown.\n");
			return;
		}
	}

	if (object_is_pinned (ptr))
		printf ("Object is pinned.\n");

	if ((forwarded = object_is_forwarded (ptr))) {
		printf ("Object is forwarded to %p:\n", forwarded);
		ptr = forwarded;
		goto restart;
	}

	printf ("VTable: %p\n", vtable);
	if (vtable == NULL) {
		printf ("VTable is invalid (empty).\n");
		goto bridge;
	}
	if (sgen_ptr_in_nursery (vtable)) {
		printf ("VTable is invalid (points inside nursery).\n");
		goto bridge;
	}
	printf ("Class: %s.%s\n", sgen_client_vtable_get_namespace (vtable), sgen_client_vtable_get_name (vtable));

	desc = sgen_vtable_get_descriptor ((GCVTable*)vtable);
	printf ("Descriptor: %lx\n", (long)desc);

	type = desc & DESC_TYPE_MASK;
	printf ("Descriptor type: %d (%s)\n", type, descriptor_types [type]);

	size = sgen_safe_object_get_size ((GCObject*)ptr);
	printf ("Size: %d\n", (int)size);

 bridge:
	;
#ifndef SGEN_WITHOUT_MONO
	sgen_bridge_describe_pointer ((GCObject*)ptr);
#endif
}

void
describe_ptr (char *ptr)
{
	describe_pointer (ptr, TRUE);
}

static gboolean missing_remsets;

/*
 * We let a missing remset slide if the target object is pinned,
 * because the store might have happened but the remset not yet added,
 * but in that case the target must be pinned.  We might theoretically
 * miss some missing remsets this way, but it's very unlikely.
 */
#undef HANDLE_PTR
#define HANDLE_PTR(ptr,obj)	do {	\
	if (*(ptr) && sgen_ptr_in_nursery ((char*)*(ptr))) { \
		if (!sgen_get_remset ()->find_address ((char*)(ptr)) && !sgen_cement_lookup (*(ptr))) { \
			GCVTable *__vt = SGEN_LOAD_VTABLE ((obj));	\
			SGEN_LOG (0, "Oldspace->newspace reference %p at offset %td in object %p (%s.%s) not found in remsets.", *(ptr), (char*)(ptr) - (char*)(obj), (obj), sgen_client_vtable_get_namespace (__vt), sgen_client_vtable_get_name (__vt)); \
			binary_protocol_missing_remset ((obj), __vt, (int) ((char*)(ptr) - (char*)(obj)), *(ptr), (gpointer)LOAD_VTABLE(*(ptr)), object_is_pinned (*(ptr))); \
			if (!object_is_pinned (*(ptr)))								\
				missing_remsets = TRUE;									\
		}																\
	}																	\
	} while (0)

/*
 * Check that each object reference which points into the nursery can
 * be found in the remembered sets.
 */
static void
check_consistency_callback (char *start, size_t size, void *dummy)
{
	GCVTable *vt = (GCVTable*)LOAD_VTABLE (start);
	mword desc = sgen_vtable_get_descriptor ((GCVTable*)vt);
	SGEN_LOG (8, "Scanning object %p, vtable: %p (%s)", start, vt, sgen_client_vtable_get_name (vt));

#include "sgen-scan-object.h"
}

/*
 * Perform consistency check of the heap.
 *
 * Assumes the world is stopped.
 */
void
sgen_check_consistency (void)
{
	// Need to add more checks

	missing_remsets = FALSE;

	SGEN_LOG (1, "Begin heap consistency check...");

	// Check that oldspace->newspace pointers are registered with the collector
	major_collector.iterate_objects (ITERATE_OBJECTS_SWEEP_ALL, (IterateObjectCallbackFunc)check_consistency_callback, NULL);

	sgen_los_iterate_objects ((IterateObjectCallbackFunc)check_consistency_callback, NULL);

	SGEN_LOG (1, "Heap consistency check done.");

	if (!binary_protocol_is_enabled ())
		g_assert (!missing_remsets);
}

static gboolean
is_major_or_los_object_marked (char *obj)
{
	if (sgen_safe_object_get_size ((GCObject*)obj) > SGEN_MAX_SMALL_OBJ_SIZE) {
		return sgen_los_object_is_pinned (obj);
	} else {
		return sgen_get_major_collector ()->is_object_live (obj);
	}
}

#undef HANDLE_PTR
#define HANDLE_PTR(ptr,obj)	do {	\
	if (*(ptr) && !sgen_ptr_in_nursery ((char*)*(ptr)) && !is_major_or_los_object_marked ((char*)*(ptr))) { \
		if (!sgen_get_remset ()->find_address_with_cards (start, cards, (char*)(ptr))) { \
			GCVTable *__vt = SGEN_LOAD_VTABLE ((obj));	\
			SGEN_LOG (0, "major->major reference %p at offset %td in object %p (%s.%s) not found in remsets.", *(ptr), (char*)(ptr) - (char*)(obj), (obj), sgen_client_vtable_get_namespace (__vt), sgen_client_vtable_get_name (__vt)); \
			binary_protocol_missing_remset ((obj), __vt, (int) ((char*)(ptr) - (char*)(obj)), *(ptr), (gpointer)LOAD_VTABLE(*(ptr)), object_is_pinned (*(ptr))); \
			missing_remsets = TRUE;				\
		}																\
	}																	\
	} while (0)

static void
check_mod_union_callback (char *start, size_t size, void *dummy)
{
	gboolean in_los = (gboolean) (size_t) dummy;
	GCVTable *vt = (GCVTable*)LOAD_VTABLE (start);
	mword desc = sgen_vtable_get_descriptor ((GCVTable*)vt);
	guint8 *cards;
	SGEN_LOG (8, "Scanning object %p, vtable: %p (%s)", start, vt, sgen_client_vtable_get_name (vt));

	if (!is_major_or_los_object_marked (start))
		return;

	if (in_los)
		cards = sgen_los_header_for_object (start)->cardtable_mod_union;
	else
		cards = sgen_get_major_collector ()->get_cardtable_mod_union_for_object (start);

	SGEN_ASSERT (0, cards, "we must have mod union for marked major objects");

#include "sgen-scan-object.h"
}

void
sgen_check_mod_union_consistency (void)
{
	missing_remsets = FALSE;

	major_collector.iterate_objects (ITERATE_OBJECTS_ALL, (IterateObjectCallbackFunc)check_mod_union_callback, (void*)FALSE);

	sgen_los_iterate_objects ((IterateObjectCallbackFunc)check_mod_union_callback, (void*)TRUE);

	if (!binary_protocol_is_enabled ())
		g_assert (!missing_remsets);
}

#undef HANDLE_PTR
#define HANDLE_PTR(ptr,obj)	do {					\
		if (*(ptr) && !LOAD_VTABLE (*(ptr)))						\
			g_error ("Could not load vtable for obj %p slot %zd (size %zd)", obj, (char*)ptr - (char*)obj, (size_t)safe_object_get_size ((GCObject*)obj)); \
	} while (0)

static void
check_major_refs_callback (char *start, size_t size, void *dummy)
{
	mword desc = sgen_obj_get_descriptor (start);

#include "sgen-scan-object.h"
}

void
sgen_check_major_refs (void)
{
	major_collector.iterate_objects (ITERATE_OBJECTS_SWEEP_ALL, (IterateObjectCallbackFunc)check_major_refs_callback, NULL);
	sgen_los_iterate_objects ((IterateObjectCallbackFunc)check_major_refs_callback, NULL);
}

/* Check that the reference is valid */
#undef HANDLE_PTR
#define HANDLE_PTR(ptr,obj)	do {	\
		if (*(ptr)) {	\
			g_assert (sgen_client_vtable_get_namespace (SGEN_LOAD_VTABLE_UNCHECKED (*(ptr))));	\
		}	\
	} while (0)

/*
 * check_object:
 *
 *   Perform consistency check on an object. Currently we only check that the
 * reference fields are valid.
 */
void
check_object (char *start)
{
	mword desc;

	if (!start)
		return;

	desc = sgen_obj_get_descriptor (start);

#include "sgen-scan-object.h"
}


static char **valid_nursery_objects;
static int valid_nursery_object_count;
static gboolean broken_heap;

static void 
setup_mono_sgen_scan_area_with_callback (char *object, size_t size, void *data)
{
	valid_nursery_objects [valid_nursery_object_count++] = object;
}

static void
setup_valid_nursery_objects (void)
{
	if (!valid_nursery_objects)
		valid_nursery_objects = sgen_alloc_os_memory (DEFAULT_NURSERY_SIZE, SGEN_ALLOC_INTERNAL | SGEN_ALLOC_ACTIVATE, "debugging data");
	valid_nursery_object_count = 0;
	sgen_scan_area_with_callback (nursery_section->data, nursery_section->end_data, setup_mono_sgen_scan_area_with_callback, NULL, FALSE);
}

static gboolean
find_object_in_nursery_dump (char *object)
{
	int first = 0, last = valid_nursery_object_count;
	while (first < last) {
		int middle = first + ((last - first) >> 1);
		if (object == valid_nursery_objects [middle])
			return TRUE;

		if (object < valid_nursery_objects [middle])
			last = middle;
		else
			first = middle + 1;
	}
	g_assert (first == last);
	return FALSE;
}

static void
iterate_valid_nursery_objects (IterateObjectCallbackFunc callback, void *data)
{
	int i;
	for (i = 0; i < valid_nursery_object_count; ++i) {
		char *obj = valid_nursery_objects [i];
		callback (obj, safe_object_get_size ((GCObject*)obj), data);
	}
}

static char*
describe_nursery_ptr (char *ptr, gboolean need_setup)
{
	int i;

	if (need_setup)
		setup_valid_nursery_objects ();

	for (i = 0; i < valid_nursery_object_count - 1; ++i) {
		if (valid_nursery_objects [i + 1] > ptr)
			break;
	}

	if (i >= valid_nursery_object_count || valid_nursery_objects [i] + safe_object_get_size ((GCObject *)valid_nursery_objects [i]) < ptr) {
		SGEN_LOG (0, "nursery-ptr (unalloc'd-memory)");
		return NULL;
	} else {
		char *obj = valid_nursery_objects [i];
		if (obj == ptr)
			SGEN_LOG (0, "nursery-ptr %p", obj);
		else
			SGEN_LOG (0, "nursery-ptr %p (interior-ptr offset %td)", obj, ptr - obj);
		return obj;
	}
}

static gboolean
is_valid_object_pointer (char *object)
{
	if (sgen_ptr_in_nursery (object))
		return find_object_in_nursery_dump (object);
	
	if (sgen_los_is_valid_object (object))
		return TRUE;

	if (major_collector.is_valid_object (object))
		return TRUE;
	return FALSE;
}

static void
bad_pointer_spew (char *obj, char **slot)
{
	char *ptr = *slot;
	GCVTable *vtable = (GCVTable*)LOAD_VTABLE (obj);

	SGEN_LOG (0, "Invalid object pointer %p at offset %td in object %p (%s.%s):", ptr,
			(char*)slot - obj,
			obj, sgen_client_vtable_get_namespace (vtable), sgen_client_vtable_get_name (vtable));
	describe_pointer (ptr, FALSE);
	broken_heap = TRUE;
}

static void
missing_remset_spew (char *obj, char **slot)
{
	char *ptr = *slot;
	GCVTable *vtable = (GCVTable*)LOAD_VTABLE (obj);

	SGEN_LOG (0, "Oldspace->newspace reference %p at offset %td in object %p (%s.%s) not found in remsets.",
			ptr, (char*)slot - obj, obj, 
			sgen_client_vtable_get_namespace (vtable), sgen_client_vtable_get_name (vtable));

	broken_heap = TRUE;
}

/*
FIXME Flag missing remsets due to pinning as non fatal
*/
#undef HANDLE_PTR
#define HANDLE_PTR(ptr,obj)	do {	\
		if (*(char**)ptr) {	\
			if (!is_valid_object_pointer (*(char**)ptr)) {	\
				bad_pointer_spew ((char*)obj, (char**)ptr);	\
			} else if (!sgen_ptr_in_nursery (obj) && sgen_ptr_in_nursery ((char*)*ptr)) {	\
				if (!sgen_get_remset ()->find_address ((char*)(ptr)) && !sgen_cement_lookup ((char*)*(ptr)) && (!allow_missing_pinned || !SGEN_OBJECT_IS_PINNED ((char*)*(ptr)))) \
			        missing_remset_spew ((char*)obj, (char**)ptr);	\
			}	\
        } \
	} while (0)

static void
verify_object_pointers_callback (char *start, size_t size, void *data)
{
	gboolean allow_missing_pinned = (gboolean) (size_t) data;
	mword desc = sgen_obj_get_descriptor (start);

#include "sgen-scan-object.h"
}

/*
FIXME:
-This heap checker is racy regarding inlined write barriers and other JIT tricks that
depend on OP_DUMMY_USE.
*/
void
sgen_check_whole_heap (gboolean allow_missing_pinned)
{
	setup_valid_nursery_objects ();

	broken_heap = FALSE;
	sgen_scan_area_with_callback (nursery_section->data, nursery_section->end_data, verify_object_pointers_callback, (void*) (size_t) allow_missing_pinned, FALSE);
	major_collector.iterate_objects (ITERATE_OBJECTS_SWEEP_ALL, verify_object_pointers_callback, (void*) (size_t) allow_missing_pinned);
	sgen_los_iterate_objects (verify_object_pointers_callback, (void*) (size_t) allow_missing_pinned);

	g_assert (!broken_heap);
}

static gboolean
ptr_in_heap (char *object)
{
	if (sgen_ptr_in_nursery (object))
		return TRUE;
	
	if (sgen_los_is_valid_object (object))
		return TRUE;

	if (major_collector.is_valid_object (object))
		return TRUE;
	return FALSE;
}

/*
 * sgen_check_objref:
 *   Do consistency checks on the object reference OBJ. Assert on failure.
 */
void
sgen_check_objref (char *obj)
{
	g_assert (ptr_in_heap (obj));
}

static void
find_pinning_ref_from_thread (char *obj, size_t size)
{
#ifndef SGEN_WITHOUT_MONO
	int j;
	SgenThreadInfo *info;
	char *endobj = obj + size;

	FOREACH_THREAD (info) {
		char **start = (char**)info->client_info.stack_start;
		if (info->client_info.skip)
			continue;
		while (start < (char**)info->client_info.stack_end) {
			if (*start >= obj && *start < endobj)
				SGEN_LOG (0, "Object %p referenced in thread %p (id %p) at %p, stack: %p-%p", obj, info, (gpointer)mono_thread_info_get_tid (info), start, info->client_info.stack_start, info->client_info.stack_end);
			start++;
		}

		for (j = 0; j < ARCH_NUM_REGS; ++j) {
#ifdef USE_MONO_CTX
			mword w = ((mword*)&info->client_info.ctx) [j];
#else
			mword w = (mword)&info->client_info.regs [j];
#endif

			if (w >= (mword)obj && w < (mword)obj + size)
				SGEN_LOG (0, "Object %p referenced in saved reg %d of thread %p (id %p)", obj, j, info, (gpointer)mono_thread_info_get_tid (info));
		} END_FOREACH_THREAD
	}
#endif
}

/*
 * Debugging function: find in the conservative roots where @obj is being pinned.
 */
static G_GNUC_UNUSED void
find_pinning_reference (char *obj, size_t size)
{
	char **start;
	RootRecord *root;
	char *endobj = obj + size;

	SGEN_HASH_TABLE_FOREACH (&roots_hash [ROOT_TYPE_NORMAL], start, root) {
		/* if desc is non-null it has precise info */
		if (!root->root_desc) {
			while (start < (char**)root->end_root) {
				if (*start >= obj && *start < endobj) {
					SGEN_LOG (0, "Object %p referenced in pinned roots %p-%p\n", obj, start, root->end_root);
				}
				start++;
			}
		}
	} SGEN_HASH_TABLE_FOREACH_END;

	find_pinning_ref_from_thread (obj, size);
}

#undef HANDLE_PTR
#define HANDLE_PTR(ptr,obj)	do {					\
		char* __target = *(char**)ptr;				\
		if (__target) {						\
			if (sgen_ptr_in_nursery (__target)) {		\
				g_assert (!SGEN_OBJECT_IS_FORWARDED (__target)); \
			} else {					\
				mword __size = sgen_safe_object_get_size ((GCObject*)__target); \
				if (__size <= SGEN_MAX_SMALL_OBJ_SIZE)	\
					g_assert (major_collector.is_object_live (__target)); \
				else					\
					g_assert (sgen_los_object_is_pinned (__target)); \
			}						\
		}							\
	} while (0)

static void
check_marked_callback (char *start, size_t size, void *dummy)
{
	gboolean flag = (gboolean) (size_t) dummy;
	mword desc;

	if (sgen_ptr_in_nursery (start)) {
		if (flag)
			SGEN_ASSERT (0, SGEN_OBJECT_IS_PINNED (start), "All objects remaining in the nursery must be pinned");
	} else if (flag) {
		if (!sgen_los_object_is_pinned (start))
			return;
	} else {
		if (!major_collector.is_object_live (start))
			return;
	}

	desc = sgen_obj_get_descriptor_safe (start);

#include "sgen-scan-object.h"
}

void
sgen_check_heap_marked (gboolean nursery_must_be_pinned)
{
	setup_valid_nursery_objects ();

	iterate_valid_nursery_objects (check_marked_callback, (void*)(size_t)nursery_must_be_pinned);
	major_collector.iterate_objects (ITERATE_OBJECTS_SWEEP_ALL, check_marked_callback, (void*)FALSE);
	sgen_los_iterate_objects (check_marked_callback, (void*)TRUE);
}

static void
check_nursery_objects_pinned_callback (char *obj, size_t size, void *data /* ScanCopyContext *ctx */)
{
	gboolean pinned = (gboolean) (size_t) data;

	g_assert (!SGEN_OBJECT_IS_FORWARDED (obj));
	if (pinned)
		g_assert (SGEN_OBJECT_IS_PINNED (obj));
	else
		g_assert (!SGEN_OBJECT_IS_PINNED (obj));
}

void
sgen_check_nursery_objects_pinned (gboolean pinned)
{
	sgen_clear_nursery_fragments ();
	sgen_scan_area_with_callback (nursery_section->data, nursery_section->end_data,
			(IterateObjectCallbackFunc)check_nursery_objects_pinned_callback, (void*) (size_t) pinned /* (void*)&ctx */, FALSE);
}

static void
verify_scan_starts (char *start, char *end)
{
	size_t i;

	for (i = 0; i < nursery_section->num_scan_start; ++i) {
		char *addr = nursery_section->scan_starts [i];
		if (addr > start && addr < end)
			SGEN_LOG (0, "NFC-BAD SCAN START [%zu] %p for obj [%p %p]", i, addr, start, end);
	}
}

void
sgen_debug_verify_nursery (gboolean do_dump_nursery_content)
{
	char *start, *end, *cur, *hole_start;

	if (nursery_canaries_enabled ())
		SGEN_LOG (0, "Checking nursery canaries...");

	/*This cleans up unused fragments */
	sgen_nursery_allocator_prepare_for_pinning ();

	hole_start = start = cur = sgen_get_nursery_start ();
	end = sgen_get_nursery_end ();

	while (cur < end) {
		size_t ss, size;
		gboolean is_array_fill;

		if (!*(void**)cur) {
			cur += sizeof (void*);
			continue;
		}

		if (object_is_forwarded (cur))
			SGEN_LOG (0, "FORWARDED OBJ %p", cur);
		else if (object_is_pinned (cur))
			SGEN_LOG (0, "PINNED OBJ %p", cur);

		ss = safe_object_get_size ((GCObject*)cur);
		size = SGEN_ALIGN_UP (ss);
		verify_scan_starts (cur, cur + size);
		is_array_fill = sgen_client_object_is_array_fill ((GCObject*)cur);
		if (do_dump_nursery_content) {
			GCVTable *vtable = SGEN_LOAD_VTABLE (cur);
			if (cur > hole_start)
				SGEN_LOG (0, "HOLE [%p %p %d]", hole_start, cur, (int)(cur - hole_start));
			SGEN_LOG (0, "OBJ  [%p %p %d %d %s.%s %d]", cur, cur + size, (int)size, (int)ss,
					sgen_client_vtable_get_namespace (vtable), sgen_client_vtable_get_name (vtable),
					is_array_fill);
		}
		if (nursery_canaries_enabled () && !is_array_fill) {
			CHECK_CANARY_FOR_OBJECT (cur);
			CANARIFY_SIZE (size);
		}
		cur += size;
		hole_start = cur;
	}
}

/*
 * Checks that no objects in the nursery are fowarded or pinned.  This
 * is a precondition to restarting the mutator while doing a
 * concurrent collection.  Note that we don't clear fragments because
 * we depend on that having happened earlier.
 */
void
sgen_debug_check_nursery_is_clean (void)
{
	char *end, *cur;

	cur = sgen_get_nursery_start ();
	end = sgen_get_nursery_end ();

	while (cur < end) {
		size_t size;

		if (!*(void**)cur) {
			cur += sizeof (void*);
			continue;
		}

		g_assert (!object_is_forwarded (cur));
		g_assert (!object_is_pinned (cur));

		size = SGEN_ALIGN_UP (safe_object_get_size ((GCObject*)cur));
		verify_scan_starts (cur, cur + size);

		cur += size;
	}
}

static gboolean scan_object_for_specific_ref_precise = TRUE;

#undef HANDLE_PTR
#define HANDLE_PTR(ptr,obj) do {					\
		if ((GCObject*)*(ptr) == key) {				\
			GCVTable *vtable = SGEN_LOAD_VTABLE (*(ptr));	\
			g_print ("found ref to %p in object %p (%s.%s) at offset %td\n", \
					key, (obj), sgen_client_vtable_get_namespace (vtable), sgen_client_vtable_get_name (vtable), ((char*)(ptr) - (char*)(obj))); \
		}							\
	} while (0)

static void
scan_object_for_specific_ref (char *start, GCObject *key)
{
	char *forwarded;

	if ((forwarded = SGEN_OBJECT_IS_FORWARDED (start)))
		start = forwarded;

	if (scan_object_for_specific_ref_precise) {
		mword desc = sgen_obj_get_descriptor_safe (start);
		#include "sgen-scan-object.h"
	} else {
		mword *words = (mword*)start;
		size_t size = safe_object_get_size ((GCObject*)start);
		int i;
		for (i = 0; i < size / sizeof (mword); ++i) {
			if (words [i] == (mword)key) {
				GCVTable *vtable = SGEN_LOAD_VTABLE (start);
				g_print ("found possible ref to %p in object %p (%s.%s) at offset %td\n",
						key, start, sgen_client_vtable_get_namespace (vtable), sgen_client_vtable_get_name (vtable), i * sizeof (mword));
			}
		}
	}
}

static void
scan_object_for_specific_ref_callback (char *obj, size_t size, GCObject *key)
{
	scan_object_for_specific_ref (obj, key);
}

static void
check_root_obj_specific_ref (RootRecord *root, GCObject *key, GCObject *obj)
{
	if (key != obj)
		return;
	g_print ("found ref to %p in root record %p\n", key, root);
}

static GCObject *check_key = NULL;
static RootRecord *check_root = NULL;

static void
check_root_obj_specific_ref_from_marker (void **obj, void *gc_data)
{
	check_root_obj_specific_ref (check_root, check_key, *obj);
}

static void
scan_roots_for_specific_ref (GCObject *key, int root_type)
{
	void **start_root;
	RootRecord *root;
	check_key = key;

	SGEN_HASH_TABLE_FOREACH (&roots_hash [root_type], start_root, root) {
		mword desc = root->root_desc;

		check_root = root;

		switch (desc & ROOT_DESC_TYPE_MASK) {
		case ROOT_DESC_BITMAP:
			desc >>= ROOT_DESC_TYPE_SHIFT;
			while (desc) {
				if (desc & 1)
					check_root_obj_specific_ref (root, key, *start_root);
				desc >>= 1;
				start_root++;
			}
			return;
		case ROOT_DESC_COMPLEX: {
			gsize *bitmap_data = sgen_get_complex_descriptor_bitmap (desc);
			int bwords = (int) ((*bitmap_data) - 1);
			void **start_run = start_root;
			bitmap_data++;
			while (bwords-- > 0) {
				gsize bmap = *bitmap_data++;
				void **objptr = start_run;
				while (bmap) {
					if (bmap & 1)
						check_root_obj_specific_ref (root, key, *objptr);
					bmap >>= 1;
					++objptr;
				}
				start_run += GC_BITS_PER_WORD;
			}
			break;
		}
		case ROOT_DESC_USER: {
			SgenUserRootMarkFunc marker = sgen_get_user_descriptor_func (desc);
			marker (start_root, check_root_obj_specific_ref_from_marker, NULL);
			break;
		}
		case ROOT_DESC_RUN_LEN:
			g_assert_not_reached ();
		default:
			g_assert_not_reached ();
		}
	} SGEN_HASH_TABLE_FOREACH_END;

	check_key = NULL;
	check_root = NULL;
}

void
mono_gc_scan_for_specific_ref (GCObject *key, gboolean precise)
{
	void **ptr;
	RootRecord *root;

	scan_object_for_specific_ref_precise = precise;

	sgen_scan_area_with_callback (nursery_section->data, nursery_section->end_data,
			(IterateObjectCallbackFunc)scan_object_for_specific_ref_callback, key, TRUE);

	major_collector.iterate_objects (ITERATE_OBJECTS_SWEEP_ALL, (IterateObjectCallbackFunc)scan_object_for_specific_ref_callback, key);

	sgen_los_iterate_objects ((IterateObjectCallbackFunc)scan_object_for_specific_ref_callback, key);

	scan_roots_for_specific_ref (key, ROOT_TYPE_NORMAL);
	scan_roots_for_specific_ref (key, ROOT_TYPE_WBARRIER);

	SGEN_HASH_TABLE_FOREACH (&roots_hash [ROOT_TYPE_PINNED], ptr, root) {
		while (ptr < (void**)root->end_root) {
			check_root_obj_specific_ref (root, *ptr, key);
			++ptr;
		}
	} SGEN_HASH_TABLE_FOREACH_END;
}

#ifndef SGEN_WITHOUT_MONO

static MonoDomain *check_domain = NULL;

static void
check_obj_not_in_domain (MonoObject **o)
{
	g_assert (((*o))->vtable->domain != check_domain);
}


static void
check_obj_not_in_domain_callback (void **o, void *gc_data)
{
	g_assert (((MonoObject*)(*o))->vtable->domain != check_domain);
}

void
sgen_scan_for_registered_roots_in_domain (MonoDomain *domain, int root_type)
{
	void **start_root;
	RootRecord *root;
	check_domain = domain;
	SGEN_HASH_TABLE_FOREACH (&roots_hash [root_type], start_root, root) {
		mword desc = root->root_desc;

		/* The MonoDomain struct is allowed to hold
		   references to objects in its own domain. */
		if (start_root == (void**)domain)
			continue;

		switch (desc & ROOT_DESC_TYPE_MASK) {
		case ROOT_DESC_BITMAP:
			desc >>= ROOT_DESC_TYPE_SHIFT;
			while (desc) {
				if ((desc & 1) && *start_root)
					check_obj_not_in_domain (*start_root);
				desc >>= 1;
				start_root++;
			}
			break;
		case ROOT_DESC_COMPLEX: {
			gsize *bitmap_data = sgen_get_complex_descriptor_bitmap (desc);
			int bwords = (int)((*bitmap_data) - 1);
			void **start_run = start_root;
			bitmap_data++;
			while (bwords-- > 0) {
				gsize bmap = *bitmap_data++;
				void **objptr = start_run;
				while (bmap) {
					if ((bmap & 1) && *objptr)
						check_obj_not_in_domain (*objptr);
					bmap >>= 1;
					++objptr;
				}
				start_run += GC_BITS_PER_WORD;
			}
			break;
		}
		case ROOT_DESC_USER: {
			SgenUserRootMarkFunc marker = sgen_get_user_descriptor_func (desc);
			marker (start_root, check_obj_not_in_domain_callback, NULL);
			break;
		}
		case ROOT_DESC_RUN_LEN:
			g_assert_not_reached ();
		default:
			g_assert_not_reached ();
		}
	} SGEN_HASH_TABLE_FOREACH_END;

	check_domain = NULL;
}

static gboolean
is_xdomain_ref_allowed (gpointer *ptr, char *obj, MonoDomain *domain)
{
	MonoObject *o = (MonoObject*)(obj);
	MonoObject *ref = (MonoObject*)*(ptr);
	size_t offset = (char*)(ptr) - (char*)o;

	if (o->vtable->klass == mono_defaults.thread_class && offset == G_STRUCT_OFFSET (MonoThread, internal_thread))
		return TRUE;
	if (o->vtable->klass == mono_defaults.internal_thread_class && offset == G_STRUCT_OFFSET (MonoInternalThread, current_appcontext))
		return TRUE;

#ifndef DISABLE_REMOTING
	if (mono_defaults.real_proxy_class->supertypes && mono_class_has_parent_fast (o->vtable->klass, mono_defaults.real_proxy_class) &&
			offset == G_STRUCT_OFFSET (MonoRealProxy, unwrapped_server))
		return TRUE;
#endif
	/* Thread.cached_culture_info */
	if (!strcmp (ref->vtable->klass->name_space, "System.Globalization") &&
			!strcmp (ref->vtable->klass->name, "CultureInfo") &&
			!strcmp(o->vtable->klass->name_space, "System") &&
			!strcmp(o->vtable->klass->name, "Object[]"))
		return TRUE;
	/*
	 *  at System.IO.MemoryStream.InternalConstructor (byte[],int,int,bool,bool) [0x0004d] in /home/schani/Work/novell/trunk/mcs/class/corlib/System.IO/MemoryStream.cs:121
	 * at System.IO.MemoryStream..ctor (byte[]) [0x00017] in /home/schani/Work/novell/trunk/mcs/class/corlib/System.IO/MemoryStream.cs:81
	 * at (wrapper remoting-invoke-with-check) System.IO.MemoryStream..ctor (byte[]) <IL 0x00020, 0xffffffff>
	 * at System.Runtime.Remoting.Messaging.CADMethodCallMessage.GetArguments () [0x0000d] in /home/schani/Work/novell/trunk/mcs/class/corlib/System.Runtime.Remoting.Messaging/CADMessages.cs:327
	 * at System.Runtime.Remoting.Messaging.MethodCall..ctor (System.Runtime.Remoting.Messaging.CADMethodCallMessage) [0x00017] in /home/schani/Work/novell/trunk/mcs/class/corlib/System.Runtime.Remoting.Messaging/MethodCall.cs:87
	 * at System.AppDomain.ProcessMessageInDomain (byte[],System.Runtime.Remoting.Messaging.CADMethodCallMessage,byte[]&,System.Runtime.Remoting.Messaging.CADMethodReturnMessage&) [0x00018] in /home/schani/Work/novell/trunk/mcs/class/corlib/System/AppDomain.cs:1213
	 * at (wrapper remoting-invoke-with-check) System.AppDomain.ProcessMessageInDomain (byte[],System.Runtime.Remoting.Messaging.CADMethodCallMessage,byte[]&,System.Runtime.Remoting.Messaging.CADMethodReturnMessage&) <IL 0x0003d, 0xffffffff>
	 * at System.Runtime.Remoting.Channels.CrossAppDomainSink.ProcessMessageInDomain (byte[],System.Runtime.Remoting.Messaging.CADMethodCallMessage) [0x00008] in /home/schani/Work/novell/trunk/mcs/class/corlib/System.Runtime.Remoting.Channels/CrossAppDomainChannel.cs:198
	 * at (wrapper runtime-invoke) object.runtime_invoke_CrossAppDomainSink/ProcessMessageRes_object_object (object,intptr,intptr,intptr) <IL 0x0004c, 0xffffffff>
	 */
	if (!strcmp (ref->vtable->klass->name_space, "System") &&
			!strcmp (ref->vtable->klass->name, "Byte[]") &&
			!strcmp (o->vtable->klass->name_space, "System.IO") &&
			!strcmp (o->vtable->klass->name, "MemoryStream"))
		return TRUE;
	return FALSE;
}

static void
check_reference_for_xdomain (gpointer *ptr, char *obj, MonoDomain *domain)
{
	MonoObject *o = (MonoObject*)(obj);
	MonoObject *ref = (MonoObject*)*(ptr);
	size_t offset = (char*)(ptr) - (char*)o;
	MonoClass *class;
	MonoClassField *field;
	char *str;

	if (!ref || ref->vtable->domain == domain)
		return;
	if (is_xdomain_ref_allowed (ptr, obj, domain))
		return;

	field = NULL;
	for (class = o->vtable->klass; class; class = class->parent) {
		int i;

		for (i = 0; i < class->field.count; ++i) {
			if (class->fields[i].offset == offset) {
				field = &class->fields[i];
				break;
			}
		}
		if (field)
			break;
	}

	if (ref->vtable->klass == mono_defaults.string_class)
		str = mono_string_to_utf8 ((MonoString*)ref);
	else
		str = NULL;
	g_print ("xdomain reference in %p (%s.%s) at offset %d (%s) to %p (%s.%s) (%s)  -  pointed to by:\n",
			o, o->vtable->klass->name_space, o->vtable->klass->name,
			offset, field ? field->name : "",
			ref, ref->vtable->klass->name_space, ref->vtable->klass->name, str ? str : "");
	mono_gc_scan_for_specific_ref (o, TRUE);
	if (str)
		g_free (str);
}

#undef HANDLE_PTR
#define HANDLE_PTR(ptr,obj)	check_reference_for_xdomain ((ptr), (obj), domain)

static void
scan_object_for_xdomain_refs (char *start, mword size, void *data)
{
	MonoVTable *vt = (MonoVTable*)SGEN_LOAD_VTABLE (start);
	MonoDomain *domain = vt->domain;
	mword desc = sgen_vtable_get_descriptor ((GCVTable*)vt);

	#include "sgen-scan-object.h"
}

void
sgen_check_for_xdomain_refs (void)
{
	LOSObject *bigobj;

	sgen_scan_area_with_callback (nursery_section->data, nursery_section->end_data,
			(IterateObjectCallbackFunc)scan_object_for_xdomain_refs, NULL, FALSE);

	major_collector.iterate_objects (ITERATE_OBJECTS_SWEEP_ALL, (IterateObjectCallbackFunc)scan_object_for_xdomain_refs, NULL);

	for (bigobj = los_object_list; bigobj; bigobj = bigobj->next)
		scan_object_for_xdomain_refs (bigobj->data, sgen_los_object_size (bigobj), NULL);
}

#endif

/* If not null, dump the heap after each collection into this file */
static FILE *heap_dump_file = NULL;

void
sgen_dump_occupied (char *start, char *end, char *section_start)
{
	fprintf (heap_dump_file, "<occupied offset=\"%td\" size=\"%td\"/>\n", start - section_start, end - start);
}

void
sgen_dump_section (GCMemSection *section, const char *type)
{
	char *start = section->data;
	char *end = section->data + section->size;
	char *occ_start = NULL;

	fprintf (heap_dump_file, "<section type=\"%s\" size=\"%lu\">\n", type, (unsigned long)section->size);

	while (start < end) {
		guint size;
		//GCVTable *vt;
		//MonoClass *class;

		if (!*(void**)start) {
			if (occ_start) {
				sgen_dump_occupied (occ_start, start, section->data);
				occ_start = NULL;
			}
			start += sizeof (void*); /* should be ALLOC_ALIGN, really */
			continue;
		}
		g_assert (start < section->next_data);

		if (!occ_start)
			occ_start = start;

		//vt = (GCVTable*)SGEN_LOAD_VTABLE (start);
		//class = vt->klass;

		size = SGEN_ALIGN_UP (safe_object_get_size ((GCObject*) start));

		/*
		fprintf (heap_dump_file, "<object offset=\"%d\" class=\"%s.%s\" size=\"%d\"/>\n",
				start - section->data,
				vt->klass->name_space, vt->klass->name,
				size);
		*/

		start += size;
	}
	if (occ_start)
		sgen_dump_occupied (occ_start, start, section->data);

	fprintf (heap_dump_file, "</section>\n");
}

static void
dump_object (GCObject *obj, gboolean dump_location)
{
#ifndef SGEN_WITHOUT_MONO
	static char class_name [1024];

	MonoClass *class = mono_object_class (obj);
	int i, j;

	/*
	 * Python's XML parser is too stupid to parse angle brackets
	 * in strings, so we just ignore them;
	 */
	i = j = 0;
	while (class->name [i] && j < sizeof (class_name) - 1) {
		if (!strchr ("<>\"", class->name [i]))
			class_name [j++] = class->name [i];
		++i;
	}
	g_assert (j < sizeof (class_name));
	class_name [j] = 0;

	fprintf (heap_dump_file, "<object class=\"%s.%s\" size=\"%zd\"",
			class->name_space, class_name,
			safe_object_get_size (obj));
	if (dump_location) {
		const char *location;
		if (sgen_ptr_in_nursery (obj))
			location = "nursery";
		else if (safe_object_get_size (obj) <= SGEN_MAX_SMALL_OBJ_SIZE)
			location = "major";
		else
			location = "LOS";
		fprintf (heap_dump_file, " location=\"%s\"", location);
	}
	fprintf (heap_dump_file, "/>\n");
#endif
}

void
sgen_debug_enable_heap_dump (const char *filename)
{
	heap_dump_file = fopen (filename, "w");
	if (heap_dump_file) {
		fprintf (heap_dump_file, "<sgen-dump>\n");
		sgen_pin_stats_enable ();
	}
}

void
sgen_debug_dump_heap (const char *type, int num, const char *reason)
{
	SgenPointerQueue *pinned_objects;
	LOSObject *bigobj;
	int i;

	if (!heap_dump_file)
		return;

	fprintf (heap_dump_file, "<collection type=\"%s\" num=\"%d\"", type, num);
	if (reason)
		fprintf (heap_dump_file, " reason=\"%s\"", reason);
	fprintf (heap_dump_file, ">\n");
#ifndef SGEN_WITHOUT_MONO
	fprintf (heap_dump_file, "<other-mem-usage type=\"mempools\" size=\"%ld\"/>\n", mono_mempool_get_bytes_allocated ());
#endif
	sgen_dump_internal_mem_usage (heap_dump_file);
	fprintf (heap_dump_file, "<pinned type=\"stack\" bytes=\"%zu\"/>\n", sgen_pin_stats_get_pinned_byte_count (PIN_TYPE_STACK));
	/* fprintf (heap_dump_file, "<pinned type=\"static-data\" bytes=\"%d\"/>\n", pinned_byte_counts [PIN_TYPE_STATIC_DATA]); */
	fprintf (heap_dump_file, "<pinned type=\"other\" bytes=\"%zu\"/>\n", sgen_pin_stats_get_pinned_byte_count (PIN_TYPE_OTHER));

	fprintf (heap_dump_file, "<pinned-objects>\n");
	pinned_objects = sgen_pin_stats_get_object_list ();
	for (i = 0; i < pinned_objects->next_slot; ++i)
		dump_object (pinned_objects->data [i], TRUE);
	fprintf (heap_dump_file, "</pinned-objects>\n");

	sgen_dump_section (nursery_section, "nursery");

	major_collector.dump_heap (heap_dump_file);

	fprintf (heap_dump_file, "<los>\n");
	for (bigobj = los_object_list; bigobj; bigobj = bigobj->next)
		dump_object ((GCObject*)bigobj->data, FALSE);
	fprintf (heap_dump_file, "</los>\n");

	fprintf (heap_dump_file, "</collection>\n");
}

static char *found_obj;

static void
find_object_for_ptr_callback (char *obj, size_t size, void *user_data)
{
	char *ptr = user_data;

	if (ptr >= obj && ptr < obj + size) {
		g_assert (!found_obj);
		found_obj = obj;
	}
}

/* for use in the debugger */
char*
sgen_find_object_for_ptr (char *ptr)
{
	if (ptr >= nursery_section->data && ptr < nursery_section->end_data) {
		found_obj = NULL;
		sgen_scan_area_with_callback (nursery_section->data, nursery_section->end_data,
				find_object_for_ptr_callback, ptr, TRUE);
		if (found_obj)
			return found_obj;
	}

	found_obj = NULL;
	sgen_los_iterate_objects (find_object_for_ptr_callback, ptr);
	if (found_obj)
		return found_obj;

	/*
	 * Very inefficient, but this is debugging code, supposed to
	 * be called from gdb, so we don't care.
	 */
	found_obj = NULL;
	major_collector.iterate_objects (ITERATE_OBJECTS_SWEEP_ALL, find_object_for_ptr_callback, ptr);
	return found_obj;
}

#ifndef SGEN_WITHOUT_MONO

static int
compare_xrefs (const void *a_ptr, const void *b_ptr)
{
	const MonoGCBridgeXRef *a = a_ptr;
	const MonoGCBridgeXRef *b = b_ptr;

	if (a->src_scc_index < b->src_scc_index)
		return -1;
	if (a->src_scc_index > b->src_scc_index)
		return 1;

	if (a->dst_scc_index < b->dst_scc_index)
		return -1;
	if (a->dst_scc_index > b->dst_scc_index)
		return 1;

	return 0;
}

/*
static void
dump_processor_state (SgenBridgeProcessor *p)
{
	int i;

	printf ("------\n");
	printf ("SCCS %d\n", p->num_sccs);
	for (i = 0; i < p->num_sccs; ++i) {
		int j;
		MonoGCBridgeSCC *scc = p->api_sccs [i];
		printf ("\tSCC %d:", i);
		for (j = 0; j < scc->num_objs; ++j) {
			MonoObject *obj = scc->objs [j];
			printf (" %p", obj);
		}
		printf ("\n");
	}

	printf ("XREFS %d\n", p->num_xrefs);
	for (i = 0; i < p->num_xrefs; ++i)
		printf ("\t%d -> %d\n", p->api_xrefs [i].src_scc_index, p->api_xrefs [i].dst_scc_index);

	printf ("-------\n");
}
*/

gboolean
sgen_compare_bridge_processor_results (SgenBridgeProcessor *a, SgenBridgeProcessor *b)
{
	int i;
	SgenHashTable obj_to_a_scc = SGEN_HASH_TABLE_INIT (INTERNAL_MEM_BRIDGE_DEBUG, INTERNAL_MEM_BRIDGE_DEBUG, sizeof (int), mono_aligned_addr_hash, NULL);
	SgenHashTable b_scc_to_a_scc = SGEN_HASH_TABLE_INIT (INTERNAL_MEM_BRIDGE_DEBUG, INTERNAL_MEM_BRIDGE_DEBUG, sizeof (int), g_direct_hash, NULL);
	MonoGCBridgeXRef *a_xrefs, *b_xrefs;
	size_t xrefs_alloc_size;

	// dump_processor_state (a);
	// dump_processor_state (b);

	if (a->num_sccs != b->num_sccs)
		g_error ("SCCS count expected %d but got %d", a->num_sccs, b->num_sccs);
	if (a->num_xrefs != b->num_xrefs)
		g_error ("SCCS count expected %d but got %d", a->num_xrefs, b->num_xrefs);

	/*
	 * First we build a hash of each object in `a` to its respective SCC index within
	 * `a`.  Along the way we also assert that no object is more than one SCC.
	 */
	for (i = 0; i < a->num_sccs; ++i) {
		int j;
		MonoGCBridgeSCC *scc = a->api_sccs [i];

		g_assert (scc->num_objs > 0);

		for (j = 0; j < scc->num_objs; ++j) {
			GCObject *obj = scc->objs [j];
			gboolean new_entry = sgen_hash_table_replace (&obj_to_a_scc, obj, &i, NULL);
			g_assert (new_entry);
		}
	}

	/*
	 * Now we check whether each of the objects in `b` are in `a`, and whether the SCCs
	 * of `b` contain the same sets of objects as those of `a`.
	 *
	 * While we're doing this, build a hash table to map from `b` SCC indexes to `a` SCC
	 * indexes.
	 */
	for (i = 0; i < b->num_sccs; ++i) {
		MonoGCBridgeSCC *scc = b->api_sccs [i];
		MonoGCBridgeSCC *a_scc;
		int *a_scc_index_ptr;
		int a_scc_index;
		int j;
		gboolean new_entry;

		g_assert (scc->num_objs > 0);
		a_scc_index_ptr = sgen_hash_table_lookup (&obj_to_a_scc, scc->objs [0]);
		g_assert (a_scc_index_ptr);
		a_scc_index = *a_scc_index_ptr;

		//g_print ("A SCC %d -> B SCC %d\n", a_scc_index, i);

		a_scc = a->api_sccs [a_scc_index];
		g_assert (a_scc->num_objs == scc->num_objs);

		for (j = 1; j < scc->num_objs; ++j) {
			a_scc_index_ptr = sgen_hash_table_lookup (&obj_to_a_scc, scc->objs [j]);
			g_assert (a_scc_index_ptr);
			g_assert (*a_scc_index_ptr == a_scc_index);
		}

		new_entry = sgen_hash_table_replace (&b_scc_to_a_scc, GINT_TO_POINTER (i), &a_scc_index, NULL);
		g_assert (new_entry);
	}

	/*
	 * Finally, check that we have the same xrefs.  We do this by making copies of both
	 * xref arrays, and replacing the SCC indexes in the copy for `b` with the
	 * corresponding indexes in `a`.  Then we sort both arrays and assert that they're
	 * the same.
	 *
	 * At the same time, check that no xref is self-referential and that there are no
	 * duplicate ones.
	 */

	xrefs_alloc_size = a->num_xrefs * sizeof (MonoGCBridgeXRef);
	a_xrefs = sgen_alloc_internal_dynamic (xrefs_alloc_size, INTERNAL_MEM_BRIDGE_DEBUG, TRUE);
	b_xrefs = sgen_alloc_internal_dynamic (xrefs_alloc_size, INTERNAL_MEM_BRIDGE_DEBUG, TRUE);

	memcpy (a_xrefs, a->api_xrefs, xrefs_alloc_size);
	for (i = 0; i < b->num_xrefs; ++i) {
		MonoGCBridgeXRef *xref = &b->api_xrefs [i];
		int *scc_index_ptr;

		g_assert (xref->src_scc_index != xref->dst_scc_index);

		scc_index_ptr = sgen_hash_table_lookup (&b_scc_to_a_scc, GINT_TO_POINTER (xref->src_scc_index));
		g_assert (scc_index_ptr);
		b_xrefs [i].src_scc_index = *scc_index_ptr;

		scc_index_ptr = sgen_hash_table_lookup (&b_scc_to_a_scc, GINT_TO_POINTER (xref->dst_scc_index));
		g_assert (scc_index_ptr);
		b_xrefs [i].dst_scc_index = *scc_index_ptr;
	}

	qsort (a_xrefs, a->num_xrefs, sizeof (MonoGCBridgeXRef), compare_xrefs);
	qsort (b_xrefs, a->num_xrefs, sizeof (MonoGCBridgeXRef), compare_xrefs);

	for (i = 0; i < a->num_xrefs; ++i) {
		g_assert (a_xrefs [i].src_scc_index == b_xrefs [i].src_scc_index);
		g_assert (a_xrefs [i].dst_scc_index == b_xrefs [i].dst_scc_index);
	}

	sgen_hash_table_clean (&obj_to_a_scc);
	sgen_hash_table_clean (&b_scc_to_a_scc);
	sgen_free_internal_dynamic (a_xrefs, xrefs_alloc_size, INTERNAL_MEM_BRIDGE_DEBUG);
	sgen_free_internal_dynamic (b_xrefs, xrefs_alloc_size, INTERNAL_MEM_BRIDGE_DEBUG);

	return TRUE;
}

#endif

#endif /*HAVE_SGEN_GC*/
