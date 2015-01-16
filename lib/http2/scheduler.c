/*
 * Copyright (c) 2015 DeNA Co., Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */
#include "h2o.h"
#include "h2o/http2.h"

static h2o_http2_scheduler_slot_t *get_or_create_slot(h2o_http2_scheduler_node_t *node, uint16_t weight)
{
    h2o_http2_scheduler_slot_t *slot;
    size_t i;

    /* locate the slot */
    for (i = 0; i != node->_list.size; ++i) {
        slot = node->_list.entries[i];
        if (slot->weight == weight) {
            goto Exit;
        } else if (slot->weight < weight) {
            break;
        }
    }
    /* not found, create new slot */
    slot = h2o_mem_alloc(sizeof(*slot));
    slot->weight = weight;
    h2o_linklist_init_anchor(&slot->_all_refs);
    h2o_linklist_init_anchor(&slot->_active_refs);
    h2o_vector_reserve(NULL, (h2o_vector_t *)&node->_list, sizeof(node->_list.entries[0]), node->_list.size + 1);
    memmove(node->_list.entries + i + 1, node->_list.entries + i,
            sizeof(node->_list.entries[0]) * (node->_list.size - i));
    node->_list.entries[i] = slot;
    ++node->_list.size;

Exit:
    return slot;
}

static void incr_active_cnt(h2o_http2_scheduler_node_t *node)
{
    h2o_http2_scheduler_openref_t *ref;

    /* do nothing if node is the root */
    if (node->_parent == NULL)
        return;

    ref = (h2o_http2_scheduler_openref_t*)node;
    if (++ref->_active_cnt != 1)
        return;
    /* just changed to active */
    assert(!h2o_linklist_is_linked(&ref->_active_link));
    h2o_linklist_insert(&ref->super._slot->_active_refs, &ref->_active_link);
    /* delegate the change towards root */
    incr_active_cnt(ref->super._parent);
}

static void decr_active_cnt(h2o_http2_scheduler_node_t *node)
{
    h2o_http2_scheduler_openref_t *ref;

    /* do notnig if node is the root */
    if (node->_parent == NULL)
        return;

    ref = (h2o_http2_scheduler_openref_t*)node;
    if (--ref->_active_cnt != 0)
        return;
    /* just changed to inactive */
    assert(h2o_linklist_is_linked(&ref->_active_link));
    h2o_linklist_unlink(&ref->_active_link);
    /* delegate the change towards root */
    decr_active_cnt(ref->super._parent);
}

static void convert_to_exclusive(h2o_http2_scheduler_node_t *parent, h2o_http2_scheduler_openref_t *added)
{
    size_t slot_index;

    for (slot_index = 0; slot_index != parent->_list.size; ++slot_index) {
        h2o_http2_scheduler_slot_t *slot = parent->_list.entries[slot_index];
        while (!h2o_linklist_is_empty(&slot->_all_refs)) {
            h2o_http2_scheduler_openref_t *child_ref = H2O_STRUCT_FROM_MEMBER(h2o_http2_scheduler_openref_t, _all_link, slot->_all_refs.next);
            if (child_ref == added) {
                /* precond: the added node should exist as the last item within the slot */
                assert(slot->_all_refs.prev == &added->_all_link);
                break;
            }
            h2o_http2_scheduler_rebind(&added->super, child_ref, 0);
        }
    }
}

void h2o_http2_scheduler_open(h2o_http2_scheduler_node_t *parent, h2o_http2_scheduler_openref_t *ref, uint16_t weight, int exclusive)
{
    h2o_http2_scheduler_slot_t *slot = get_or_create_slot(parent, weight);

    *ref = (h2o_http2_scheduler_openref_t){ { parent, slot } };
    h2o_linklist_insert(&slot->_all_refs, &ref->_all_link);

    if (exclusive)
        convert_to_exclusive(parent, ref);
}

void h2o_http2_scheduler_close(h2o_http2_scheduler_node_t *parent, h2o_http2_scheduler_openref_t *ref)
{
    assert(h2o_http2_scheduler_ref_is_open(ref));

    /* move dependents to parent */
    if (ref->super._list.size != 0) {
        size_t slot_index;
        for (slot_index = 0; slot_index != ref->super._list.size; ++slot_index) {
            h2o_http2_scheduler_slot_t *src_slot = ref->super._list.entries[slot_index];
            while (!h2o_linklist_is_empty(&src_slot->_all_refs)) {
                h2o_http2_scheduler_openref_t *child_ref = H2O_STRUCT_FROM_MEMBER(h2o_http2_scheduler_openref_t, _all_link, src_slot->_all_refs.next);
                h2o_http2_scheduler_rebind(parent, child_ref, 0);
            }
        }
    }

    /* detach self */
    h2o_linklist_unlink(&ref->_all_link);
    if (ref->_self_is_active) {
        assert(ref->_active_cnt == 1);
        assert(h2o_linklist_is_linked(&ref->_active_link));
        h2o_linklist_unlink(&ref->_active_link);
        decr_active_cnt(ref->super._parent);
    } else {
        assert(ref->_active_cnt == 0);
        assert(!h2o_linklist_is_linked(&ref->_active_link));
    }
}

