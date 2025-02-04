#ifndef XHPROF_TRACE_H
#define XHPROF_TRACE_H

static zend_always_inline void hp_mode_common_beginfn(hp_entry_t **entries, hp_entry_t *current)
{
    hp_entry_t *p;

    /* This symbol's recursive level */
    int recurse_level = 0;

    if (XHPROF_G(func_hash_counters[current->hash_code]) > 0) {
        /* Find this symbols recurse level */
        for (p = (*entries); p; p = p->prev_hprof) {
            if (zend_string_equals(current->name_hprof, p->name_hprof)) {
                recurse_level = (p->rlvl_hprof) + 1;
                break;
            }
        }
    }

    XHPROF_G(func_hash_counters[current->hash_code])++;

    /* Init current function's recurse level */
    current->rlvl_hprof = recurse_level;
}

static zend_always_inline int hp_ignored_functions_filter_collision(hp_ignored_functions *functions, zend_ulong hash)
{
    zend_ulong idx = hash % XHPROF_MAX_IGNORED_FUNCTIONS;
    return functions->filter[idx];
}

static zend_always_inline int hp_ignore_entry_work(zend_ulong hash_code, zend_string *curr_func)
{
    if (XHPROF_G(ignored_functions) == NULL) {
        return 0;
    }

    hp_ignored_functions *functions = XHPROF_G(ignored_functions);

    if (hp_ignored_functions_filter_collision(functions, hash_code)) {
        int i = 0;
        for (; functions->names[i] != NULL; i++) {
            zend_string *name = functions->names[i];
            if (zend_string_equals(curr_func, name)) {
                return 1;
            }
        }
    }

    return 0;
}

static zend_always_inline zend_string *hp_get_function_name(zend_execute_data *execute_data)
{
    zend_function *curr_func;
    zend_string *real_function_name;

    if (!execute_data) {
        return NULL;
    }

    curr_func = execute_data->func;

    if (!curr_func->common.function_name) {
        return NULL;
    }

    if (curr_func->common.scope != NULL) {
        real_function_name = strpprintf(0, "%s::%s", curr_func->common.scope->name->val, ZSTR_VAL(curr_func->common.function_name));
    } else {
        real_function_name = zend_string_copy(curr_func->common.function_name);
    }

    return real_function_name;
}

// ---

static zend_always_inline uint64_t hp_make_composite_key(
    uint32_t parent_id,
    uint32_t child_id,
    uint16_t recursion_level
) {
    return ((uint64_t)parent_id << 32) |
           ((uint64_t)child_id << 16) |
           recursion_level;
}

static zend_always_inline hp_composite_key hp_decompose_key(uint64_t key) {
    return (hp_composite_key){
        .parent_id = (uint32_t)(key >> 32),
        .child_id = (uint32_t)((key >> 16) & 0xFFFF),
        .recursion_level = (uint16_t)(key & 0xFFFF)
    };
}

static zend_always_inline uint32_t hp_get_function_id(zend_string *function_name)
{
    zval *id_zv;

    if ((id_zv = zend_hash_find(XHPROF_G(function_map), function_name)) != NULL) {
        return Z_LVAL_P(id_zv);
    }

    if (XHPROF_G(function_counter) >= XHPROF_MAX_FUNCTIONS) {
        return 0;
    }

    uint32_t func_id = ++XHPROF_G(function_counter);

    zval id_val;
    ZVAL_LONG(&id_val, func_id);
    zend_hash_add(XHPROF_G(function_map), function_name, &id_val);

    zend_string_addref(function_name);
    zend_hash_index_add_ptr(XHPROF_G(function_map), func_id, function_name);

    return func_id;
}

static zend_always_inline hp_stat_entry* hp_stats_find_or_add(uint64_t key)
{
    hp_stat_vector *arr = &XHPROF_G(stats_array);
    zval *index_zv;

    if ((index_zv = zend_hash_index_find(arr->index_map, key)) != NULL) {
        return &arr->entries[Z_LVAL_P(index_zv)];
    }

    if (arr->count >= arr->capacity) {
        size_t new_size = arr->capacity * 2;
        hp_stat_entry *new_entries = realloc(arr->entries,
                                             sizeof(hp_stat_entry) * new_size);
        if (!new_entries) {
            return NULL;
        }
        arr->entries = new_entries;
        arr->capacity = new_size;
    }

    size_t index = arr->count++;
    hp_stat_entry *entry = &arr->entries[index];
    entry->key = key;
    entry->wt = 0;
    entry->ct = 0;
    entry->cpu = 0;
    entry->mu = 0;
    entry->pmu = 0;

    zval index_val;
    ZVAL_LONG(&index_val, index);
    zend_hash_index_add(arr->index_map, key, &index_val);

    return entry;
}

