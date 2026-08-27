#include <windows.h>

#include <stdio.h>
#include <string.h>

#include "../src/history.h"

typedef struct {
    int discarded;
    int staged_discarded;
} DiscardContext;

static void record_discard(GoldenHistoryEntry *entry, void *opaque) {
    DiscardContext *context = (DiscardContext *)opaque;
    ++context->discarded;
    if (entry->staged) ++context->staged_discarded;
}

static GoldenHistoryEntry resource_entry(GoldenHistoryKind kind, int value) {
    GoldenHistoryEntry entry;
    wchar_t source[32], destination[32];
    _snwprintf(source, _countof(source), L"source-%d", value);
    _snwprintf(destination, _countof(destination), L"destination-%d", value);
    golden_history_entry_resource(&entry, kind, source, destination, NULL);
    return entry;
}

static int test_mixed_order_and_transfer(void) {
    static GoldenHistory history;
    golden_history_init(&history, NULL, NULL);
    Annotation annotation = {0};
    wcscpy(annotation.name, L"before");
    GoldenHistoryEntry annotations;
    if (!golden_history_entry_annotations(&annotations, &annotation, 1, 0)) return 1;
    GoldenHistoryEntry directory = resource_entry(GOLDEN_HISTORY_CREATE_DIRECTORY, 1);
    GoldenHistoryEntry png = resource_entry(GOLDEN_HISTORY_MOVE_PNG, 2);
    golden_history_push_new(&history, &annotations);
    golden_history_push_new(&history, &directory);
    golden_history_push_new(&history, &png);

    GoldenHistoryEntry popped = {0};
    int failed = !golden_history_pop_undo(&history, &popped) ||
        popped.kind != GOLDEN_HISTORY_MOVE_PNG;
    golden_history_transfer_to_redo(&history, &popped);
    if (!failed) failed = !golden_history_pop_undo(&history, &popped) ||
        popped.kind != GOLDEN_HISTORY_CREATE_DIRECTORY;
    golden_history_transfer_to_redo(&history, &popped);
    if (!failed) failed = !golden_history_pop_redo(&history, &popped) ||
        popped.kind != GOLDEN_HISTORY_CREATE_DIRECTORY;
    golden_history_transfer_to_undo(&history, &popped);
    golden_history_destroy(&history);
    return failed;
}

static int test_branch_clears_redo(void) {
    DiscardContext discarded = {0};
    static GoldenHistory history;
    golden_history_init(&history, record_discard, &discarded);
    GoldenHistoryEntry first = resource_entry(GOLDEN_HISTORY_MOVE_PNG, 1);
    golden_history_push_new(&history, &first);
    GoldenHistoryEntry popped = {0};
    if (!golden_history_pop_undo(&history, &popped)) return 1;
    popped.staged = TRUE;
    golden_history_transfer_to_redo(&history, &popped);
    GoldenHistoryEntry branch = resource_entry(GOLDEN_HISTORY_CREATE_DIRECTORY, 2);
    golden_history_push_new(&history, &branch);
    int failed = golden_history_can_redo(&history) || discarded.discarded != 1 ||
                 discarded.staged_discarded != 1;
    golden_history_destroy(&history);
    return failed;
}

static int test_capacity_evicts_oldest(void) {
    DiscardContext discarded = {0};
    static GoldenHistory history;
    golden_history_init(&history, record_discard, &discarded);
    for (int i = 0; i < GOLDEN_HISTORY_LIMIT + 3; ++i) {
        GoldenHistoryEntry entry = resource_entry(GOLDEN_HISTORY_MOVE_DIRECTORY, i);
        golden_history_push_new(&history, &entry);
    }
    GoldenHistoryEntry popped = {0};
    int failed = history.undo.count != GOLDEN_HISTORY_LIMIT || discarded.discarded != 3;
    for (int expected = GOLDEN_HISTORY_LIMIT + 2; !failed && expected >= 3; --expected) {
        if (!golden_history_pop_undo(&history, &popped)) { failed = 1; break; }
        wchar_t expected_source[32];
        _snwprintf(expected_source, _countof(expected_source), L"source-%d", expected);
        failed = wcscmp(popped.source, expected_source) != 0;
        golden_history_entry_dispose(&popped);
    }
    golden_history_destroy(&history);
    return failed;
}

static int test_failed_operation_can_be_restored(void) {
    static GoldenHistory history;
    golden_history_init(&history, NULL, NULL);
    GoldenHistoryEntry original = resource_entry(GOLDEN_HISTORY_MOVE_PNG, 7);
    golden_history_push_new(&history, &original);
    GoldenHistoryEntry attempt = {0};
    int failed = !golden_history_pop_undo(&history, &attempt) ||
                 golden_history_can_undo(&history);
    golden_history_restore_undo(&history, &attempt);
    if (!failed) failed = !golden_history_pop_undo(&history, &attempt) ||
        wcscmp(attempt.source, L"source-7");
    golden_history_entry_dispose(&attempt);
    golden_history_destroy(&history);
    return failed;
}

static int test_annotation_ownership_and_filter(void) {
    static GoldenHistory history;
    golden_history_init(&history, NULL, NULL);
    Annotation value = {0};
    wcscpy(value.name, L"owned-copy");
    GoldenHistoryEntry annotation;
    if (!golden_history_entry_annotations(&annotation, &value, 1, 0)) return 1;
    golden_history_push_new(&history, &annotation);
    wcscpy(value.name, L"mutated-source");
    GoldenHistoryEntry resource = resource_entry(GOLDEN_HISTORY_CREATE_DIRECTORY, 4);
    golden_history_push_new(&history, &resource);
    golden_history_remove_annotations(&history);
    GoldenHistoryEntry popped = {0};
    int failed = history.undo.count != 1 ||
        !golden_history_pop_undo(&history, &popped) ||
        popped.kind != GOLDEN_HISTORY_CREATE_DIRECTORY;
    golden_history_entry_dispose(&popped);
    golden_history_destroy(&history);
    return failed;
}

static int test_saved_annotation_comparison(void) {
    Annotation saved[2] = {0};
    Annotation current[2] = {0};
    wcscpy(saved[0].name, L"box");
    saved[0].boundary = (RECT){1, 2, 30, 40};
    memcpy(current, saved, sizeof(saved));
    if (!golden_history_annotations_equal(current, 1, saved, 1)) return 1;
    current[0].has_click = TRUE;
    if (golden_history_annotations_equal(current, 1, saved, 1)) return 1;
    current[0] = saved[0];
    return golden_history_annotations_equal(current, 2, saved, 1);
}

int main(void) {
    if (test_mixed_order_and_transfer() ||
        test_branch_clears_redo() ||
        test_capacity_evicts_oldest() ||
        test_failed_operation_can_be_restored() ||
        test_annotation_ownership_and_filter() ||
        test_saved_annotation_comparison()) return 1;
    puts("All Goldens history tests passed.");
    return 0;
}