void h2o_http2_scheduler_rebind(h2o_http2_scheduler_node_t *parent, h2o_http2_scheduler_openref_t *ref, int exclusive)
{
    h2o_http2_scheduler_slot_t *new_slot;

    assert(h2o_http2_scheduler_ref_is_open(ref));

    /* do nothing if trying to link to the current parent */
    if (parent == ref->super._parent)
        return;

    new_slot = get_or_create_slot(parent, ref->super._slot->weight);
    /* rebind _all_link */
    h2o_linklist_unlink(&ref->_all_link);
    h2o_linklist_insert(&new_slot->_all_refs, &ref->_all_link);
    /* rebind _active_link (as well as adjust active_cnt) */
    if (h2o_linklist_is_linked(&ref->_active_link)) {
        h2o_linklist_unlink(&ref->_active_link);
        h2o_linklist_insert(&new_slot->_active_refs, &ref->_active_link);
        decr_active_cnt(ref->super._parent);
        incr_active_cnt(parent);
    }

    if (exclusive)
        convert_to_exclusive(parent, ref);
}

void h2o_http2_scheduler_dispose(h2o_http2_scheduler_t *scheduler)
{
    if (scheduler->_list.size != 0) {
        size_t i;
        for (i = 0; i != scheduler->_list.size; ++i) {
            h2o_http2_scheduler_slot_t *slot = scheduler->_list.entries[i];
            assert(h2o_linklist_is_empty(&slot->_all_refs));
            free(slot);
        }
        free(scheduler->_list.entries);
    }
}

void h2o_http2_scheduler_set_active(h2o_http2_scheduler_openref_t *ref)
{
    assert(!ref->_self_is_active);
    ref->_self_is_active = 1;
    incr_active_cnt(&ref->super);
}

int h2o_http2_scheduler_iterate(h2o_http2_scheduler_t *scheduler, h2o_http2_scheduler_iterate_cb cb, void *cb_arg)
{
    size_t slot_index;
    int bail_out = 0;

    for (slot_index = 0; slot_index != scheduler->_list.size; ++slot_index) {
        h2o_http2_scheduler_slot_t *slot = scheduler->_list.entries[slot_index];
        /* the flag is used for exitting the loop without reexecuting child nodes */
        h2o_linklist_t *readded_first = scheduler->_parent != NULL ? NULL : (void*)1;
        while (!h2o_linklist_is_empty(&slot->_active_refs) && slot->_active_refs.next != readded_first) {
            h2o_http2_scheduler_openref_t *ref = H2O_STRUCT_FROM_MEMBER(h2o_http2_scheduler_openref_t, _active_link,
                                                                        slot->_active_refs.next);
            if (ref->_self_is_active) {
                /* call the callbacks */
                int still_is_active;
                assert(ref->_active_cnt != 0);
                bail_out = cb(ref, &still_is_active, cb_arg);
                if (still_is_active) {
                    h2o_linklist_unlink(&ref->_active_link);
                    h2o_linklist_insert(&slot->_active_refs, &ref->_active_link);
                    if (readded_first == NULL)
                        readded_first = &ref->_active_link;
                } else {
                    ref->_self_is_active = 0;
                    decr_active_cnt(&ref->super);
                    if (ref->_active_cnt != 0) {
                        /* relink to the end */
                        h2o_linklist_unlink(&ref->_active_link);
                        h2o_linklist_insert(&slot->_active_refs, &ref->_active_link);
                    }
                }
            } else {
                /* run the children */
                h2o_linklist_unlink(&ref->_active_link);
                h2o_linklist_insert(&slot->_active_refs, &ref->_active_link);
                bail_out = h2o_http2_scheduler_iterate(&ref->super, cb, cb_arg);
                if (readded_first == NULL && h2o_linklist_is_linked(&ref->_active_link))
                    readded_first = &ref->_active_link;
            }
            if (bail_out)
                goto Exit;
        }
    }

Exit:
    return bail_out;
}