static void hp_init_stats_array()
{
    XHPROF_G(stats_array).capacity = XHPROF_INITIAL_STATS_SIZE;
    XHPROF_G(stats_array).count = 0;
    XHPROF_G(stats_array).entries = malloc(sizeof(hp_stat_entry) *
                                        XHPROF_INITIAL_STATS_SIZE);

    ALLOC_HASHTABLE(XHPROF_G(stats_array).index_map);
    zend_hash_init(XHPROF_G(stats_array).index_map, 1024, NULL, NULL, 0);
}

static void hp_cleanup_stats_array()
{
    if (XHPROF_G(stats_array).entries) {
        free(XHPROF_G(stats_array).entries);
        XHPROF_G(stats_array).entries = NULL;
    }

    if (XHPROF_G(stats_array).index_map) {
        zend_hash_destroy(XHPROF_G(stats_array).index_map);
        FREE_HASHTABLE(XHPROF_G(stats_array).index_map);
        XHPROF_G(stats_array).index_map = NULL;
    }
}

// ---

static zend_always_inline zend_string *hp_get_trace_callback(zend_string *function_name, zend_execute_data *data)
{
    zend_string *trace_name;
    hp_trace_callback *callback;

    if (XHPROF_G(trace_callbacks)) {
        callback = (hp_trace_callback*)zend_hash_find_ptr(XHPROF_G(trace_callbacks), function_name);
        if (callback) {
            trace_name = (*callback)(function_name, data);
        } else {
            return function_name;
        }
    } else {
        return function_name;
    }

    zend_string_release(function_name);

    return trace_name;
}

static zend_always_inline hp_entry_t *hp_fast_alloc_hprof_entry()
{
    hp_entry_t *p;

    p = XHPROF_G(entry_free_list);

    if (p) {
        XHPROF_G(entry_free_list) = p->prev_hprof;
        return p;
    } else {
        return (hp_entry_t *)malloc(sizeof(hp_entry_t));
    }
}

static zend_always_inline void hp_fast_free_hprof_entry(hp_entry_t *p)
{
    if (p->name_hprof != NULL) {
        zend_string_release(p->name_hprof);
    }

    /* we use/overload the prev_hprof field in the structure to link entries in
     * the free list.
     * */
    p->prev_hprof = XHPROF_G(entry_free_list);
    XHPROF_G(entry_free_list) = p;
}

static zend_always_inline int begin_profiling(zend_string *root_symbol, zend_execute_data *execute_data)
{
    zend_string *function_name;
    hp_entry_t **entries = &XHPROF_G(entries);

    if (root_symbol == NULL) {
        function_name = hp_get_function_name(execute_data);
    } else {
        function_name = zend_string_copy(root_symbol);
    }

    if (function_name == NULL) {
        return 0;
    }

    zend_ulong hash_code = ZSTR_HASH(function_name);
    int profile_curr = !hp_ignore_entry_work(hash_code, function_name);
    if (profile_curr) {
        if (execute_data != NULL) {
            function_name = hp_get_trace_callback(function_name, execute_data);
        }

        uint32_t function_id = hp_get_function_id(function_name);

        hp_entry_t *cur_entry = hp_fast_alloc_hprof_entry();
        (cur_entry)->hash_code = hash_code % XHPROF_FUNC_HASH_COUNTERS_SIZE;
        (cur_entry)->name_hprof = function_name;
        (cur_entry)->func_id = function_id;
        (cur_entry)->prev_hprof = (*(entries));
#if PHP_VERSION_ID >= 80000
        (cur_entry)->is_trace = 1;
#endif
        /* Call the universal callback */
        hp_mode_common_beginfn((entries), (cur_entry));
        /* Call the mode's beginfn callback */
        XHPROF_G(mode_cb).begin_fn_cb((entries), (cur_entry));
        /* Update entries linked list */
        (*(entries)) = (cur_entry);
    } else {
#if PHP_VERSION_ID >= 80000
        hp_entry_t *cur_entry = hp_fast_alloc_hprof_entry();
        (cur_entry)->name_hprof = zend_string_copy((*(entries))->name_hprof);
        (cur_entry)->prev_hprof = (*(entries));
        (cur_entry)->is_trace = 0;
        (*(entries)) = (cur_entry);
#endif
        zend_string_release(function_name);
    }

    return profile_curr;
}

static zend_always_inline void end_profiling()
{
    hp_entry_t *cur_entry;
    hp_entry_t **entries = &XHPROF_G(entries);

    /* Call the mode's endfn callback. */
    /* NOTE(cjiang): we want to call this 'end_fn_cb' before */
    /* 'hp_mode_common_endfn' to avoid including the time in */
    /* 'hp_mode_common_endfn' in the profiling results.      */
    XHPROF_G(mode_cb).end_fn_cb(entries);
    cur_entry = (*(entries));
    /* Free top entry and update entries linked list */
    (*(entries)) = (*(entries))->prev_hprof;
    hp_fast_free_hprof_entry(cur_entry);
}
#endif
